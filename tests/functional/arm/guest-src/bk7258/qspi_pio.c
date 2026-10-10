/* SPDX-License-Identifier: Apache-2.0
 * Source-built, polling QSPI/SSI NOR contract probe. Not a product firmware.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"
#define R(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#ifndef BAD_DUMMY
#define BAD_DUMMY 0
#endif
static volatile unsigned armed;
static void start(void), tick(void), fault(void);
static volatile unsigned ticks;
__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000, [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault, [15] = (uintptr_t)tick,
};
static void hex(uint32_t v)
{
    char s[10];
    for (unsigned i = 0; i < 8; i++) {
        s[i] = "0123456789abcdef"[(v >> (28 - i * 4)) & 15];
    }
    s[8] = ' '; s[9] = 0; bk7258_test_console_puts(s);
}
static __attribute__((noreturn)) void finish(unsigned status)
{
    uint32_t args[] = {0x20026, status};
    bk7258_test_console_puts(status ? "QSPI FAILED\n" : "QSPI PIO OK\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {}
}
__attribute__((used, noinline, noreturn)) static void fault_report(uint32_t *sp)
{
    bk7258_test_console_puts("QSPI FAULT PC BFAR CFSR ");
    hex(sp[6]); hex(R(0xe000ed38)); hex(R(0xe000ed28));
    bk7258_test_console_puts("\n");
    if (BAD_DUMMY && armed && R(0xe000ed38) == 0x4604005c &&
        (R(0xe000ed28) & 0xff00) == 0x8200 && R(0x4604005c) == 0) {
        bk7258_test_console_puts("QSPI DUMMY PRECISE FAULT OK\n"); finish(0);
    }
    finish(1);
}
__attribute__((naked)) static void fault(void)
{
    __asm__ volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b fault_report");
}
static void tick(void) { if (++ticks > 200) { finish(2); } }
static void command(uint32_t b, unsigned read, unsigned opcode,
                    unsigned addr, unsigned len, unsigned addressed)
{
    uint32_t c = b + (read ? 0x50 : 0x40);
    R(b + 0x6c) = 4; R(b + 0x6c) = 0;
    if (R(b + 0x70) & 4) { finish(3); }
    R(c) = 0;
    R(c + 4) = opcode | (addressed ? ((addr >> 16) & 255) << 8 |
        ((addr >> 8) & 255) << 16 | (addr & 255) << 24 : 0);
    R(c + 8) = addressed ? 0x300 : 0xc;
    R(c + 12) = (len << 2) | 1;
    while (!(R(b + 0x70) & 4)) {}
    if (R(c + 12) & 1) { finish(4); }
}
static void start(void)
{
    static const unsigned lengths[] = {1, 3, 4, 5, 31, 32};
    R(SYS + 0x30) = 4;
    R(0x44820008) = 1; R(0x44820010) = 0xe11b;
    R(0xe000e014) = 25999; R(0xe000e018) = 0; R(0xe000e010) = 7;
    /* Deliberately first: baseline must expose the actual absent device. */
    R(0x46040060) = 9;
    R(SYS + 0x114) = 1u << 5;
    while (R(SYS + 0xe8) & (1u << 5)) {}
    R(SYS + 0x24) = (1u << 10) | (9u << 6);
    R(SYS + 0x28) = (1u << 10) | (9u << 6);
    R(SYS + 0x30) |= 3u << 20;
    if (BAD_DUMMY) {
        R(0x46040054) = 0x9f; R(0x46040058) = 0xc;
        armed = 1; R(0x4604005c) = 0x1000d; finish(11);
    }
    for (unsigned g = 0; g < 2; g++) {
        uint32_t b = 0x46040000u + g * 0x20000u;
        R(b + 0x60) = 9;
        R(b + 0x120) = 0x5a5a5a5a;
        command(b, 1, 0x9f, 0, 3, 0);
        if ((R(b + 0x100) & 0xffffff) != 0x1640ef) { finish(5); }
        command(b, 1, 5, 0, 1, 0);
        if (R(b + 0x100) & 3) { finish(6); }
        for (unsigned k = 0; k < 6; k++) {
            unsigned n = lengths[k], addr = 0x100 + k * 0x100;
            uint32_t data[9];
            for (unsigned j = 0; j < 9; j++) { data[j] = 0xa5a5a5a5; }
            for (unsigned j = 0; j < n; j++) {
                ((uint8_t *)data)[j] = (uint8_t)(j + 17 * k + 83 * g);
            }
            command(b, 0, 6, 0, 0, 0);
            command(b, 1, 5, 0, 1, 0);
            if (!(R(b + 0x100) & 2)) { finish(7); }
            for (unsigned j = 0; j < (n + 3) / 4; j++) {
                R(b + 0x100 + 4 * j) = data[j];
            }
            R(b + 0x100 + ((n + 3) & ~3u)) = 0x5a5a5a5a;
            command(b, 0, 2, addr, n, 1);
            for (unsigned j = 0; j < 8; j++) { R(b + 0x100 + j * 4) = 0xa5a5a5a5; }
            command(b, 1, 3, addr, n, 1);
            for (unsigned j = 0; j < (n + 3) / 4; j++) {
                if (R(b + 0x100 + j * 4) != data[j]) { finish(8); }
            }
            if (R(b + 0x120) != 0x5a5a5a5a) { finish(10); }
            /* Read the first byte beyond the programmed range: no padding. */
            command(b, 1, 3, addr + n, 1, 1);
            if ((R(b + 0x100) & 255) != 255) { finish(9); }
        }
    }
    finish(0);
}
