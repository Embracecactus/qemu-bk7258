/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 shared-RAM DMA, real IRQ0/57 and failed-transaction probe.
 * Adapted from the adjacent Apache-2.0 spi.c bare-metal harness.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Guest MMIO and DMA-visible shared memory, never host model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
/* DMA-visible bytes must not be folded into cached compiler values. */
#define BYTE(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define FIN (1u << 18)
#define HALF (1u << 19)
#define ERR (1u << 20)
/* Handler observations and an independent CPU-clock timeout. */
static volatile uint32_t ticks;
/* Recorded asynchronously by real NVIC handlers. */
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
    [16] = (uintptr_t)irq0,
    [73] = (uintptr_t)irq1,
};

static uint32_t base(unsigned unit)
{
    return 0x45020000u + unit * 0x10000u;
}

static void print(const char *text)
{
    bk7258_test_console_puts(text);
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    print(status ? "BK7258 DMA PROBE FAILED\n" :
                   "BK7258 DMA RAM IRQ ERROR ISOLATION OK\n");

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

static void handler(unsigned unit)
{
    uint32_t exception, status = REG(base(unit) + 0x70);

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != (unit ? 73 : 16) || (status & ERR) ||
        !(status & (FIN | HALF))) {
        finish(1);
    }
    seen[unit] |= status & (FIN | HALF);
    REG(base(unit) + 0x70) = status & (FIN | HALF);
}

static void irq0(void)
{
    handler(0);
}

static void irq1(void)
{
    handler(1);
}

static void transfer(unsigned unit, uint32_t src, uint32_t dst,
                     unsigned bytes, unsigned config)
{
    uint32_t c = base(unit) + 0x40;

    REG(c + 4) = dst;
    REG(c + 8) = src;
    REG(c) = ((bytes - 1) << 16) | config | 1;
}

static void wait_stopped(unsigned unit)
{
    uint32_t begin = ticks;

    while (REG(base(unit) + 0x40) & 1) {
        if (ticks - begin > 20) {
            finish(1);
        }
    }
}

static void start(void)
{
    ticks = seen[0] = seen[1] = 0;
    REG(SYS + 0x30) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = 1;
    REG(SYS + 0x84) = MISSING_ROUTE ? 0 : 1u << 25;
    REG(0xe000e100) = 1;
    REG(0xe000e104) = 1u << 25;
    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = base(unit), begin = ticks;

        REG(b + 8) = 1;
        for (unsigned n = 0; n < 1024; n++) {
            BYTE(0x28001000 + n) = n ^ 0xa5;
            BYTE(0x28002000 + n) = 0;
        }
        transfer(unit, 0x08001000, 0x38002000, 1024, 0x3a6);
        while (!(seen[unit] & FIN) && ticks - begin < 20) {
            __asm__ volatile("wfi");
        }
        for (unsigned n = 0; n < 1024; n++) {
            if (BYTE(0x18002000 + n) != (uint8_t)(n ^ 0xa5)) {
                finish(1);
            }
        }
        if (seen[unit] != (FIN | HALF)) {
#if MISSING_ROUTE
            if (unit == 1 && seen[0] == (FIN | HALF) && !seen[1] &&
                (REG(b + 0x70) & (FIN | HALF | ERR)) == (FIN | HALF) &&
                !(REG(SYS + 0x84) & (1u << 25))) {
                print("BK7258 MISSING DMA1 ROUTE DETECTED\n");
            }
#endif
            finish(1);
        }
        if (REG(b + 0x70) || REG(b + 0x1c)) {
            finish(1);
        }
    }
    /* Error checks use polling; mask and clear any stale NVIC level first. */
    REG(0xe000e180) = 1;
    REG(0xe000e184) = 1u << 25;
    REG(0xe000e280) = 1;
    REG(0xe000e284) = 1u << 25;
    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t c = base(unit) + 0x40;

        REG(0x2809fffc) = 0x76543210;
        REG(0x28002004) = 0xabcdef01;
        transfer(unit, 0x3809fffc, 0x18002000, 8, 0x3a0);
        wait_stopped(unit);
        if (REG(c + 0x30) != 0x10180004 ||
            REG(c + 0x28) != 0x380a0000 ||
            REG(c + 0x2c) != 0x18002004 ||
            REG(0x28002000) != 0x76543210 ||
            REG(0x28002004) != 0xabcdef01) {
            finish(1);
        }
        REG(c + 0x30) = FIN | HALF | ERR;
        /* A destination failure also retains progress and never completes. */
        transfer(unit, 0x28002000, 0x0809fffc, 8, 0x3a0);
        wait_stopped(unit);
        if (REG(c + 0x30) != 0x10180004 ||
            REG(c + 0x28) != 0x28002004 || REG(c + 0x2c) != 0x080a0000) {
            finish(1);
        }
        REG(c + 0x30) = FIN | HALF | ERR;
        REG(0x20002000) = 0xface0123;
        for (unsigned write = 0; write < 2; write++) {
            transfer(unit, write ? 0x28002000 : 0x20002000,
                     write ? 0x20002000 : 0x28002000, 4, 0xa0);
            wait_stopped(unit);
            if (REG(c + 0x30) != (ERR | 4) ||
                REG(0x20002000) != 0xface0123 ||
                REG(0x28002000) != 0x76543210) {
                finish(1);
            }
            REG(c + 0x30) = FIN | HALF | ERR;
        }
    }
    /* Independently check all width/fixed/increment combinations in RAM. */
    for (unsigned unit = 0; unit < 2; unit++) {
        for (unsigned w = 0; w < 3; w++) {
            unsigned width = 1u << w;

            for (unsigned inc = 0; inc < 4; inc++) {
                for (unsigned n = 0; n < 32; n++) {
                    BYTE(0x28001000 + n) = n ^ 0x5a;
                    BYTE(0x28002000 + n) = 0xcc;
                }
                transfer(unit, 0x18001000, 0x38002000, 32,
                         (w << 4) | (w << 6) | (inc << 8));
                wait_stopped(unit);
                if (REG(base(unit) + 0x70) != 0x110c0000) {
                    finish(1);
                }
                for (unsigned n = 0; n < 32; n++) {
                    unsigned expected = 0xcc;

                    if ((inc & 2) || n < width) {
                        unsigned offset = (inc & 2) ? n : 32 - width + n;

                        if (!(inc & 1)) {
                            offset %= width;
                        }
                        expected = offset ^ 0x5a;
                    }
                    if (BYTE(0x28002000 + n) != expected) {
                        finish(1);
                    }
                }
                REG(base(unit) + 0x70) = FIN | HALF | ERR;
            }
        }
    }
    finish(0);
}
