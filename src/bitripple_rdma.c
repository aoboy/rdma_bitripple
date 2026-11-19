#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "bitripple.h"
#include "bitripple_rdma_internal.h"

#define BR_RDMA_MAX_POLL 2000

int br_rdma_init_path(br_rdma_path_t *path, const char *dev_name,
                      int port_num, size_t buf_size) {
    (void)port_num;
    memset(path, 0, sizeof(*path));
    strncpy(path->dev_name, dev_name, sizeof(path->dev_name) - 1);
    path->buf_size = buf_size;
    path->send_buf_size = BR_RDMA_MAX_MSG_SIZE;
    path->next_recv_idx = 0;
    path->active = false;
    path->connected = false;
    return BR_OK;
}

void br_rdma_cleanup_path(br_rdma_path_t *path) {
    if (!path) return;

    if (path->send_mr) ibv_dereg_mr(path->send_mr);
    free(path->send_buf);

    for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
        if (path->recv_bufs[i].mr) ibv_dereg_mr(path->recv_bufs[i].mr);
        free(path->recv_bufs[i].buf);
    }

    if (path->qp) ibv_destroy_qp(path->qp);
    if (path->cq) ibv_destroy_cq(path->cq);
    if (path->pd) ibv_dealloc_pd(path->pd);
    if (path->cm_id) rdma_destroy_id(path->cm_id);

    memset(path, 0, sizeof(*path));
}

int br_rdma_post_recv(br_rdma_path_t *path, uint32_t idx) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr, *bad_wr;

    sge.addr = (uint64_t)(uintptr_t)path->recv_bufs[idx].buf;
    sge.length = path->recv_bufs[idx].len;
    sge.lkey = path->recv_bufs[idx].mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = idx;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    return (ibv_post_recv(path->qp, &wr, &bad_wr) == 0) ? BR_OK : BR_ERR_NET;
}

int br_rdma_post_all_recv(br_rdma_path_t *path) {
    for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
        if (br_rdma_post_recv(path, i) != BR_OK) return BR_ERR_NET;
    }
    return BR_OK;
}

int br_rdma_post_send(br_rdma_path_t *path, const void *data, size_t len) {
    if (len > path->send_buf_size) return BR_ERR_INVAL;

    memcpy(path->send_buf, data, len);

    struct ibv_sge sge;
    sge.addr = (uint64_t)(uintptr_t)path->send_buf;
    sge.length = len;
    sge.lkey = path->send_mr->lkey;

    struct ibv_send_wr wr = {0};
    wr.wr_id = 0;
    wr.next = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    struct ibv_send_wr *bad_wr;
    int ret = ibv_post_send(path->qp, &wr, &bad_wr);
    
    // Poll for completion immediately to reduce latency
    if (ret == 0) {
        struct ibv_wc wc;
        int64_t timeout = 1000; // 1ms timeout
        while (timeout > 0) {
            int ne = ibv_poll_cq(path->cq, 1, &wc);
            if (ne > 0 && wc.wr_id == 0) {
                if (wc.status != IBV_WC_SUCCESS) {
                    return BR_ERR_NET;
                }
                break;
            }
            if (ne < 0) {
                return BR_ERR_NET;
            }
            usleep(10);
            timeout--;
        }
    }
    
    return (ret == 0) ? BR_OK : BR_ERR_NET;
}

int br_rdma_poll_cq(br_rdma_path_t *path, int timeout_ms, struct ibv_wc *wc) {
    int64_t remain = timeout_ms;
    while (remain > 0) {
        int ne = ibv_poll_cq(path->cq, 1, wc);
        if (ne > 0) return ne;
        if (ne < 0) return BR_ERR_NET;
        usleep(100);
        remain--;
    }
    return 0;
}

int br_rdma_poll_all_cqs(br_rdma_ctx_t *ctx, int timeout_ms, struct ibv_wc *wc,
                         uint32_t *out_path_idx) {
    int64_t remain = timeout_ms;
    while (remain > 0) {
        for (uint32_t i = 0; i < ctx->n_paths; i++) {
            if (!ctx->paths[i].active) continue;
            int ne = ibv_poll_cq(ctx->paths[i].cq, 1, wc);
            if (ne > 0) {
                *out_path_idx = i;
                return ne;
            }
        }
        usleep(100);
        remain--;
    }
    return 0;
}

