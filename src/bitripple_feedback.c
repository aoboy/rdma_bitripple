/**
 * @file bitripple_feedback.c
 * @brief Feedback-driven adaptation implementation
 *
 * Implements feedback processing and profile updates based on
 * network conditions (ECN, loss, RTT) from servers.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bitripple.h"

/*======================================================================*
 * Internal Structure                                                 *
 *======================================================================*/

/**
 * @brief Feedback context for path adaptation
 *
 * Maintains running statistics and smoothing state
 * for each path.
 */
struct br_feedback {
    br_profile_t* profile;           /**< Profile to adapt */
    uint32_t n_paths;              /**< Number of paths */

    /* Running statistics per path */
    uint32_t ecn_count[BR_MAX_PATHS];
    uint32_t loss_count[BR_MAX_PATHS];
    uint32_t rtt_sum_usec[BR_MAX_PATHS];
    uint32_t rtt_samples[BR_MAX_PATHS];
    uint32_t last_rtt_usec[BR_MAX_PATHS];

    /* EWMA smoothing for RTT */
    double rtt_ewma[BR_MAX_PATHS];

    /* Configuration thresholds */
    uint32_t rtt_baseline_usec;
    double loss_threshold;
    double ecn_threshold;
};

/*======================================================================*
 * Context Lifecycle                                                 *
 *======================================================================*/

/**
 * @brief Create a new feedback context
 *
 * Initializes feedback tracking for a profile.
 *
 * @param profile Profile to adapt
 * @param n_paths Number of paths
 * @return New feedback context, or NULL on error
 */
br_feedback_t* br_feedback_new(br_profile_t* profile, uint32_t n_paths)
{
    if (!profile || n_paths == 0 || n_paths > BR_MAX_PATHS) {
        return NULL;
    }
    if (n_paths != profile->n) {
        return NULL;
    }

    br_feedback_t* fb = calloc(1, sizeof(*fb));
    if (!fb) {
        return NULL;
    }

    fb->profile = profile;
    fb->n_paths = n_paths;
    fb->rtt_baseline_usec = 1000;   /* 1ms default */
    fb->loss_threshold = 0.05;       /* 5% loss threshold */
    fb->ecn_threshold = 0.10;      /* 10% ECN threshold */

    for (uint32_t i = 0; i < n_paths; i++) {
        fb->rtt_ewma[i] = 0.0;
    }

    return fb;
}

/**
 * @brief Free feedback context
 * @param fb Feedback context to free
 */
void br_feedback_free(br_feedback_t* fb)
{
    free(fb);
}

/*======================================================================*
 * Severity Classification                                           *
 *======================================================================*/

/**
 * @brief Classify severity from raw metrics
 *
 * Implements the severity classification algorithm
 * from Section 6 of the paper.
 *
 * @param ecn_count ECN marks received
 * @param loss_count Packets lost
 * @param rtt_usec Measured RTT
 * @param baseline_usec Baseline RTT
 * @return Severity level
 */
br_severity_t br_feedback_classify(uint32_t ecn_count, uint32_t loss_count,
                                  uint32_t rtt_usec, uint32_t baseline_usec)
{
    if (loss_count > 0) {
        return BR_SEV_SEVERE;
    }
    if (ecn_count > 0 && rtt_usec > 3 * baseline_usec) {
        return BR_SEV_CRITICAL;
    }
    if (ecn_count > 0) {
        return BR_SEV_MODERATE;
    }
    if (rtt_usec > 2 * baseline_usec) {
        return BR_SEV_MILD;
    }
    return BR_SEV_NONE;
}

/*======================================================================*
 * Severity-Based Redistribution (Embodiment 4)                         *
 *======================================================================*/

/**
 * @brief Apply severity classification to update profile
 *
 * Implements Embodiment 4: minimize sum(w[i]*b[i])
 * by redistributing balls inversely proportional to severity.
 *
 * @param fb Feedback context
 * @param severity Array of severity levels per path
 * @return BR_OK on success
 */
