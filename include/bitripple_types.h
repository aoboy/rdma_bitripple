#ifndef BITRIPPLE_TYPES_H
#define BITRIPPLE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_MAX_PATHS       8
#define BR_MAX_SYMBOL_SIZE 65535
#define BR_MAGIC           0x4252504C /* 'BRPL' */

/* RDMA device defaults */
#define BR_RDMA_DEFAULT_DEV  "rxe0"
#define BR_RDMA_DEFAULT_PORT 1
#define BR_RDMA_MAX_WR       128
#define BR_RDMA_CQ_SIZE      256

/* Feedback flags */
#define BR_FLAG_ECN        0x0001
#define BR_FLAG_LOSS       0x0002
#define BR_FLAG_RTT_UPDATE 0x0004

/* Severity weights */
typedef enum {
    BR_SEV_NONE = 0,
    BR_SEV_MILD = 1,
    BR_SEV_MODERATE = 2,
    BR_SEV_SEVERE = 3,
    BR_SEV_CRITICAL = 4
} br_severity_t;

typedef struct br_profile {
    uint32_t n;
    uint32_t m;
    uint32_t balls[BR_MAX_PATHS];
    uint32_t cum[BR_MAX_PATHS];
    uint32_t residual;
} br_profile_t;

typedef struct br_spray {
    uint32_t j;
    uint32_t l;
    uint32_t sa;
    uint32_t sb;
    int      method;
} br_spray_t;

/* Network header — same format, now carried in RDMA SEND payload */
typedef struct br_net_header {
    uint32_t magic;
    uint32_t block_id;
    uint32_t symbol_id;
    uint32_t path_id;
    uint32_t path_seq;
    uint16_t symbol_size;
    uint16_t flags;
    uint64_t oti_common;
    uint32_t oti_scheme;
    uint32_t rtt_usec;
} __attribute__((packed)) br_net_header_t;

/* Feedback packet */
typedef struct br_feedback_pkt {
    uint32_t magic;
    uint32_t path_id;
    uint32_t path_seq;
    uint16_t flags;
    uint16_t severity;
    uint32_t rtt_usec;
    uint32_t lost_seq;
    uint32_t lost_count;
} __attribute__((packed)) br_feedback_pkt_t;

/* Per-path statistics */
typedef struct br_path_stats {
    uint32_t packets_received;
    uint32_t packets_lost;
    uint32_t last_seq;
    uint32_t ecn_count;
    uint64_t rtt_sum_usec;
    uint32_t rtt_samples;
    uint32_t last_rtt_usec;
} br_path_stats_t;

typedef enum {
    BR_OK = 0,
    BR_ERR_NOMEM = -1,
    BR_ERR_INVAL = -2,
    BR_ERR_CODEC = -3,
    BR_ERR_NET   = -4,
    BR_ERR_TIMEOUT = -5,
    BR_ERR_RDMA  = -6
} br_err_t;

#ifdef __cplusplus
}
#endif

#endif

