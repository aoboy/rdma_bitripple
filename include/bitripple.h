#ifndef BITRIPPLE_H
#define BITRIPPLE_H

#include "bitripple_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Library lifecycle */
int br_init(void);
void br_cleanup(void);

/* Profile */
br_profile_t* br_profile_new(uint32_t n_paths, uint32_t m_balls);
void br_profile_free(br_profile_t* p);
int br_profile_set_uniform(br_profile_t* p);
int br_profile_set_fractions(br_profile_t* p, const double* frac);
int br_profile_update_whack(br_profile_t* p, uint32_t path, double alpha);
int br_profile_update_redistribute_even(br_profile_t* p, 
                                        const uint32_t* e, uint32_t n);
int br_profile_update_redistribute_exclude(br_profile_t* p,
                                           const uint32_t* e, uint32_t n);
int br_profile_update_redistribute_proportional(br_profile_t* p,
                                                const uint32_t* e, uint32_t n);

/* Spray */
br_spray_t* br_spray_new(uint32_t l, uint32_t sa, uint32_t sb, int method);
void br_spray_free(br_spray_t* s);
uint32_t br_spray_select(const br_profile_t* p, br_spray_t* s, uint32_t j);
uint32_t br_spray_next(br_profile_t* p, br_spray_t* s);

/* Codec */
typedef struct br_encoder br_encoder_t;
typedef struct br_decoder br_decoder_t;

br_encoder_t* br_encoder_new(const void* data, size_t len, uint16_t symbol_size);
void br_encoder_free(br_encoder_t* e);
uint32_t br_encoder_num_blocks(br_encoder_t* e);
uint32_t br_encoder_block_symbols(br_encoder_t* e, uint8_t sbn);
int br_encoder_symbol(br_encoder_t* e, uint8_t sbn, uint32_t esi,
                      void* buf, size_t* out_len);
void br_encoder_oti(br_encoder_t* e, uint64_t* common, uint32_t* scheme);

br_decoder_t* br_decoder_new(uint64_t oti_common, uint32_t oti_scheme,
                             size_t data_len);
void br_decoder_free(br_decoder_t* d);
int br_decoder_feed(br_decoder_t* d, uint8_t sbn, uint32_t esi,
                    const void* buf, size_t len);
int br_decoder_ready(br_decoder_t* d);
int br_decoder_reconstruct(br_decoder_t* d, void* out, size_t len);

/* Feedback */
typedef struct br_feedback br_feedback_t;

br_feedback_t* br_feedback_new(br_profile_t* profile, uint32_t n_paths);
void br_feedback_free(br_feedback_t* fb);
br_severity_t br_feedback_classify(uint32_t ecn_count, uint32_t loss_count,
                                   uint32_t rtt_usec, uint32_t baseline_usec);
int br_feedback_apply_severity(br_feedback_t* fb, 
                               const br_severity_t* severity);
int br_feedback_apply_ecn(br_feedback_t* fb, uint32_t path);
int br_feedback_apply_loss(br_feedback_t* fb, uint32_t path, 
                           uint32_t lost_count);
int br_feedback_apply_rtt(br_feedback_t* fb, uint32_t path,
                          uint32_t rtt_usec, uint32_t baseline_usec);

/* RDMA Client — one QP per path */
typedef struct br_client br_client_t;
br_client_t* br_client_new(const br_profile_t* prof,
                           const char** hosts,
                           const uint16_t* ports,
                           uint32_t n_paths,
                           uint16_t symbol_size,
                           uint16_t feedback_port);
void br_client_free(br_client_t* c);
int br_client_send(br_client_t* c, const void* data, size_t len);
int br_client_poll_feedback(br_client_t* c, int timeout_ms);

/* RDMA Server — single listening QP */
typedef struct br_server br_server_t;
br_server_t* br_server_new(uint16_t port, size_t expected_len, 
                           uint16_t symbol_size,
                           const char* client_host,
                           uint16_t client_feedback_port);
void br_server_free(br_server_t* s);
int br_server_recv(br_server_t* s, void* out, size_t len, int timeout_ms);
int br_server_accept_paths(br_server_t* s, int expected_paths);

/* RDMA utility: set device name for a path (default: mlx5_0) */
int br_client_set_rdma_device(br_client_t* c, uint32_t path_idx, 
                              const char* dev_name, int port_num);
int br_server_set_rdma_device(br_server_t* s, const char* dev_name, 
                              int port_num);

#ifdef __cplusplus
}
#endif

#endif