int br_feedback_apply_severity(br_feedback_t* fb,
                               const br_severity_t* severity)
{
    if (!fb || !severity) {
        return BR_ERR_INVAL;
    }

    br_profile_t* p = fb->profile;
    uint32_t n = p->n;

    /* Compute weights: higher severity = higher weight */
    double w[BR_MAX_PATHS];
    double wsum = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        switch (severity[i]) {
            case BR_SEV_NONE:
                w[i] = 1.0;
                break;
            case BR_SEV_MILD:
                w[i] = 2.0;
                break;
            case BR_SEV_MODERATE:
                w[i] = 4.0;
                break;
            case BR_SEV_SEVERE:
                w[i] = 8.0;
                break;
            case BR_SEV_CRITICAL:
                w[i] = 16.0;
                break;
            default:
                w[i] = 1.0;
        }
        wsum += w[i];
    }

    /* Redistribute inversely proportional to weight */
    uint32_t new_balls[BR_MAX_PATHS];
    uint32_t total = p->m;
    uint32_t allocated = 0;

    for (uint32_t i = 0; i < n; i++) {
        double frac = (1.0 / w[i]) / (n / wsum);
        new_balls[i] = (uint32_t)(total * frac);
        allocated += new_balls[i];
    }

    /* Fix rounding to maintain invariant */
    int32_t diff = (int32_t)total - (int32_t)allocated;
    while (diff > 0) {
        uint32_t best = 0;
        for (uint32_t i = 1; i < n; i++) {
            if (w[i] < w[best]) {
                best = i;
            }
        }
        new_balls[best]++;
        diff--;
    }
    while (diff < 0) {
        uint32_t worst = 0;
        for (uint32_t i = 1; i < n; i++) {
            if (w[i] > w[worst]) {
                worst = i;
            }
        }
        if (new_balls[worst] > 0) {
            new_balls[worst]--;
            diff++;
        }
    }

    memcpy(p->balls, new_balls, n * sizeof(uint32_t));

    /* Rebuild cumulative */
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum += p->balls[i];
        p->cum[i] = sum;
    }

    return BR_OK;
}

/*======================================================================*
 * Individual Feedback Handlers                                           *
 *======================================================================*/

/**
 * @brief Apply ECN feedback for a path
 *
 * Applies moderate whacking (alpha=0.25) when ECN rate
 * exceeds the debounce threshold.
 *
 * @param fb Feedback context
 * @param path Path index
 * @return BR_OK on success
 */
int br_feedback_apply_ecn(br_feedback_t* fb, uint32_t path)
{
    if (!fb || path >= fb->n_paths) {
        return BR_ERR_INVAL;
    }
    fb->ecn_count[path]++;

    /* Debounce: only react if ECN rate exceeds threshold */
    if (fb->ecn_count[path] < 3) {
        return BR_OK;
    }

    return br_profile_update_whack(fb->profile, path, 0.25);
}

/**
 * @brief Apply loss feedback for a path
 *
 * Applies severe whacking (alpha=0.50-0.90) based on
 * the number of lost packets.
 *
 * @param fb Feedback context
 * @param path Path index
 * @param lost_count Number of lost packets
 * @return BR_OK on success
 */
int br_feedback_apply_loss(br_feedback_t* fb, uint32_t path,
                        uint32_t lost_count)
{
    if (!fb || path >= fb->n_paths) {
        return BR_ERR_INVAL;
    }
    fb->loss_count[path] += lost_count;

    /* Exponential backoff based on loss count */
    double alpha = 0.50;
    if (lost_count > 5) {
        alpha = 0.75;
    }
    if (lost_count > 10) {
        alpha = 0.90;
    }

    return br_profile_update_whack(fb->profile, path, alpha);
}

/**
 * @brief Apply RTT measurement for a path
 *
 * Updates EWMA and applies mild whacking if RTT
 * is significantly elevated above baseline.
 *
 * @param fb Feedback context
 * @param path Path index
 * @param rtt_usec Measured RTT in microseconds
 * @param baseline_usec Baseline RTT for comparison
 * @return BR_OK on success
 */
int br_feedback_apply_rtt(br_feedback_t* fb, uint32_t path,
                        uint32_t rtt_usec, uint32_t baseline_usec)
{
    if (!fb || path >= fb->n_paths) {
        return BR_ERR_INVAL;
    }

    fb->rtt_sum_usec[path] += rtt_usec;
    fb->rtt_samples[path]++;
    fb->last_rtt_usec[path] = rtt_usec;

    /* EWMA update with smoothing factor 0.3 */
    double alpha = 0.3;
    if (fb->rtt_ewma[path] == 0.0) {
        fb->rtt_ewma[path] = (double)rtt_usec;
    } else {
        fb->rtt_ewma[path] = alpha * rtt_usec +
                            (1.0 - alpha) * fb->rtt_ewma[path];
    }

    /* React if RTT significantly elevated */
    if (fb->rtt_ewma[path] > 2.0 * baseline_usec) {
        double whack = 0.10;  /* Mild: reduce 10% */
        if (fb->rtt_ewma[path] > 4.0 * baseline_usec) {
            whack = 0.30;
        }
        return br_profile_update_whack(fb->profile, path, whack);
    }

    return BR_OK;
}