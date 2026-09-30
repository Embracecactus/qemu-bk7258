/*
 * BK7258 unsupported SYS-access fault probe.
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from the adjacent diagnostic.c's bare-metal UART/semihosting setup.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>

/* Real guest MMIO accesses, never host device-model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define UART 0x44820000u
#if WRITE_PROBE
#define PROBE_ADDRESS 0x540100a0u
#else
#define PROBE_ADDRESS 0x44010000u
#endif

static void start(void);
static void fault(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
};

static __attribute__((noreturn)) void finish(uint32_t status)
{
    const char *text = status ? "BK7258 SYS ACCESS FAULT FAILED\n" :
                               "BK7258 SYS ACCESS FAULT OK\n";
    uint32_t args[2] = {0x20026, status};
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;

    while (*text) {
        while (!(REG(UART + 0x18) & (1u << 20))) {
        }
        REG(UART + 0x1c) = (uint8_t)*text++;
    }
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    /* Precise BusFault escalation must record the actual fault address. */
    if (exception != 3 || !(REG(0xe000ed2c) & (1u << 30)) ||
        (REG(0xe000ed28) & 0xff00) != 0x8200 ||
        REG(0xe000ed38) != PROBE_ADDRESS) {
        finish(1);
    }
    finish(0);
}

static void start(void)
{
    REG(0x44010030) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
#if WRITE_PROBE
    /* SYS IRQ status is not a writable register; use the NS address alias. */
    REG(PROBE_ADDRESS) = 0x12345678;
#else
    /* Device identity is absent from this model, not a fabricated zero ID. */
    (void)REG(PROBE_ADDRESS);
#endif
    finish(1); /* Silent read-zero/ignored-write behavior must fail the test. */
}
