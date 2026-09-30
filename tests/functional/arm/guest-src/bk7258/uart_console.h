/*
 * SPDX-License-Identifier: Apache-2.0
 * Shared console helper for source-built BK7258 hardware test fixtures.
 * Adapted from diagnostic.c's Apache-2.0 UART setup/printing pattern.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#ifndef BK7258_TEST_UART_CONSOLE_H
#define BK7258_TEST_UART_CONSOLE_H

#include <stdint.h>

static inline void bk7258_test_console_puts(const char *text)
{
    /* Volatile hardware transactions preserve FIFO and completion ordering. */
    volatile uint32_t *uart = (volatile uint32_t *)(uintptr_t)0x44820000;

    if (!*text) {
        return;
    }
    while (*text) {
        while (!(uart[0x18 / 4] & (1U << 20))) {
        }
        /*
         * TX_FINISH is W1C; keep one byte outstanding so an interrupt cannot
         * leave an earlier byte's completion latched during a later shift.
         */
        uart[0x24 / 4] = 1U << 5;
        uart[0x1c / 4] = (uint8_t)*text++;
        /* FIFO occupancy excludes the outstanding shift byte. */
        while ((uart[0x18 / 4] & 0xff) || !(uart[0x24 / 4] & (1U << 5))) {
        }
    }
}

#endif