/* Client: connect to server via rdma_cm */
int br_rdma_connect_client(br_rdma_path_t *path, const char *server_ip,
                           int server_port) {
    struct rdma_addrinfo hints, *res;
    struct ibv_qp_init_attr qp_attr;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_port_space = RDMA_PS_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", server_port);

    ret = rdma_getaddrinfo((char*)server_ip, port_str, &hints, &res);
    if (ret) return BR_ERR_NET;

    struct rdma_event_channel *ec = rdma_create_event_channel();
    if (!ec) { rdma_freeaddrinfo(res); return BR_ERR_NET; }

    ret = rdma_create_id(ec, &path->cm_id, NULL, RDMA_PS_TCP);
    if (ret) { rdma_destroy_event_channel(ec); rdma_freeaddrinfo(res); return BR_ERR_NET; }

    ret = rdma_resolve_addr(path->cm_id, NULL, res->ai_dst_addr, 2000);
    rdma_freeaddrinfo(res);
    if (ret) return BR_ERR_NET;

    struct rdma_cm_event *event;
    ret = rdma_get_cm_event(ec, &event);
    if (ret || event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        if (event) rdma_ack_cm_event(event);
        return BR_ERR_NET;
    }
    rdma_ack_cm_event(event);

    ret = rdma_resolve_route(path->cm_id, 2000);
    if (ret) return BR_ERR_NET;

    ret = rdma_get_cm_event(ec, &event);
    if (ret || event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        if (event) rdma_ack_cm_event(event);
        return BR_ERR_NET;
    }
    rdma_ack_cm_event(event);

    /* Create resources */
    path->pd = ibv_alloc_pd(path->cm_id->verbs);
    if (!path->pd) return BR_ERR_NOMEM;

    path->cq = ibv_create_cq(path->cm_id->verbs, 256, NULL, NULL, 0);
    if (!path->cq) return BR_ERR_NOMEM;

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.send_cq = path->cq;
    qp_attr.recv_cq = path->cq;
    qp_attr.cap.max_send_wr = 512;
    qp_attr.cap.max_recv_wr = 512;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 64;

    ret = rdma_create_qp(path->cm_id, path->pd, &qp_attr);
    if (ret) return BR_ERR_NET;
    path->qp = path->cm_id->qp;

    /* Allocate buffers */
    path->send_buf = malloc(BR_RDMA_MAX_MSG_SIZE);
    path->send_buf_size = BR_RDMA_MAX_MSG_SIZE;
    if (!path->send_buf) return BR_ERR_NOMEM;

    path->send_mr = ibv_reg_mr(path->pd, path->send_buf, path->send_buf_size,
                               IBV_ACCESS_LOCAL_WRITE);
    if (!path->send_mr) return BR_ERR_NOMEM;

    for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
        path->recv_bufs[i].buf = malloc(path->buf_size);
        path->recv_bufs[i].len = path->buf_size;
        if (!path->recv_bufs[i].buf) return BR_ERR_NOMEM;

        path->recv_bufs[i].mr = ibv_reg_mr(path->pd, path->recv_bufs[i].buf,
                                           path->recv_bufs[i].len,
                                           IBV_ACCESS_LOCAL_WRITE);
        if (!path->recv_bufs[i].mr) return BR_ERR_NOMEM;
    }

    /* Post recv buffers */
    if (br_rdma_post_all_recv(path) != BR_OK) return BR_ERR_NET;

    /* Connect */
    struct rdma_conn_param conn_param = {0};
    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    conn_param.rnr_retry_count = 7;

    ret = rdma_connect(path->cm_id, &conn_param);
    if (ret) return BR_ERR_NET;

    ret = rdma_get_cm_event(ec, &event);
    if (ret || event->event != RDMA_CM_EVENT_ESTABLISHED) {
        if (event) rdma_ack_cm_event(event);
        return BR_ERR_NET;
    }
    rdma_ack_cm_event(event);

    path->connected = true;
    path->active = true;
    rdma_destroy_event_channel(ec);
    return BR_OK;
}

