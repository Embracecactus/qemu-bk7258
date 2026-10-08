/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AI-assisted downstream experiment, not an upstream contribution. */
#include <stdint.h>
#include "uart_console.h"

/* Volatile guest MMIO transactions, not host model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u

static void primary_start(void);
static void stale_start(void);

__attribute__((section(".vectors.cp"), used))
const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)primary_start,
    [3] = (uintptr_t)stale_start,
};

__attribute__((section(".vectors.ap1"), used))
static const uintptr_t ap1_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)stale_start,
    [3] = (uintptr_t)stale_start,
};

__attribute__((section(".vectors.ap2"), used))
static const uintptr_t ap2_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)stale_start,
    [3] = (uintptr_t)stale_start,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = { 0x20026, status };
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;

    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void console_init(void)
{
    REG(SYS + 0x30) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
}

static void stale_start(void)
{
    console_init();
    bk7258_test_console_puts("BK7258 STALE CORE CONTROL EXECUTED\n");
    finish(1);
}

static void primary_start(void)
{
    console_init();
    if (REG(SYS + 0x10) != ((uintptr_t)cp_vectors | 1) ||
        REG(SYS + 0x14) != 2 || REG(SYS + 0x18) != 10) {
        stale_start();
    }
    /* A live secondary must fail through its own executed reset handler. */
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 5;
    for (unsigned i = 0; i < 3; i++) {
        while (!(REG(0xe000e010) & (1u << 16))) {
        }
    }
    REG(0xe000e010) = 0;
    bk7258_test_console_puts("BK7258 PENDING CORE RESET READY\n");
    /* Let the host finish its cont reply before the semihosting exit. */
    while (!(REG(UART + 0x18) & (1u << 21))) {
    }
    if (((REG(UART + 0x1c) >> 8) & 0xff) != 'X') {
        stale_start();
    }
    bk7258_test_console_puts("BK7258 PENDING CORE RESET PASS\n");
    finish(0);
}
