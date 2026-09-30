/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 PWM counter/compare IRQ probe; no waveform or pad claim.
 * Adapted from the adjacent Apache-2.0 spi.c bare-metal harness.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>

/* Guest MMIO, never host device-model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
/* Independent SysTick timeout. */
static volatile uint32_t ticks;
/* Recorded asynchronously by real IRQ5/43 handlers. */
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
    [21] = (uintptr_t)irq0,
    [59] = (uintptr_t)irq1,
};

static uint32_t base(unsigned unit)
{
    return 0x458a0000u + unit * 0x50000u;
}

static void print(const char *text)
{
    while (*text) {
        REG(UART + 0x1c) = (uint8_t)*text++;
    }
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;

    print(status ? "BK7258 PWM COUNTER PROBE FAILED\n" :
                   "BK7258 PWM COUNTER COMPARE IRQ OK\n");
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
    if (++ticks > 100) {
        finish(1);
    }
}

static void handler(unsigned unit)
{
    uint32_t exception, status = REG(base(unit) + 0x20);

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != (unit ? 59 : 21) || !(status & 0x207) ||
        (status & ~0x207u)) {
        finish(1);
    }
    seen[unit] |= status;
    REG(base(unit) + 0x20) = status;
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
    REG(SYS + 0x30) = (1u << 2) | (1u << 3) | (1u << 12);
    REG(SYS + 0x20) = (1u << 18) | (1u << 19); /* Sourced XTAL only. */
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = 1u << 5;
    REG(SYS + 0x84) = MISSING_ROUTE ? 0 : 1u << 11;
    REG(0xe000e100) = 1u << 5;
    REG(0xe000e104) = 1u << 11;
    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = base(unit), begin = ticks;

        REG(b + 8) = 1;
        REG(b + 0x10) = 0x10000120; /* Preload ARR/CCR; URS excludes UG IRQ. */
        REG(b + 0x3c) = 25999;
        REG(b + 0x54) = 6500;
        REG(b + 0x58) = 13000;
        REG(b + 0x5c) = 19500;
        REG(b + 0x24) = 0x200;
        while (REG(b + 0x24)) {
        }
        if (REG(b + 0x7c) != 25999 || REG(b + 0x94) != 6500 ||
            REG(b + 0x20) || REG(b + 0x2c)) {
            finish(1);
        }
        REG(b + 0x1c) = 0x207;
        REG(b + 0x10) = 0x10000124;
        while (seen[unit] != 0x207 && ticks - begin < 20) {
            __asm__ volatile("wfi");
        }
        if (seen[unit] != 0x207) {
#if MISSING_ROUTE
            if (unit == 1 && seen[0] == 0x207 && !seen[1] &&
                (REG(b + 0x20) & 0x207) == 0x207 &&
                !(REG(SYS + 0x84) & (1u << 11))) {
                print("BK7258 MISSING PWM1 ROUTE DETECTED\n");
            }
#endif
            finish(1);
        }
        REG(b + 0x1c) = 0;
        REG(b + 0x10) = 0x10000120;
        REG(b + 0x20) = 0xfff;
    }
    finish(0);
}
