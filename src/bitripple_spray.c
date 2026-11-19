/**
 * @file bitripple_spray.c
 * @brief Spray routing implementation
 *
 * Implements the Spraypray-based path selection using
 * configurable spray methods (LRRT and RLRT variants).
 */

#include <stdlib.h>
#include "bitripple.h"

/*======================================================================*
 * Internal Helpers                                                    *
 *======================================================================*/

/**
 * @brief Reverse bits in an integer
 *
 * Reverses the bottom 'l' bits of x.
 *
 * @param x Value to reverse
 * @param l Number of bits to reverse
 * @return Bit-reversed value
 */
static inline uint32_t bitreverse(uint32_t x, uint32_t l)
{
    uint32_t r = 0;

    for (uint32_t i = 0; i < l; i++) {
        r |= ((x >> i) & 1U) << (l - 1 - i);
    }

    return r;
}

/*======================================================================*
 * Spray Creation                                                      *
 *======================================================================*/

/**
 * @brief Create a new spray state
 *
 * Initializes the spray generator for path selection.
 * The step value (sb) must be odd for full-period cycling.
 *
 * @param l Spray length in bits (log2 of spray factor)
 * @param sa Starting value
 * @param sb Step value (must be odd)
 * @param method Spray method (1=LRRT, 2=RLRT)
 * @return New spray state, or NULL on error
 */
br_spray_t* br_spray_new(uint32_t l, uint32_t sa, uint32_t sb, int method)
{
    if (l == 0 || l > 31 || (method != 1 && method != 2)) {
        return NULL;
    }
    if ((sb & 1U) == 0) {
        return NULL;  /* sb must be odd */
    }

    br_spray_t* s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->j = 0;
    s->l = l;
    s->sa = sa & ((1U << l) - 1);
    s->sb = sb & ((1U << l) - 1);

    if (s->sb == 0) {
        s->sb = 1;
    }

    s->method = method;

    return s;
}

/**
 * @brief Free spray state
 * @param s Spray to free
 */
void br_spray_free(br_spray_t* s)
{
    free(s);
}

/*======================================================================*
 * Path Selection                                                      *
 *======================================================================*/

/**
 * @brief Select path for a given spray index
 *
 * Uses the configured spray method to generate a path index
 * from the spray state and profile distribution.
 *
 * Method 1 (LRRT): bitreverse(sa + j*sb)
 * Method 2 (RLRT): sa + sb * bitreverse(j)
 *
 * @param p Profile with ball distribution
 * @param s Spray state
 * @param j Spray index
 * @return Selected path index
 */
uint32_t br_spray_select(const br_profile_t* p, br_spray_t* s, uint32_t j)
{
    uint32_t mask = (1U << s->l) - 1;
    uint32_t theta;

    if (s->method == 1) {
        /* LRRT: bitreverse on the result */
        theta = bitreverse((s->sa + j * s->sb) & mask, s->l);
    } else {
        /* RLRT: bitreverse on the index */
        theta = (s->sa + s->sb * bitreverse(j & mask, s->l)) & mask;
    }

    /* Binary search on cumulative to find path */
    int lo = 0;
    int hi = (int)p->n - 1;

    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (theta < p->cum[mid]) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }

    return (uint32_t)lo;
}

/**
 * @brief Get next path and advance spray state
 *
 * Returns the next path using the current spray index,
 * then increments the index (wrapping at 2^l).
 *
 * @param p Profile with ball distribution
 * @param s Spray state
 * @return Selected path index
 */
uint32_t br_spray_next(br_profile_t* p, br_spray_t* s)
{
    uint32_t path = br_spray_select(p, s, s->j);
    s->j++;

    if (s->j >= (1U << s->l)) {
        s->j = 0;
    }

    return path;
}