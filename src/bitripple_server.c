#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bitripple.h"
#include "bitripple_rdma_internal.h"

struct br_server {
    br_rdma_ctx_t     rdma;
    br_decoder_t*     dec;
    size_t            expected_len;
    uint16_t          symbol_size;
    uint16_t          port;
    uint32_t          path_seq_next[BR_MAX_PATHS];
    br_path_stats_t   stats[BR_MAX_PATHS];
    char              client_host[64];
    uint16_t          client_feedback_port;
};

br_server_t* br_server_new(uint16_t port, size_t expected_len,
                           uint16_t symbol_size,
                           const char* client_host,
                           uint16_t client_feedback_port) {
    if (expected_len == 0 || symbol_size == 0) return NULL;
    br_server_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->port = port;
    s->expected_len = expected_len;
    s->symbol_size  = symbol_size;
    s->dec = NULL;
    s->client_feedback_port = client_feedback_port;

    if (client_host) {
        strncpy(s->client_host, client_host, sizeof(s->client_host) - 1);
    }

    s->rdma.max_wr = 256;
    s->rdma.n_paths = 0;
    s->rdma.max_paths = BR_MAX_PATHS;

    return s;
}

void br_server_free(br_server_t* s) {
    if (!s) return;
    br_decoder_free(s->dec);
    for (uint32_t i = 0; i < s->rdma.n_paths; i++) {
        br_rdma_cleanup_path(&s->rdma.paths[i]);
    }
    free(s);
}

static void server_send_feedback_rdma(br_server_t* s, uint32_t path,
                                      br_feedback_pkt_t *fb) {
    if (path >= s->rdma.n_paths) return;

    br_rdma_post_send(&s->rdma.paths[path], fb, sizeof(*fb));

    struct ibv_wc wc;
    br_rdma_poll_cq(&s->rdma.paths[path], 100, &wc);
}

static void server_check_loss(br_server_t* s, const br_net_header_t* hdr) {
    uint32_t path = hdr->path_id;
    uint32_t seq = hdr->path_seq;
    br_path_stats_t* st = &s->stats[path];

    if (st->packets_received > 0) {
        uint32_t expected = st->last_seq + 1;
        if (seq > expected) {
            uint32_t lost = seq - expected;
            st->packets_lost += lost;

            br_feedback_pkt_t fb = {
                .magic = BR_MAGIC,
                .path_id = path,
                .path_seq = expected,
                .flags = BR_FLAG_LOSS,
                .severity = BR_SEV_SEVERE,
                .rtt_usec = 0,
                .lost_seq = expected,
                .lost_count = lost
            };
            server_send_feedback_rdma(s, path, &fb);
        }
    }
    st->last_seq = seq;
    st->packets_received++;
}

static void server_check_ecn(br_server_t* s, const br_net_header_t* hdr) {
    if (!(hdr->flags & BR_FLAG_ECN)) return;

    uint32_t path = hdr->path_id;
    s->stats[path].ecn_count++;

    br_feedback_pkt_t fb = {
        .magic = BR_MAGIC,
        .path_id = path,
        .path_seq = hdr->path_seq,
        .flags = BR_FLAG_ECN,
        .severity = BR_SEV_MODERATE,
        .rtt_usec = 0,
        .lost_seq = 0,
        .lost_count = 0
    };
    server_send_feedback_rdma(s, path, &fb);
}

int br_server_accept_paths(br_server_t* s, int expected_paths) {
    if (!s) return BR_ERR_INVAL;
    if (s->rdma.n_paths > 0) return BR_OK;

    int ret = br_rdma_accept_multi(&s->rdma, s->port, expected_paths);
    if (ret != BR_OK) return ret;

    return BR_OK;
}

int br_server_recv(br_server_t* s, void* out, size_t len, int timeout_ms) {
    if (!s || !out) return BR_ERR_INVAL;
    if (s->rdma.n_paths == 0) return BR_ERR_NET;

    uint8_t* buf = malloc(sizeof(br_net_header_t) + s->symbol_size);
    if (!buf) return BR_ERR_NOMEM;

    int64_t remain = timeout_ms;

    while (1) {
        if (s->dec && br_decoder_ready(s->dec))
            break;

        struct ibv_wc wc;
        uint32_t found_path = 0;

        int ret = br_rdma_poll_all_cqs(&s->rdma, (remain > 1000) ? 1000 : (int)remain,
                                      &wc, &found_path);
        if (ret <= 0) {
            remain -= (remain > 1000) ? 1000 : remain;
            if (remain <= 0) { 
                free(buf); 
                return BR_ERR_TIMEOUT; 
            }
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS || wc.opcode != IBV_WC_RECV)
            continue;

        memcpy(buf, s->rdma.paths[found_path].recv_bufs[wc.wr_id].buf,
               sizeof(br_net_header_t) + s->symbol_size);

        br_net_header_t hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != BR_MAGIC) {
            br_rdma_post_recv(&s->rdma.paths[found_path], wc.wr_id);
            continue;
        }
        if (hdr.path_id >= BR_MAX_PATHS) {
            br_rdma_post_recv(&s->rdma.paths[found_path], wc.wr_id);
            continue;
        }

        size_t plen = wc.byte_len - sizeof(br_net_header_t);
        if (plen != hdr.symbol_size) {
            br_rdma_post_recv(&s->rdma.paths[found_path], wc.wr_id);
            continue;
        }

        server_check_loss(s, &hdr);
        server_check_ecn(s, &hdr);

        if (!s->dec) {
            s->dec = br_decoder_new(hdr.oti_common, hdr.oti_scheme,
                                    s->expected_len);
            if (!s->dec) { 
                free(buf); 
                return BR_ERR_CODEC; 
            }
        }

        int rc = br_decoder_feed(s->dec, (uint8_t)hdr.block_id, hdr.symbol_id,
                               buf + sizeof(br_net_header_t), plen);
        (void)rc;

        br_rdma_post_recv(&s->rdma.paths[found_path], wc.wr_id);
    }

    free(buf);
    if (!s->dec) return BR_ERR_TIMEOUT;
    return br_decoder_reconstruct(s->dec, out, len);
}