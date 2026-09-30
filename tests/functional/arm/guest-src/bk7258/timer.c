/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 native timer-group clock, snapshot and interrupt probe.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile MMIO and exception-handler communication. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define TIM0 0x44810000u
#define TIM1 0x45800000u
#define UART 0x44820000u
/* Independent deadline published by the SysTick exception handler. */
static volatile uint32_t ticks;
/* Timer exception handlers publish completion to the interrupted thread. */
static volatile uint32_t seen[2];
static void start(void);
static void fault(void);
static void systick(void);
static void timer0(void);
static void timer1(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [19] = (uintptr_t)timer0,
    [29] = (uintptr_t)timer1,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    const char *text = status ? "BK7258 TIMER PROBE FAILED\n" :
                               "BK7258 TIMER GROUP PROBE OK\n";
    uint32_t args[2] = {0x20026, status};

    bk7258_test_console_puts(text);

    /* Bind caller-clobbered arguments only after console calls return. */
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
#if FAULT_PROBE
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception == 3 && (REG(0xe000ed2c) & (1u << 30)) &&
        (REG(0xe000ed28) & 0x8200) == 0x8200 &&
        REG(0xe000ed38) == TIM0 + 0x20) {
        finish(0);
    }
#endif
    finish(1);
}

static void systick(void)
{
    if (++ticks > 200) {
        finish(1);
    }
}

static void wait_ticks(unsigned count)
{
    uint32_t begin = ticks;

    while (ticks - begin < count) {
        __asm__ volatile("wfi");
    }
}

static void timer0(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 19 || !(REG(TIM0 + 0x1c) & 0x80)) {
        finish(1);
    }
    REG(TIM0 + 0x1c) = 0x80;
    seen[0]++;
}

static void timer1(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 29 || !(REG(TIM1 + 0x1c) & 0x80)) {
        finish(1);
    }
    REG(TIM1 + 0x1c) = 0x80;
    seen[1]++;
}

static uint32_t snapshot(uint32_t base)
{
    REG(base + 0x20) = 1;
    while (REG(base + 0x20) & 1) {
    }
    return REG(base + 0x24);
}

static void start(void)
{
    uint32_t before, after;

    ticks = seen[0] = seen[1] = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 4) | (1u << 13);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent CPU-clock deadline. */
    REG(TIM0 + 8) = REG(TIM1 + 8) = 1;
#if FAULT_PROBE
    REG(TIM0 + 0x20) = 13; /* Invalid channel must produce a real BusFault. */
    finish(1);
#endif
    REG(TIM0 + 0x10) = 1000000;
    REG(TIM0 + 0x1c) = 1;
    wait_ticks(5);
    before = snapshot(TIM0);
    if (!before) {
        finish(1);
    }
    REG(SYS + 0x30) &= ~(1u << 4);
    REG(TIM0 + 0x20) = 1;
    wait_ticks(5);
    if (REG(TIM0 + 0x20) != 1 || REG(TIM0 + 0x24) != before) {
        finish(1);
    }
    REG(SYS + 0x30) |= 1u << 4;
    while (REG(TIM0 + 0x20) & 1) {
    }
    after = REG(TIM0 + 0x24);
    if (after < before || after - before > 16) {
        finish(1);
    }
    REG(SYS + 0x30) &= ~(1u << 4);
    REG(TIM0 + 0x20) = 1;
    REG(TIM0 + 8) = 0;
    REG(SYS + 0x30) |= 1u << 4;
    wait_ticks(2);
    if (REG(TIM0 + 0x20) || REG(TIM0 + 0x24) || REG(TIM0 + 0x1c)) {
        finish(1);
    }
    REG(TIM0 + 8) = 1;
    REG(SYS + 0x20) = 1u << 21; /* Group0 CLK32, group1 XTAL. */
    REG(TIM0 + 0x10) = 64;
    REG(TIM1 + 0x10) = 26000;
    REG(SYS + 0x80) = (1u << 3) | (MISSING_ROUTE ? 0 : (1u << 13));
    REG(0xe000e100) = (1u << 3) | (1u << 13);
    REG(TIM0 + 0x1c) = 1;
    REG(TIM1 + 0x1c) = 1 | (7u << 3); /* XTAL / 8, 8 ms period. */
    wait_ticks(30);
    if (seen[0] != 1 || seen[1] != 1 ||
        (REG(TIM0 + 0x1c) & 0x380) || (REG(TIM1 + 0x1c) & 0x380)) {
        finish(1);
    }
    finish(0);
}
