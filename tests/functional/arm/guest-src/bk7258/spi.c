/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 8-bit SSI-bus probe with externally supplied SPI flash fixtures.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile guest MMIO and exception-handler communication. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#ifndef LSB_FIRST
#define LSB_FIRST 0
#endif
/* Independent CPU-clock deadline published by SysTick. */
static volatile uint32_t ticks;
/* Completion observations from real IRQ7/17 handlers. */
static volatile uint32_t seen[2];
static void start(void);
static void fault(void);
static void systick(void);
static void irq0(void);
static void irq1(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [23] = (uintptr_t)irq0,
    [33] = (uintptr_t)irq1,
};

static uint32_t base(unsigned g)
{
    return 0x44870000u + 0x1010000u * g;
}

static unsigned irq(unsigned g)
{
    return g ? 17 : 7;
}

static void print(const char *text)
{
    bk7258_test_console_puts(text);
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    print(status ? "BK7258 SPI PROBE FAILED\n" :
                   (LSB_FIRST ? "BK7258 SPI LSB SSI ID IRQ OK\n" :
                                "BK7258 SPI SSI ID IRQ OK\n"));

    /* Bind caller-clobbered arguments only after console calls return. */
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(1);
}

static void systick(void)
{
    if (++ticks > 200) {
        finish(1);
    }
}

static void handler(unsigned g)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 16 + irq(g) ||
        (REG(base(g) + 0x18) & 0x7800) != 0x6000) {
        finish(1);
    }
    REG(base(g) + 0x18) = 0x6000;
    seen[g]++;
}

static void irq0(void)
{
    handler(0);
}

static void irq1(void)
{
    handler(1);
}

static void start(void)
{
    ticks = seen[0] = seen[1] = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 1) | (1u << 9);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = (1u << 7) | (MISSING_ROUTE ? 0 : (1u << 17));
    REG(0xe000e100) = (1u << 7) | (1u << 17);
    for (unsigned g = 0; g < 2; g++) {
        uint32_t begin = ticks;
        uint8_t received[4];

        REG(base(g) + 8) = 3;
        /* Eight-bit master, selected bit order; external slave is MSB. */
        REG(base(g) + 0x10) = 0x41c00130 | (LSB_FIRST ? 1u << 19 : 0);
        REG(base(g) + 0x1c) = LSB_FIRST ? 0xf9 : 0x9f;
        REG(base(g) + 0x1c) = 0;
        REG(base(g) + 0x1c) = 0;
        REG(base(g) + 0x1c) = 0;
        REG(base(g) + 0x14) = 0x0040040f;
        while (!seen[g] && ticks - begin < 20) {
            __asm__ volatile("wfi");
        }
        if (seen[g] != 1) {
#if MISSING_ROUTE
            if (g == 1 && seen[0] == 1 &&
                (REG(base(g) + 0x18) & 0x6000) == 0x6000 &&
                !(REG(SYS + 0x80) & (1u << 17))) {
                print("BK7258 MISSING SPI1 ROUTE DETECTED\n");
            }
#endif
            finish(1);
        }
        for (unsigned i = 0; i < 4; i++) {
            if (!(REG(base(g) + 0x18) & 4)) {
                finish(1);
            }
            received[i] = REG(base(g) + 0x1c);
        }
#if MISSING_ENDPOINT
        if (!g && !received[0] && !received[1] &&
            !received[2] && !received[3]) {
            print("BK7258 ABSENT SPI ENDPOINT DETECTED\n");
            finish(1);
        }
#endif
        /* ID belongs to the explicitly attached w25q32 test device. */
        if (received[1] != (LSB_FIRST ? 0xf7 : 0xef) ||
            received[2] != (LSB_FIRST ? 0x02 : 0x40) ||
            received[3] != (LSB_FIRST ? 0x68 : 0x16) ||
            (REG(base(g) + 0x18) & (4 | 0x6000))) {
            finish(1);
        }
        REG(base(g) + 0x14) = 0;
    }
    finish(0);
}
