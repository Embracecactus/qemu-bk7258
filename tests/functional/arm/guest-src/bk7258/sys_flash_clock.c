/* SPDX-License-Identifier: Apache-2.0
 * SYS Flash configuration test, not a Flash clock/performance test.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
static void start(void), fault(void);
static volatile uint32_t armed;
__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000, [1] = (uintptr_t)start, [3] = (uintptr_t)fault,
};
static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};
    bk7258_test_console_puts(status ? "BK7258 FLASH CONFIG FAILED\n" :
                                    "BK7258 FLASH CONFIG OK\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {}
}
static void fault(void)
{
    if (!armed || !(REG(0xe000ed2c) & (1u << 30)) ||
        (REG(0xe000ed28) & 0xff00) != 0x8200 ||
        REG(0xe000ed38) != 0x54010024u || REG(SYS + 0x24) != 0x0f000000u) {
        finish(1);
    }
    finish(0);
}
static void start(void)
{
    REG(SYS + 0x30) = 1u << 2;
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    if (REG(SYS + 0x24) != 0) { finish(1); }
    for (unsigned sel = 0; sel < 4; sel++) {
        for (unsigned div = 0; div < 4; div++) {
            REG(SYS + 0x24) = (REG(SYS + 0x24) & ~0x03000000u) | (sel << 24);
            REG(0x54010024) = (REG(0x54010024) & ~0x0c000000u) | (div << 26);
            if (REG(SYS + 0x24) != ((sel << 24) | (div << 26)) ||
                REG(0x54010024) != REG(SYS + 0x24)) { finish(1); }
        }
    }
#if BAD_WRITE
    armed = 1;
    /* Reject the whole transaction, including otherwise valid clear bits. */
    REG(0x54010024) = 1;
    finish(1);
#else
    REG(SYS + 0x24) = 0;
    if (REG(0x54010024) != 0) { finish(1); }
    finish(0);
#endif
}
