/**
 * @file bitripple_profile.c
 * @brief Profile management implementation
 *
 * Implements the ball distribution profile for spray routing.
 * Provides creation, updates, and cumulative sum rebuild.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bitripple.h"

/*======================================================================*
 * Profile Creation                                                     *
 *======================================================================*/

/**
 * @brief Create a new profile
 *
 * Allocates and initializes a new profile with N paths and M total balls.
 * The ball count M must be a power of 2 for proper spray operation.
 *
 * @param n Number of paths (1 to BR_MAX_PATHS)
 * @param m Total balls (must be power of 2)
 * @return New profile, or NULL on error
 */
br_profile_t* br_profile_new(uint32_t n, uint32_t m)
{
    if (n == 0 || n > BR_MAX_PATHS || m == 0 || (m & (m - 1)) != 0) {
        return NULL;
    }

    br_profile_t* p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }

    p->n = n;
    p->m = m;
    p->residual = 0;
    br_profile_set_uniform(p);

    return p;
}

/**
 * @brief Free a profile
 * @param p Profile to free
 */
void br_profile_free(br_profile_t* p)
{
    free(p);
}

/*======================================================================*
 * Internal Helpers                                                    *
 *======================================================================*/

/**
 * @brief Rebuild cumulative sum array from ball counts
 *
 * Rebuilds the cumulative array for O(log n) path selection
 * using binary search.
 *
 * @param p Profile to update
 */
static void rebuild_cum(br_profile_t* p)
{
    uint32_t sum = 0;

    for (uint32_t i = 0; i < p->n; i++) {
        sum += p->balls[i];
        p->cum[i] = sum;
    }
}

/*======================================================================*
 * Profile Initialization                                             *
 *======================================================================*/

/**
 * @brief Set uniform ball distribution
 *
 * Distributes balls as evenly as possible across all paths.
 * Each path gets either floor(M/N) or floor(M/N)+1 balls.
 *
 * @param p Profile to modify
 * @return BR_OK on success
 */
int br_profile_set_uniform(br_profile_t* p)
{
    if (!p) {
        return BR_ERR_INVAL;
    }

    uint32_t base = p->m / p->n;
    uint32_t rem  = p->m % p->n;

    for (uint32_t i = 0; i < p->n; i++) {
        p->balls[i] = base + (i < rem ? 1 : 0);
    }

    p->residual = rem % p->n;
    rebuild_cum(p);

    return BR_OK;
}

/**
 * @brief Set distribution from fractional weights
 *
 * Converts fractional weights to integer ball counts.
 * Excess balls are distributed round-robin to maintain total.
 *
 * @param p Profile to modify
 * @param frac Fractional weights for each path
 * @return BR_OK on success
 */
int br_profile_set_fractions(br_profile_t* p, const double* frac)
{
    if (!p || !frac) {
        return BR_ERR_INVAL;
    }

    uint32_t sum = 0;

    for (uint32_t i = 0; i < p->n; i++) {
        if (frac[i] < 0) {
            return BR_ERR_INVAL;
        }
        p->balls[i] = (uint32_t)(frac[i] * p->m);
        sum += p->balls[i];
    }

    /* Add missing balls round-robin */
    uint32_t i = 0;
    while (sum < p->m) {
        p->balls[i % p->n]++;
        sum++;
        i++;
    }

    /* Remove excess balls round-robin */
    i = 0;
    while (sum > p->m) {
        if (p->balls[i % p->n] > 0) {
            p->balls[i % p->n]--;
            sum--;
        }
        i++;
    }

    rebuild_cum(p);

    return BR_OK;
}

/*======================================================================*
 * Profile Updates (Embodiment Implementations)                         *
 *======================================================================*/

/**
 * @brief Embodiment 1: Whack a path, redistribute evenly to all others
 *
 * Reduces balls from the specified path by alpha fraction,
 * then redistributes those balls evenly across all paths.
 *
 * @param p Profile to update
 * @param path Path index to reduce
 * @param alpha Fraction to remove (0.0 to 1.0)
 * @return BR_OK on success
 */
