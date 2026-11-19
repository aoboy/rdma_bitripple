#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <math.h>
#include "bitripple.h"
#include "bitripple_rdma_internal.h"

struct br_client {
    br_profile_t*     prof;
    br_spray_t*       spray;
    br_encoder_t*    enc;
    br_feedback_t*    fb;
    br_rdma_ctx_t     rdma;
    uint32_t          n_paths;
    char*             hosts[BR_MAX_PATHS];
    uint16_t          ports[BR_MAX_PATHS];
    uint32_t          path_seq[BR_MAX_PATHS];
    uint16_t          symbol_size;
    uint16_t          feedback_port;
};

br_client_t* br_client_new(const br_profile_t* prof,
                           const char** hosts,
                           const uint16_t* ports,
                           uint32_t n_paths,
                           uint16_t symbol_size,
                           uint16_t feedback_port) {
    if (!prof || !hosts || !ports || n_paths == 0 || n_paths > BR_MAX_PATHS)
        return NULL;
    if (n_paths != prof->n) return NULL;

    br_client_t* c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->prof = br_profile_new(prof->n, prof->m);
    if (!c->prof) { free(c); return NULL; }
    memcpy(c->prof, prof, sizeof(*prof));

    uint32_t l = prof->m > 1 ? (uint32_t)log2(prof->m) : 1;
    c->spray = br_spray_new(l, 0, 1, 1);
    if (!c->spray) {
        br_profile_free(c->prof);
        free(c);
        return NULL;
    }

    c->n_paths = n_paths;
    c->symbol_size = symbol_size;
    c->feedback_port = feedback_port;

    for (uint32_t i = 0; i < n_paths; i++) {
        c->hosts[i] = malloc(256);
        if (c->hosts[i]) {
            strncpy(c->hosts[i], hosts[i], 255);
            c->hosts[i][255] = '\0';
        }
        c->ports[i] = ports[i];
        c->path_seq[i] = 0;
    }

    char *dev = getenv("BR_RDMA_DEV");
    if (!dev) dev = "rxe0";

    size_t buf_size = sizeof(br_net_header_t) + symbol_size;
    c->rdma.n_paths = 0;
    c->rdma.max_wr = 256;

    for (uint32_t i = 0; i < n_paths; i++) {
        br_rdma_path_t *path = &c->rdma.paths[i];
        if (br_rdma_init_path(path, dev, 1, buf_size) != BR_OK) {
            br_client_free(c);
            return NULL;
        }
        if (br_rdma_connect_client(path, hosts[i], ports[i]) != BR_OK) {
            br_client_free(c);
            return NULL;
        }
    }
    c->rdma.n_paths = n_paths;

    c->fb = br_feedback_new(c->prof, n_paths);
    if (!c->fb) {
        br_client_free(c);
        return NULL;
    }

    return c;
}

void br_client_free(br_client_t* c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->rdma.n_paths; i++) {
        br_rdma_cleanup_path(&c->rdma.paths[i]);
    }
    br_encoder_free(c->enc);
    br_feedback_free(c->fb);
    br_spray_free(c->spray);
    br_profile_free(c->prof);
    for (uint32_t i = 0; i < c->n_paths; i++) free(c->hosts[i]);
    free(c);
}

