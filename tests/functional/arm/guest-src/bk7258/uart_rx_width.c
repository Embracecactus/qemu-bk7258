/*
 * SPDX-License-Identifier: Apache-2.0
 * UART receive-width and FIFO-snapshot probes using normal guest MMIO.
 * Adapted from the adjacent Apache-2.0 bare-metal UART fixtures.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>

/* Volatile accesses are guest MMIO, not host model-state variables. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART (INDEX == 0 ? 0x44820000u : (0x45820000u + INDEX * 0x10000u))
#define IRQ (INDEX == 0 ? 4u : 14u + INDEX)
#define GATE (INDEX == 0 ? 2u : 9u + INDEX)
#define CONFIG(bits) ((259u << 8) | (((bits) - 5u) << 3) | 3u)

/* SysTick provides a bounded wait if the host never supplies a character. */
static volatile uint32_t ticks;
static void start(void);
static void fault(void);
static void tick(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)tick,
};

static void putchar_wait(uint8_t ch)
{
    REG(UART + 0x24) = 32;
    REG(UART + 0x1c) = ch;
    while (!(REG(UART + 0x24) & 32)) {
    }
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};
    const char *text = status ? "BK7258 UART RX WIDTH FAILED\n" :
                               "BK7258 UART RX WIDTH AND SNAPSHOT OK\n";

    REG(0xe000e010) = 0;
    REG(UART + 0x20) = 0;
    REG(UART + 0x10) = CONFIG(8);
    while (*text) {
        putchar_wait((uint8_t)*text++);
    }
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(1);
}

static void tick(void)
{
    if (++ticks > 2000) {
        finish(1);
    }
}

static void request_input(unsigned bits)
{
    const char *text = "BK7258 UART RX WIDTH ";

    REG(UART + 0x10) = CONFIG(8);
    while (*text) {
        putchar_wait((uint8_t)*text++);
    }
    putchar_wait('0' + bits);
    /*
     * Newline fits every tested width. Configure RX before transmitting it
     * so even an immediately scheduled host response sees the selected width.
     */
    REG(UART + 0x10) = CONFIG(bits);
    putchar_wait('\n');
}

static void start(void)
{
    static const uint8_t data[] = {0xff, 0x80, 0xe5, 0x55};

    ticks = 0;
    REG(SYS + 0x30) = 1u << GATE;
    REG(SYS + 0x80) = 1u << IRQ;
    REG(UART + 8) = 1;
    REG(UART + 0x14) = sizeof(data) << 8;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    for (unsigned bits = 5; bits <= 8; bits++) {
        REG(UART + 0x20) = 0;
        request_input(bits);
        while (((REG(UART + 0x18) >> 8) & 0xff) != sizeof(data)) {
        }
        if (!(REG(UART + 0x24) & 2) || REG(SYS + 0xa0)) {
            finish(1);
        }
        /* Neither widening nor narrowing may reinterpret queued values. */
        REG(UART + 0x10) = CONFIG(bits == 8 ? 5 : 8);
        REG(UART + 0x20) = 2;
        if (!(REG(SYS + 0xa0) & (1u << IRQ))) {
            finish(1);
        }
        for (unsigned i = 0; i < sizeof(data); i++) {
            uint32_t expected = (data[i] & ((1u << bits) - 1)) << 8;

            /* Check the stream model's zero-extended character value. */
            if (REG(UART + 0x1c) != expected) {
                finish(1);
            }
        }
        if ((REG(UART + 0x18) & (0xffu << 8)) ||
            !(REG(UART + 0x24) & 2)) {
            finish(1);
        }
        REG(UART + 0x24) = 2;
        if (REG(SYS + 0xa0)) {
            finish(1);
        }
    }
    finish(0);
}
