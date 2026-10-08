/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AI-assisted downstream experiment, not an upstream contribution. */
#include <stdint.h>
#include "uart_console.h"

/* Volatile guest MMIO/shared-memory transactions, not host model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define SHARED 0x28030000u
#define STAGE 0x72580001u

static void cp_start(void);
static void ap1_start(void);
static void ap2_start(void);
static void alternate_start(void);
static void fault(void);

__attribute__((section(".vectors.cp"), used))
const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)cp_start, [3] = (uintptr_t)fault,
};

__attribute__((section(".vectors.ap1"), used))
static const uintptr_t ap1_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)ap1_start, [3] = (uintptr_t)fault,
};

__attribute__((section(".vectors.ap2"), used))
static const uintptr_t ap2_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)ap2_start, [3] = (uintptr_t)fault,
};

__attribute__((aligned(512), used))
static const uintptr_t alternate_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)alternate_start,
    [3] = (uintptr_t)fault,
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
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
}

static void fault(void)
{
    console_init();
    bk7258_test_console_puts("BK7258 CORE RESET VECTOR FAILED\n");
    finish(1);
}

static void ap1_start(void)
{
    REG(SHARED + 4) = 1;
    while (!REG(SHARED + 12)) {
    }
    /* CPU1 restarts CPU0 at a real alternate guest vector table. */
    REG(SYS + 0x10) = (uintptr_t)alternate_vectors;
    REG(SYS + 0x10) = (uintptr_t)alternate_vectors | 1;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void ap2_start(void)
{
    REG(SHARED + 8) = 1;
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void alternate_start(void)
{
    console_init();
    if (REG(SHARED) != STAGE ||
        REG(SYS + 0x10) != ((uintptr_t)alternate_vectors | 1)) {
        fault();
    }
    REG(SHARED) = STAGE + 1;
    bk7258_test_console_puts("BK7258 CORE ALTERNATE VECTOR OK\n");
    /* Full-machine reset must restore the configured diagnostic boot vector. */
    REG(0xe000ed0c) = 0x05fa0004;
    for (;;) {
    }
}

static void cp_start(void)
{
    console_init();
    if (REG(SYS + 0x10) != ((uintptr_t)cp_vectors | 1) ||
        REG(SYS + 0x14) != 2 || REG(SYS + 0x18) != 10) {
        fault();
    }
    if (REG(SHARED) == STAGE + 1) {
        bk7258_test_console_puts("BK7258 CORE RESET VECTOR PASS\n");
        finish(0);
    }
    if (REG(SHARED)) {
        fault();
    }
    REG(SHARED) = STAGE;
    REG(SHARED + 4) = REG(SHARED + 8) = REG(SHARED + 12) = 0;
    REG(SYS + 0x14) = (uintptr_t)ap1_vectors | 1;
    REG(SYS + 0x18) = (uintptr_t)ap2_vectors | 1;
    while (!REG(SHARED + 4) || !REG(SHARED + 8)) {
    }
    bk7258_test_console_puts("BK7258 THREE CORE RESET VECTOR READY\n");
    REG(SHARED + 12) = 1;
    for (;;) {
        __asm__ volatile("wfi");
    }
}
