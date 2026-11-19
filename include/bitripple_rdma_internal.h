#ifndef BITRIPPLE_RDMA_INTERNAL_H
#define BITRIPPLE_RDMA_INTERNAL_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#define BR_RDMA_RECV_BUF_COUNT  32
#define BR_RDMA_MAX_MSG_SIZE   131072

typedef struct {
    void           *buf;
    struct ibv_mr  *mr;
    uint32_t       len;
} br_rdma_recv_buf_t;

typedef struct {
    struct rdma_cm_id   *cm_id;
    struct ibv_pd       *pd;
    struct ibv_cq       *cq;
    struct ibv_qp       *qp;
    struct ibv_mr       *send_mr;
    br_rdma_recv_buf_t   recv_bufs[BR_RDMA_RECV_BUF_COUNT];
    void                *send_buf;
    size_t               send_buf_size;
    uint32_t             next_recv_idx;
    size_t               buf_size;
    bool                 connected;
    bool                 active;
    char                 dev_name[32];
} br_rdma_path_t;

typedef struct {
    struct rdma_event_channel *ec;
    struct rdma_cm_id         *listen_id;
    br_rdma_path_t            paths[BR_MAX_PATHS];
    uint32_t                  n_paths;
    uint32_t                  max_paths;
    uint32_t                  max_wr;
    pthread_mutex_t           mutex;
    pthread_cond_t            conn_cond;
    int                       num_accepted;
} br_rdma_ctx_t;

int  br_rdma_init_path(br_rdma_path_t *path, const char *dev_name, 
                      int port_num, size_t buf_size);
void br_rdma_cleanup_path(br_rdma_path_t *path);
int  br_rdma_connect_client(br_rdma_path_t *path, const char *server_ip, 
                            int server_port);
int  br_rdma_accept_server(br_rdma_path_t *path, int listen_port);
int  br_rdma_accept_multi(br_rdma_ctx_t *ctx, int listen_port, int max_paths);
int  br_rdma_post_recv(br_rdma_path_t *path, uint32_t idx);
int  br_rdma_post_all_recv(br_rdma_path_t *path);
int  br_rdma_post_send(br_rdma_path_t *path, const void *data, size_t len);
int  br_rdma_poll_cq(br_rdma_path_t *path, int timeout_ms, struct ibv_wc *wc);
int  br_rdma_poll_all_cqs(br_rdma_ctx_t *ctx, int timeout_ms, struct ibv_wc *wc,
                          uint32_t *out_path_idx);

#endif