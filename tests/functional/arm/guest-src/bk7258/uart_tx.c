/*
 * SPDX-License-Identifier: Apache-2.0
 * Timed UART FIFO/IRQ and precise full-FIFO transaction-fault probes.
 * Adapted from the adjacent Apache-2.0 bare-metal IRQ/fault fixtures.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile guest MMIO transactions, never host model-state accesses. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART0 0x44820000u
#define UART (INDEX == 0 ? UART0 : (0x45820000u + INDEX * 0x10000u))
#define IRQ (INDEX == 0 ? 4u : 14u + INDEX)
#define GATE (INDEX == 0 ? 2u : 9u + INDEX)
/* Updated asynchronously by the real SysTick exception handler. */
static volatile uint32_t ticks;
/* IRQ delivery count communicated between handler and main probe. */
static volatile uint32_t seen;
/* Main selects the status source checked by the IRQ handler. */
static volatile uint32_t phase;
static void start(void);
static void fault(void);
static void tick(void);
static void irq(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)tick,
    [20] = (uintptr_t)irq,
    [31] = (uintptr_t)irq,
    [32] = (uintptr_t)irq,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    REG(0xe000e010) = 0;
    REG(UART + 0x20) = 0;
    REG(SYS + 0x80) = 0;
    /* Console recovery also discards undelivered bytes after the fault. */
    REG(UART + 8) = 0;
    REG(SYS + 0x30) |= 1u << 2;
    REG(UART0 + 8) = 1;
    REG(UART0 + 0x10) = 0xe11b;
    bk7258_test_console_puts(status ? "BK7258 UART TX PROBE FAILED\n" :
        (FULL_FIFO ? "BK7258 UART FIFO PRECISE FAULT OK\n" :
                     "BK7258 UART TX FIFO AND IRQ OK\n"));
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (!FULL_FIFO || exception != 3 || !(REG(0xe000ed2c) & (1u << 30)) ||
        (REG(0xe000ed28) & 0xff00) != 0x8200 ||
        REG(0xe000ed38) != UART + 0x1000001c ||
        (REG(UART + 0x18) & 0x11ffff) != 0x10080 ||
        REG(UART + 0x24)) {
        finish(1);
    }
    finish(0);
}

static void tick(void)
{
    if (++ticks > 200) {
        finish(1);
    }
}

static void irq(void)
{
    uint32_t exception, expected = phase == 1 ? 32u : 1u;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 16 + IRQ || !(REG(UART + 0x24) & expected)) {
        finish(1);
    }
    REG(UART + 0x20) = 0; /* Service level remains latched until refill. */
    REG(UART + 0x24) = expected;
    seen++;
}

static void queue(const char *text)
{
    while (*text) {
        if (!(REG(UART + 0x18) & (1u << 20))) {
            finish(1);
        }
        REG(UART + 0x1c) = (uint8_t)*text++;
    }
}

static void wait_seen(unsigned count)
{
    uint32_t limit = ticks + 10;

    while (seen < count && ticks < limit) {
        __asm__ volatile("wfi");
    }
    if (seen != count) {
        finish(1);
    }
}

static void start(void)
{
    ticks = seen = 0;
    phase = 1;
    REG(SYS + 0x30) = (1u << 2) | (1u << GATE);
    REG(UART0 + 8) = 1;
    REG(UART0 + 0x10) = 0xe11b;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = ((FULL_FIFO ? 65535u : 259u) << 8) | 0x19;
    if (FULL_FIFO) {
        for (unsigned i = 0; i < 129; i++) {
            REG(UART + 0x1c) = 'x';
        }
        REG(UART + 0x1000001c) = 'y';
        finish(1); /* Ignored overflow or synchronous drain must fail. */
    }
    REG(SYS + 0x80) = MISSING_ROUTE ? 0 : 1u << IRQ;
    REG(0xe000e100) = 1u << IRQ;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    queue("U" INDEX_TEXT ":A\n");
    while (!(REG(UART + 0x24) & 32)) {
    }
    if (seen) {
        finish(1); /* Peripheral mask must withhold delivery. */
    }
    REG(UART + 0x20) = 32;
    if (MISSING_ROUTE) {
        uint32_t limit = ticks + 3;

        while (ticks < limit) {
            __asm__ volatile("wfi");
        }
        if (seen || !(REG(UART + 0x24) & 32)) {
            finish(1);
        }
        bk7258_test_console_puts("BK7258 MISSING UART ROUTE DETECTED\n");
        finish(1);
    }
    wait_seen(1);
    phase = 2;
    queue("s" INDEX_TEXT ":B\n"); /* One shifter plus four queued bytes. */
    REG(UART + 0x14) = 4;
    REG(UART + 0x20) = 1;
    wait_seen(2);
    while (!(REG(UART + 0x24) & 32)) {
    }
    if (REG(UART + 0x18) & 0xff) {
        finish(1);
    }
    finish(0);
}
