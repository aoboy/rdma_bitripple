/**
 * @file bitripple.c
 * @brief Library lifecycle implementation
 *
 * Minimal initialization/cleanup for the BitRipple library.
 * Currently a placeholder for future global state management.
 */

#include "bitripple.h"

/**
 * @brief Initialize the BitRipple library
 *
 * Performs any global initialization needed by the library.
 * Currently a no-op but provided for future extensibility.
 *
 * @return BR_OK always
 */
int br_init(void)
{
    return BR_OK;
}

/**
 * @brief Cleanup the BitRipple library
 *
 * Releases any global resources held by the library.
 * Currently a no-op but provided for future extensibility.
 */
void br_cleanup(void)
{
    /* No global resources to release */
}