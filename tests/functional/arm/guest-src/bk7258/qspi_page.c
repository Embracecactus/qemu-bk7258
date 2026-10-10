/* SPDX-License-Identifier: Apache-2.0
 * Single-page sequence compatibility with SDK cb080de1 single helpers.
 * Single calls avoid their uint32_t pointer/byte-count cross-block defect.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"
#define R(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u

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
    bk7258_test_console_puts(status ? "QSPI FAILED\n" : "QSPI PAGE OK\n");
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
    finish(1);
}
__attribute__((naked)) static void fault(void)
{
    __asm__ volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b fault_report");
}
static void tick(void) { if (++ticks > 200) { finish(2); } }
/* Equivalent MMIO for fixed SDK LL, not an unmodified SDK binary. */
#ifndef PAGE_LENGTH
#define PAGE_LENGTH 256
#endif
#ifndef PROFILE_48
#define PROFILE_48 0
#endif
static void command(uint32_t b, unsigned read, unsigned opcode,
                    unsigned addr, unsigned len, unsigned addressed)
{
    uint32_t c = b + (read ? 0x50 : 0x40);
    R(c) = 0;
    R(c + 4) = opcode | (addressed ? ((addr >> 16) & 255) << 8 |
        ((addr >> 8) & 255) << 16 | (addr & 255) << 24 : 0);
    R(c + 8) = addressed ? 0x300 : 0xc;
    R(c + 12) = (R(c + 12) & ~0xffcu) | (len << 2);
    R(c + 12) &= ~(3u << 14);
    R(c + 12) &= ~(127u << 16);
    R(c + 12) &= ~(7u << 24);
    R(c + 12) |= 1;
    while (!(R(b + 0x70) & 4)) {}
    if (R(c + 12) & 1) { finish(4); }
    R(b + 0x6c) = 4; R(b + 0x6c) = 0;
    if (R(b + 0x70) & 4) { finish(3); }
}
static unsigned status(uint32_t b)
{
    command(b, 1, 5, 0, 1, 0);
    return R(b + 0x100) & 255;
}
static void wait_wip(uint32_t b)
{
    /* Separate target status command; controller completion is insufficient. */
    while (status(b) & 1) {}
}
static void start(void)
{
    R(SYS + 0x30) = 4;
    R(0x44820008) = 1; R(0x44820010) = 0xe11b;
    R(0xe000e014) = 25999; R(0xe000e018) = 0; R(0xe000e010) = 7;
    R(SYS + 0x114) = 1u << 5;
    while (R(SYS + 0xe8) & (1u << 5)) {}
    R(SYS + 0x24) = PROFILE_48 ? 0x640 : 0x500;
    R(SYS + 0x28) = PROFILE_48 ? 0x640 : 0x500;
    R(SYS + 0x30) |= 3u << 20;
    for (unsigned g = 0; g < 2; g++) {
        uint32_t b = 0x46040000u + g * 0x20000u;
        R(b + 0x60) = PROFILE_48 ? 9 : 0x209;
        command(b, 1, 0x9f, 0, 3, 0);
        if ((R(b + 0x100) & 0xffffff) != 0x1640ef) { finish(5); }
        for (unsigned page = 0; page < 2; page++) {
            unsigned addr = 0x1000 + page * 0x100;
            uint32_t data[64];
            for (unsigned j = 0; j < 64; j++) { data[j] = 0xa5a5a5a5; }
            for (unsigned j = 0; j < PAGE_LENGTH; j++) {
                ((uint8_t *)data)[j] = (uint8_t)(j * 37 + 17 * page + 83 * g);
            }
            command(b, 0, 6, 0, 0, 0);
            wait_wip(b); /* Fixed SDK wren helper includes this status read. */
            if (!(R(b + 0x100) & 2)) { finish(7); }
            for (unsigned j = 0; j < (PAGE_LENGTH + 3) / 4; j++) {
                R(b + 0x100 + 4 * j) = data[j];
            }
            command(b, 0, 2, addr, PAGE_LENGTH, 1);
            wait_wip(b);
            for (unsigned j = 0; j < (PAGE_LENGTH + 3) / 4; j++) {
                R(b + 0x100 + j * 4) = 0xa5a5a5a5;
            }
            command(b, 1, 3, addr, PAGE_LENGTH, 1);
            for (unsigned j = 0; j < (PAGE_LENGTH + 3) / 4; j++) {
                if (R(b + 0x100 + j * 4) != data[j]) { finish(8); }
            }
        }
        /* Read page 0 again after page 1: independent destination contents. */
        command(b, 1, 3, 0x1000, PAGE_LENGTH, 1);
        for (unsigned j = 0; j < PAGE_LENGTH; j++) {
            unsigned byte = R(b + 0x100 + (j / 4) * 4) >> (8 * (j % 4));
            if ((byte & 255) != ((j * 37 + 83 * g) & 255)) { finish(9); }
        }
    }
    finish(0);
}