int br_profile_update_whack(br_profile_t* p, uint32_t path, double alpha)
{
    if (!p || path >= p->n || alpha < 0.0 || alpha > 1.0) {
        return BR_ERR_INVAL;
    }

    uint32_t e = (uint32_t)(alpha * p->balls[path]);
    if (e == 0) {
        return BR_OK;
    }
    if (e > p->balls[path]) {
        e = p->balls[path];
    }

    uint32_t x = e / p->n;
    uint32_t y = e % p->n;

    /* Redistribute evenly to all paths except the whacked one */
    for (uint32_t i = 0; i < p->n; i++) {
        if (i != path) {
            p->balls[i] += x;
        }
    }
    p->balls[path] = p->balls[path] - e + x;

    /* Distribute remainder round-robin */
    for (uint32_t i = 0; i < y; i++) {
        p->residual = (p->residual + 1) % p->n;
        p->balls[p->residual]++;
    }

    rebuild_cum(p);

    return BR_OK;
}

/*======================================================================*
 * Additional Profile Updates                                            *
 *======================================================================*/

/**
 * @brief Embodiment 2: Redistribute evenly to specified paths
 *
 * Redistributes balls evenly across a subset of paths.
 *
 * @param p Profile to update
 * @param e Array of path indices
 * @param n Number of paths in array
 * @return BR_OK on success
 */
int br_profile_update_redistribute_even(br_profile_t* p,
                                        const uint32_t* e, uint32_t n)
{
    if (!p || !e || n == 0 || n > p->n) {
        return BR_ERR_INVAL;
    }

    uint32_t total = p->m;
    uint32_t base = total / n;
    uint32_t rem = total % n;

    for (uint32_t i = 0; i < n; i++) {
        if (e[i] >= p->n) {
            return BR_ERR_INVAL;
        }
        p->balls[e[i]] = base + (i < rem ? 1 : 0);
    }

    for (uint32_t i = 0; i < p->n; i++) {
        int found = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (e[j] == i) {
                found = 1;
                break;
            }
        }
        if (!found) {
            p->balls[i] = 0;
        }
    }

    rebuild_cum(p);

    return BR_OK;
}

/**
 * @brief Embodiment 2 variant: Redistribute to all paths except specified
 *
 * @param p Profile to update
 * @param e Array of path indices to exclude
 * @param n Number of paths in array
 * @return BR_OK on success
 */
int br_profile_update_redistribute_exclude(br_profile_t* p,
                                        const uint32_t* e, uint32_t n)
{
    if (!p || !e || n > p->n) {
        return BR_ERR_INVAL;
    }

    uint32_t count = p->n - n;
    if (count == 0) {
        for (uint32_t i = 0; i < p->n; i++) {
            p->balls[i] = 0;
        }
        rebuild_cum(p);
        return BR_OK;
    }

    uint32_t total = p->m;
    uint32_t base = total / count;
    uint32_t rem = total % count;

    uint32_t path_idx = 0;
    for (uint32_t i = 0; i < p->n; i++) {
        int excluded = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (e[j] == i) {
                excluded = 1;
                break;
            }
        }
        if (!excluded) {
            p->balls[i] = base + (path_idx < rem ? 1 : 0);
            path_idx++;
        } else {
            p->balls[i] = 0;
        }
    }

    rebuild_cum(p);

    return BR_OK;
}

/**
 * @brief Embodiment 4: Redistribute proportionally to path quality
 *
 * Redistributes balls proportionally across paths based on
 * implicit weighting (inverse of index for simplicity).
 *
 * @param p Profile to update
 * @param e Array of path indices
 * @param n Number of paths in array
 * @return BR_OK on success
 */
int br_profile_update_redistribute_proportional(br_profile_t* p,
                                               const uint32_t* e, uint32_t n)
{
    if (!p || !e || n == 0 || n > p->n) {
        return BR_ERR_INVAL;
    }

    double weight_sum = 0.0;
    double* weights = malloc(n * sizeof(double));
    if (!weights) {
        return BR_ERR_NOMEM;
    }

    for (uint32_t i = 0; i < n; i++) {
        if (e[i] >= p->n) {
            free(weights);
            return BR_ERR_INVAL;
        }
        weights[i] = 1.0 / (double)(i + 1);
        weight_sum += weights[i];
    }

    uint32_t total = p->m;
    uint32_t allocated = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t balls = (uint32_t)((weights[i] / weight_sum) * total);
        p->balls[e[i]] = balls;
        allocated += balls;
    }

    free(weights);

    int32_t diff = (int32_t)total - (int32_t)allocated;
    while (diff > 0) {
        p->balls[e[0]]++;
        diff--;
    }

    for (uint32_t i = 0; i < p->n; i++) {
        int found = 0;
        for (uint32_t j = 0; j < n; j++) {
            if (e[j] == i) {
                found = 1;
                break;
            }
        }
        if (!found) {
            p->balls[i] = 0;
        }
    }

    rebuild_cum(p);

    return BR_OK;
}