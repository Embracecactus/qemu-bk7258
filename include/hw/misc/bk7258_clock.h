/*
 * BK7258 bounded peripheral-operation clock accounting.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#ifndef HW_MISC_BK7258_CLOCK_H
#define HW_MISC_BK7258_CLOCK_H

#include "qemu/timer.h"

/*
 * Call with the unchanged cycle budget and old rate of a pending timer.
 * All modeled operations have at most 32-bit cycle budgets. Timestamps are
 * nonnegative QEMU virtual time, and the armed deadline must be representable.
 */
static inline uint64_t bk7258_remaining_cycles(uint64_t budget, unsigned hz,
                                               int64_t deadline, int64_t now)
{
    uint64_t duration, left, elapsed, spent;

    assert(hz && budget <= UINT32_MAX);
    if (now >= deadline) {
        return 0;
    }
    duration = DIV_ROUND_UP(budget * NANOSECONDS_PER_SECOND, hz);
    left = deadline - now;
    /* Recover elapsed time from the arm anchor, not the rounded deadline. */
    elapsed = duration - MIN(duration, left);
    /* elapsed < duration bounds this product below budget * 1e9. */
    spent = elapsed * hz / NANOSECONDS_PER_SECOND;
    return budget - MIN(budget, spent);
}

#endif