/* Server: accept multiple connections */
int br_rdma_accept_multi(br_rdma_ctx_t *ctx, int listen_port, int max_paths) {
    struct rdma_addrinfo hints, *res;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = RAI_PASSIVE;
    hints.ai_port_space = RDMA_PS_TCP;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", listen_port);

    ret = rdma_getaddrinfo(NULL, port_str, &hints, &res);
    if (ret) return BR_ERR_NET;

    ctx->ec = rdma_create_event_channel();
    if (!ctx->ec) { 
        rdma_freeaddrinfo(res); 
        return BR_ERR_NET; 
    }

    struct rdma_cm_id *listen_id;
    ret = rdma_create_id(ctx->ec, &listen_id, NULL, RDMA_PS_TCP);
    if (ret) { 
        rdma_destroy_event_channel(ctx->ec); 
        rdma_freeaddrinfo(res); 
        return BR_ERR_NET; 
    }

    ret = rdma_bind_addr(listen_id, res->ai_src_addr);
    rdma_freeaddrinfo(res);
    if (ret) return BR_ERR_NET;

    ret = rdma_listen(listen_id, max_paths);
    if (ret) return BR_ERR_NET;

    ctx->paths[0].cm_id = listen_id;
    ctx->max_paths = max_paths;
    ctx->n_paths = 0;
    ctx->num_accepted = 0;
    pthread_mutex_init(&ctx->mutex, NULL);
    pthread_cond_init(&ctx->conn_cond, NULL);

    int accepted = 0;
    while (accepted < max_paths) {
        struct rdma_cm_event *event;
        ret = rdma_get_cm_event(ctx->ec, &event);
        if (ret) break;

        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            struct rdma_cm_id *conn_id = event->id;
            rdma_ack_cm_event(event);

            br_rdma_path_t *path = &ctx->paths[accepted];

            path->buf_size = 4096;
            path->send_buf = malloc(BR_RDMA_MAX_MSG_SIZE);
            path->send_buf_size = BR_RDMA_MAX_MSG_SIZE;
            if (!path->send_buf) {
                // Clean up previously allocated resources
                for (int i = 0; i < accepted; i++) {
                    br_rdma_cleanup_path(&ctx->paths[i]);
                }
                break;
            }

            // Allocate receive buffers
            for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
                path->recv_bufs[i].buf = malloc(path->buf_size);
                path->recv_bufs[i].len = path->buf_size;
                if (!path->recv_bufs[i].buf) {
                    // Clean up previously allocated resources
                    free(path->send_buf);
                    for (uint32_t j = 0; j <= i; j++) {
                        free(path->recv_bufs[j].buf);
                    }
                    for (int k = 0; k < accepted; k++) {
                        br_rdma_cleanup_path(&ctx->paths[k]);
                    }
                    break;
                }
            }

            // Allocate protection domain
            path->pd = ibv_alloc_pd(conn_id->verbs);
            if (!path->pd) {
                // Clean up previously allocated resources
                free(path->send_buf);
                for (uint32_t j = 0; j < BR_RDMA_RECV_BUF_COUNT; j++) {
                    free(path->recv_bufs[j].buf);
                }
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }

            // Create completion queue
            path->cq = ibv_create_cq(conn_id->verbs, 256, NULL, NULL, 0);
            if (!path->cq) {
                // Clean up previously allocated resources
                ibv_dealloc_pd(path->pd);
                free(path->send_buf);
                for (uint32_t j = 0; j < BR_RDMA_RECV_BUF_COUNT; j++) {
                    free(path->recv_bufs[j].buf);
                }
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }

            // Create queue pair
            struct ibv_qp_init_attr qp_attr = {0};
            qp_attr.qp_type = IBV_QPT_RC;
            qp_attr.send_cq = path->cq;
            qp_attr.recv_cq = path->cq;
            qp_attr.cap.max_send_wr = 256;
            qp_attr.cap.max_recv_wr = 256;
            qp_attr.cap.max_send_sge = 1;
            qp_attr.cap.max_recv_sge = 1;

            ret = rdma_create_qp(conn_id, path->pd, &qp_attr);
            if (ret) {
                // Clean up previously allocated resources
                ibv_destroy_cq(path->cq);
                ibv_dealloc_pd(path->pd);
                free(path->send_buf);
                for (uint32_t j = 0; j < BR_RDMA_RECV_BUF_COUNT; j++) {
                    free(path->recv_bufs[j].buf);
                }
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }
            path->qp = conn_id->qp;
            path->cm_id = conn_id;

            // Register memory regions
            path->send_mr = ibv_reg_mr(path->pd, path->send_buf, path->send_buf_size,
                                     IBV_ACCESS_LOCAL_WRITE);
            if (!path->send_mr) {
                // Clean up previously allocated resources
                ibv_destroy_qp(path->qp);
                ibv_destroy_cq(path->cq);
                ibv_dealloc_pd(path->pd);
                free(path->send_buf);
                for (uint32_t j = 0; j < BR_RDMA_RECV_BUF_COUNT; j++) {
                    free(path->recv_bufs[j].buf);
                }
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }

            for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
                path->recv_bufs[i].mr = ibv_reg_mr(path->pd, path->recv_bufs[i].buf,
                                                   path->recv_bufs[i].len,
                                                   IBV_ACCESS_LOCAL_WRITE);
                if (!path->recv_bufs[i].mr) {
                    // Clean up previously allocated resources
                    ibv_dereg_mr(path->send_mr);
                    ibv_destroy_qp(path->qp);
                    ibv_destroy_cq(path->cq);
                    ibv_dealloc_pd(path->pd);
                    free(path->send_buf);
                    for (uint32_t j = 0; j <= i; j++) {
                        ibv_dereg_mr(path->recv_bufs[j].mr);
                        free(path->recv_bufs[j].buf);
                    }
                    for (uint32_t j = i+1; j < BR_RDMA_RECV_BUF_COUNT; j++) {
                        free(path->recv_bufs[j].buf);
                    }
                    for (int k = 0; k < accepted; k++) {
                        br_rdma_cleanup_path(&ctx->paths[k]);
                    }
                    break;
                }
            }

            // Post receive buffers
            br_rdma_post_all_recv(path);

            // Accept connection
            struct rdma_conn_param conn_param = {0};
            conn_param.responder_resources = 1;
            conn_param.initiator_depth = 1;

            ret = rdma_accept(conn_id, &conn_param);
            if (ret) {
                // Clean up previously allocated resources
                for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
                    ibv_dereg_mr(path->recv_bufs[i].mr);
                    free(path->recv_bufs[i].buf);
                }
                ibv_dereg_mr(path->send_mr);
                ibv_destroy_qp(path->qp);
                ibv_destroy_cq(path->cq);
                ibv_dealloc_pd(path->pd);
                free(path->send_buf);
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }

            event = NULL;
            ret = rdma_get_cm_event(ctx->ec, &event);
            if (ret || !event || event->event != RDMA_CM_EVENT_ESTABLISHED) {
                if (event) rdma_ack_cm_event(event);
                // Clean up previously allocated resources
                for (uint32_t i = 0; i < BR_RDMA_RECV_BUF_COUNT; i++) {
                    ibv_dereg_mr(path->recv_bufs[i].mr);
                    free(path->recv_bufs[i].buf);
                }
                ibv_dereg_mr(path->send_mr);
                ibv_destroy_qp(path->qp);
                ibv_destroy_cq(path->cq);
                ibv_dealloc_pd(path->pd);
                free(path->send_buf);
                for (int k = 0; k < accepted; k++) {
                    br_rdma_cleanup_path(&ctx->paths[k]);
                }
                break;
            }
            rdma_ack_cm_event(event);

            path->connected = true;
            path->active = true;
            accepted++;
            ctx->num_accepted++;
        } else {
            rdma_ack_cm_event(event);
        }
    }

    ctx->n_paths = accepted;
    rdma_destroy_id(listen_id);
    return (accepted > 0) ? BR_OK : BR_ERR_NET;
}

