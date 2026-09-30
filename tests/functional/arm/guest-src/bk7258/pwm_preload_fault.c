/*
 * SPDX-License-Identifier: Apache-2.0
 * Reject a PWM enable that would form an unsupported mixed shadow pair.
 * Adapted from the adjacent Apache-2.0 sys_fault.c bare-metal harness.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Guest register transactions, not direct model-state changes. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define PWM (0x458a0000u + UNIT * 0x50000u)
#define OLD_ARR (KEEP_ARR ? 9u : 25u)
#define OLD_CCR (KEEP_ARR ? 2u : 13u)
#define NEW_ARR (KEEP_ARR ? 25u : 9u)
#define NEW_CCR (KEEP_ARR ? 13u : 2u)

static void start(void);
static void fault(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t vectors[16] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    const char *text = status ? "BK7258 PWM MIXED PRELOAD FAILED\n" :
                               "BK7258 PWM MIXED PRELOAD FAULT ATOMIC OK\n";
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
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 3 || !(REG(0xe000ed2c) & (1u << 30)) ||
        (REG(0xe000ed28) & 0xff00) != 0x8200 ||
        REG(0xe000ed38) != PWM + 0x10 ||
        REG(PWM + 0x10) != 0x120 || REG(PWM + 0x2c) ||
        REG(PWM + 0x3c) != NEW_ARR || REG(PWM + 0x54) != NEW_CCR ||
        REG(PWM + 0x7c) != OLD_ARR || REG(PWM + 0x94) != OLD_CCR ||
        REG(PWM + 0x20)) {
        finish(1);
    }
    finish(0);
}

static void start(void)
{
    REG(SYS + 0x30) = (1u << 2) | (1u << (UNIT ? 12 : 3));
    REG(SYS + 0x20) = 1u << (18 + UNIT);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(PWM + 8) = 1;
    REG(PWM + 0x10) = 0x120;
    REG(PWM + 0x3c) = OLD_ARR;
    REG(PWM + 0x54) = OLD_CCR;
    REG(PWM + 0x24) = 0x200;
    for (unsigned n = 0; n < 100000 && REG(PWM + 0x24); n++) {
    }
    if (REG(PWM + 0x24) || REG(PWM + 0x7c) != OLD_ARR ||
        REG(PWM + 0x94) != OLD_CCR) {
        finish(1);
    }
    REG(PWM + 0x20) = 0xfff;
    REG(PWM + 0x3c) = NEW_ARR;
    REG(PWM + 0x54) = NEW_CCR;
    /* Pending and old pairs are valid; the effective mixed pair is not. */
    REG(PWM + 0x10) = KEEP_ARR ? 0x24 : 0x104;
    finish(1); /* A silently accepted configuration must fail this probe. */
}