int br_client_send(br_client_t* c, const void* data, size_t len) {
    if (!c || !data) return BR_ERR_INVAL;

    if (c->enc) { br_encoder_free(c->enc); c->enc = NULL; }
    c->enc = br_encoder_new(data, len, c->symbol_size);
    if (!c->enc) return BR_ERR_CODEC;

    uint64_t oti_common = 0;
    uint32_t oti_scheme = 0;
    br_encoder_oti(c->enc, &oti_common, &oti_scheme);

    uint8_t* pkt = malloc(sizeof(br_net_header_t) + c->symbol_size);
    if (!pkt) {
        br_encoder_free(c->enc);
        c->enc = NULL;
        return BR_ERR_NOMEM;
    }

    uint32_t num_blocks = br_encoder_num_blocks(c->enc);

    for (uint8_t sbn = 0; sbn < num_blocks; sbn++) {
        uint32_t num_src = br_encoder_block_symbols(c->enc, sbn);
        uint32_t total = num_src + (num_src / 2);
        if (total < num_src + 4) total = num_src + 4;

        // Pre-allocate all symbols for this block
        typedef struct {
            uint8_t* data;
            size_t len;
        } symbol_data_t;
        
        symbol_data_t* symbols = malloc(total * sizeof(symbol_data_t));
        if (!symbols) {
            free(pkt);
            br_encoder_free(c->enc);
            c->enc = NULL;
            return BR_ERR_NOMEM;
        }
        
        // Generate all symbols for this block
        for (uint32_t i = 0; i < total; i++) {
            uint32_t esi = i;
            symbols[i].data = malloc(c->symbol_size);
            if (!symbols[i].data) {
                // Free previously allocated symbols
                for (uint32_t j = 0; j < i; j++) {
                    free(symbols[j].data);
                }
                free(symbols);
                free(pkt);
                br_encoder_free(c->enc);
                c->enc = NULL;
                return BR_ERR_NOMEM;
            }
            
            int rc = br_encoder_symbol(c->enc, sbn, esi,
                                  symbols[i].data, &symbols[i].len);
            if (rc != BR_OK) {
                // Free allocated symbols
                for (uint32_t j = 0; j <= i; j++) {
                    free(symbols[j].data);
                }
                free(symbols);
                free(pkt);
                br_encoder_free(c->enc);
                c->enc = NULL;
                return rc;
            }
        }

        // Send all symbols for this block
        for (uint32_t i = 0; i < total; i++) {
            uint32_t esi = i;
            uint32_t path_idx = br_spray_next(c->prof, c->spray);
            path_idx = path_idx % c->n_paths;
            size_t sym_len = symbols[i].len;

            br_net_header_t hdr = {
                .magic       = BR_MAGIC,
                .block_id    = sbn,
                .symbol_id   = esi,
                .path_id     = path_idx,
                .path_seq    = c->path_seq[path_idx]++,
                .symbol_size  = (uint16_t)sym_len,
                .flags       = 0,
                .oti_common  = oti_common,
                .oti_scheme  = oti_scheme,
                .rtt_usec    = 0
            };
            memcpy(pkt, &hdr, sizeof(hdr));
            memcpy(pkt + sizeof(hdr), symbols[i].data, sym_len);

            if (path_idx >= c->n_paths) path_idx = 0;
            int rc = br_rdma_post_send(&c->rdma.paths[path_idx], pkt,
                                       sizeof(hdr) + sym_len);
            if (rc != BR_OK) {
                // Free allocated symbols
                for (uint32_t j = 0; j < total; j++) {
                    free(symbols[j].data);
                }
                free(symbols);
                free(pkt);
                br_encoder_free(c->enc);
                c->enc = NULL;
                return rc;
            }
        }
        
        // Free symbols for this block
        for (uint32_t i = 0; i < total; i++) {
            free(symbols[i].data);
        }
        free(symbols);
    }

    free(pkt);
    return BR_OK;
}

int br_client_poll_feedback(br_client_t* c, int timeout_ms) {
    if (!c) return BR_ERR_INVAL;

    struct ibv_wc wc;
    int64_t remain = timeout_ms;

    while (remain > 0) {
        for (uint32_t i = 0; i < c->rdma.n_paths; i++) {
            int ret = br_rdma_poll_cq(&c->rdma.paths[i], 0, &wc);
            if (ret > 0 && wc.opcode == IBV_WC_RECV) {
                br_feedback_pkt_t *fb = (br_feedback_pkt_t*)
                    c->rdma.paths[i].recv_bufs[wc.wr_id].buf;
                if (fb->magic == BR_MAGIC) {
                    if (fb->flags & BR_FLAG_LOSS) {
                        br_feedback_apply_loss(c->fb, fb->path_id, fb->lost_count);
                    }
                    if (fb->flags & BR_FLAG_ECN) {
                        br_feedback_apply_ecn(c->fb, fb->path_id);
                    }
                }
                br_rdma_post_recv(&c->rdma.paths[i], wc.wr_id);
            }
        }
        if (remain > 0) {
            usleep(1000);
            remain--;
        }
    }
    return BR_OK;
}