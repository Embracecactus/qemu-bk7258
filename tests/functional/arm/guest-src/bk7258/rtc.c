/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 RTC native IRQ/clock probe, using diagnostic.c's bare-metal setup.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>

/* Real guest MMIO and state shared with exception handlers. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define RTC 0x44000200u
#define UART 0x44820000u
/* The SysTick handler publishes the independent timeout to the thread. */
static volatile uint32_t ticks;
/* The RTC handler publishes completion to the interrupted thread. */
static volatile uint32_t seen;
static void start(void);
static void fault(void);
static void systick(void);
static void rtc_irq(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [70] = (uintptr_t)rtc_irq,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    const char *text = status ? "BK7258 RTC PROBE FAILED\n" :
                               "BK7258 RTC IRQ CLOCK STOP OK\n";
    uint32_t args[2] = {0x20026, status};
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;

    while (*text) {
        REG(UART + 0x1c) = (uint8_t)*text++;
    }
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
    ticks++;
    if (ticks > 200) {
        fault();
    }
}

static void wait_ticks(unsigned count)
{
    uint32_t begin = ticks;

    while (ticks - begin < count) {
        __asm__ volatile("wfi");
    }
}

static void rtc_irq(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 70 || !(REG(RTC) & 0x20)) {
        fault();
    }
    REG(RTC) = 0x68;
    seen++;
}

static void start(void)
{
    uint32_t paused, begin;

    ticks = seen = 0;
    REG(0x44010030) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent CPU-clock deadline. */
    REG(RTC) = 0x43;
    REG(RTC + 0x18) = 0xffffffff;
    REG(RTC + 4) = 0xffffffff;
    REG(RTC + 0x1c) = 0;
    REG(RTC + 8) = 1024;
    while (REG(RTC + 0x20) != 0xffffffff ||
           REG(RTC + 0x10) != 0xffffffff || REG(RTC + 0x14) != 1024) {
    }
    REG(RTC) = 0x40;
    wait_ticks(5);
    REG(RTC) = 0x42;
    paused = REG(RTC + 0x0c);
    wait_ticks(5);
    if (!paused || REG(RTC + 0x0c) != paused) {
        fault();
    }
    REG(0x44000104) = 1;
    REG(RTC) = 0x40;
    wait_ticks(5);
    if (REG(RTC + 0x0c) != paused) {
        fault();
    }
    REG(0x44000104) = 0;
    REG(RTC + 8) = REG(RTC + 0x0c) + 64;
#if !MISSING_ROUTE
    REG(0x44010084) = 1u << 22;
#endif
    REG(0xe000e104) = 1u << 22;
    REG(RTC) = 0x48;
    begin = ticks;
    while (!seen && ticks - begin < 20) {
        __asm__ volatile("wfi");
    }
    if (seen != 1 || (REG(RTC) & 0x20)) {
        fault();
    }
    REG(RTC) = 0x41;
    wait_ticks(2);
    if (REG(RTC + 0x0c) || REG(RTC + 0x28)) {
        fault();
    }
    finish(0);
}
