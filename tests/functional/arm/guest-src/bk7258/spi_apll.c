/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bare-metal ideal audio-PLL SPI fixture, not production firmware.
 * Adapts the adjacent spi.c's SSI/IRQ probe, timer.c's TIMG snapshots and
 * diagnostic.c's startup/UART pattern (the latter adapted from contest
 * tests/host/bk7258/qemu/diagnostic.c at
 * ffe8356bf8b0e4eeaa504b6bcfaaf529bc842b43); see the adjacent LICENSE.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile MMIO and handler observations preserve transaction ordering. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define TIM 0x44810000u
#define DEADLINE_TIM 0x45800000u
#define ANA25 0xc2a0ae86u
#define TRIGGER (1u << 18)
#define DEADLINE 260000u /* Ten milliseconds on the independent XTAL TIMG. */
/* Under 2.5 us; far below the two profiles' separation. */
#define TOLERANCE 64u
#ifndef PROBE_MODE
#define PROBE_MODE 0
#endif

/* The real IRQ7 and IRQ17 handlers publish completion to the main loop. */
static volatile uint32_t seen[2];
/* Captured in each SPI handler, before any unrelated deadline wakeup. */
static volatile uint32_t completed[2];
static void start(void);
static void fault(void);
static void irq0(void);
static void irq1(void);
static void deadline_irq(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault, [23] = (uintptr_t)irq0,
    [29] = (uintptr_t)deadline_irq, [33] = (uintptr_t)irq1,
};

static uint32_t base(unsigned g)
{
    return 0x44870000u + 0x1010000u * g;
}

static void print(const char *text)
{
    bk7258_test_console_puts(text);
}

static void hex(uint32_t value)
{
    char text[9];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = 0;
    print(text);
}

static __attribute__((noreturn)) void finish(uint32_t status, const char *text)
{
    uint32_t args[2] = {0x20026, status};

    print(text);
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(1, "BK7258 SPI APLL PROBE FAILED\n");
}

static uint32_t snapshot(void)
{
    uint32_t mask, value;

    /* A SPI handler also snapshots TIMG0; never overlap CDC requests. */
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(mask) : : "memory");
    REG(TIM + 0x20) = 1;
    while (REG(TIM + 0x20) & 1) {
    }
    value = REG(TIM + 0x24);
    __asm__ volatile("msr primask, %0" : : "r"(mask) : "memory");
    return value;
}

static void wait_xtal(uint32_t count)
{
    uint32_t begin = snapshot();

    while (snapshot() - begin < count) {
        __asm__ volatile("wfi");
    }
}

static void analog_write(unsigned index, uint32_t value)
{
    uint32_t begin = snapshot();

    REG(SYS + 0x100 + 4 * index) = value;
    /* The pinned CP/AP defconfig polls SYS REG_3A, not a fabricated lock. */
    while (REG(SYS + 0xe8) & (1u << index)) {
        if (snapshot() - begin > 2600) {
            fault();
        }
    }
    if (REG(SYS + 0x100 + 4 * index) != value) {
        fault();
    }
}

static void handler(unsigned g)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != (g ? 33 : 23) ||
        (REG(base(g) + 0x18) & 0x7800) != 0x6000) {
        fault();
    }
    completed[g] = snapshot();
    REG(base(g) + 0x18) = 0x6000;
    seen[g]++;
}

static void irq0(void)
{
    handler(0);
}

static void irq1(void)
{
    handler(1);
}

static void deadline_irq(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 29 || !(REG(DEADLINE_TIM + 0x1c) & 0x80)) {
        fault();
    }
    REG(DEADLINE_TIM + 0x1c) = 0x81;
}

static void configure_profile(unsigned profile)
{
    analog_write(5, REG(SYS + 0x114) & ~(1u << 13));
    analog_write(25, ANA25);
    analog_write(26, PROBE_MODE == 3 ? 0x8973ca6eu :
                 (profile ? 0x88af2ec9u : 0x8973ca6fu));
    if (PROBE_MODE != 1) {
        /* Explicit ideal-model activation policy, not silicon lock timing. */
        analog_write(25, ANA25 | TRIGGER);
        if (PROBE_MODE != 2) {
            analog_write(25, ANA25);
        }
    }
    /* Identical committed writes must preserve an already active source. */
    analog_write(26, REG(SYS + 0x168));
}

static void prepare(unsigned g, uint8_t command, unsigned count)
{
    seen[g] = completed[g] = 0;
    REG(base(g) + 8) = 0;
    REG(base(g) + 8) = 3;
    /* Raw divider 255, zero byte interval, eight-bit four-wire master. */
    REG(base(g) + 0x10) = 0x40c0ff30;
    REG(base(g) + 0x1c) = command;
    for (unsigned i = 1; i < count; i++) {
        REG(base(g) + 0x1c) = 0;
    }
}

static uint32_t begin_transfer(unsigned g, unsigned count)
{
    uint32_t begin = snapshot();

    REG(base(g) + 0x14) = (count << 20) | (count << 8) | 15;
    return begin;
}

static uint32_t wait_transfer(unsigned g, uint32_t begin)
{
    uint32_t elapsed;

    while (!seen[g]) {
        elapsed = snapshot() - begin;
        if (elapsed >= DEADLINE) {
            return 0;
        }
        /* Independent periodic XTAL IRQ also wakes unavailable SPI sources. */
        __asm__ volatile("wfi");
    }
    if (seen[g] != 1) {
        fault();
    }
    return completed[g] - begin;
}

static void receive(unsigned g, unsigned count, int status)
{
    for (unsigned i = 0; i < count; i++) {
        uint32_t value;

        if (!(REG(base(g) + 0x18) & 4)) {
            fault();
        }
        value = REG(base(g) + 0x1c);
        if (i && (status < 0 ?
                  value != (i == 1 ? 0xef : i == 2 ? 0x40 : 0x16) :
                  value != (unsigned)status)) {
            fault();
        }
    }
    if (REG(base(g) + 0x18) & (4 | 0x7800)) {
        fault();
    }
    REG(base(g) + 0x14) = 0;
}

static uint32_t transfer(unsigned g, uint8_t command, unsigned count,
                         int status)
{
    uint32_t elapsed;

    prepare(g, command, count);
    elapsed = wait_transfer(g, begin_transfer(g, count));
    if (elapsed) {
        receive(g, count, status);
    }
    return elapsed;
}

static void gate_resume(void)
{
    for (unsigned paused = 0; paused < 2; paused++) {
        unsigned live = paused ^ 1;
        uint32_t gate = 1u << (paused ? 9 : 1);
        uint32_t begin;

        REG(SYS + 0x30) &= ~gate;
        prepare(paused, 5, 32);
        begin_transfer(paused, 32);
        if (!transfer(live, 5, 32, 0) || seen[paused] ||
            (REG(base(paused) + 0x18) & (4 | 0x7800))) {
            fault();
        }
        begin = snapshot();
        REG(SYS + 0x30) |= gate;
        if (!wait_transfer(paused, begin)) {
            fault();
        }
        receive(paused, 32, 0);
    }
    print("BK7258 SPI APLL INDEPENDENT GATE RESUME IRQ OK\n");
}

static void trigger_resume(void)
{
    uint32_t begin;

    for (unsigned g = 0; g < 2; g++) {
        prepare(g, 5, 32);
        begin_transfer(g, 32);
    }
    wait_xtal(2600); /* Some bytes finish before the shared source pauses. */
    analog_write(25, ANA25 | TRIGGER);
    wait_xtal(78000); /* Three milliseconds, longer than an entire frame. */
    if (seen[0] || seen[1]) {
        fault();
    }
    analog_write(25, ANA25);
    begin = snapshot();
    for (unsigned g = 0; g < 2; g++) {
        if (!wait_transfer(g, begin)) {
            fault();
        }
        receive(g, 32, 0);
    }
    print("BK7258 SPI APLL TRIGGER RESUME IRQ OK\n");
}

static void start(void)
{
    seen[0] = seen[1] = completed[0] = completed[1] = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 4) | (1u << 13) |
                     (1u << 1) | (1u << 9);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    /* Both timer groups use independent 26-MHz XTAL, not the audio PLL. */
    REG(SYS + 0x20) = (1u << 20) | (1u << 21);
    REG(TIM + 8) = 1;
    REG(TIM + 0x10) = 0xffffffffu;
    REG(TIM + 0x1c) = 1;
    REG(DEADLINE_TIM + 8) = 1;
    REG(DEADLINE_TIM + 0x10) = 2600; /* A bounded 100-us deadline wakeup. */
    REG(DEADLINE_TIM + 0x1c) = 1;
    REG(SYS + 0x80) = (1u << 7) | (1u << 13) | (1u << 17);
    REG(0xe000e100) = (1u << 7) | (1u << 13) | (1u << 17);
    REG(SYS + 0x28) = (1u << 4) | (1u << 5);

    for (unsigned profile = 0; profile < 2; profile++) {
        unsigned stalled = 0;

        configure_profile(profile);
        for (unsigned g = 0; g < 2; g++) {
            uint32_t elapsed, begin;
            /* Four JEDEC bytes stay within the defined flash ID response. */
            prepare(g, 0x9f, 4);
            begin = begin_transfer(g, 4);
            if (!wait_transfer(g, begin)) {
                if (seen[g] || (REG(base(g) + 0x18) & (4 | 0x7800))) {
                    fault();
                }
                elapsed = snapshot() - begin;
                REG(base(g) + 0x14) = 0;
                stalled |= 1u << g;
                print("BK7258 SPI APLL DEADLINE ");
                hex(g);
                print(" ");
                hex(elapsed);
                print("\n");
                continue;
            }
            receive(g, 4, -1);
            /* WREN/RDSR/WRDI prove defined status bytes from the real slave. */
            if (!transfer(g, 6, 1, 0)) {
                fault();
            }
            elapsed = transfer(g, 5, 32, 2);
            print("BK7258 SPI APLL SAMPLE ");
            hex(profile);
            print(" ");
            hex(g);
            print(" ");
            hex(elapsed);
            print("\n");
            /* 32 * 8 * (2 * 255) input clocks, scaled by the XTAL oracle. */
            uint32_t expected = profile ? 37585 : 34531;

            if (elapsed + TOLERANCE < expected ||
                elapsed > expected + TOLERANCE ||
                !transfer(g, 4, 1, 0) || !transfer(g, 5, 2, 0)) {
                fault();
            }
        }
        if (stalled) {
            if (stalled != 3) {
                fault();
            }
            finish(1, "BK7258 SPI APLL XTAL DEADLINE FAILED\n");
        }
    }
    gate_resume();
    trigger_resume();
    finish(0, "BK7258 SPI APLL SSI ID STATUS TIMING IRQ OK\n");
}
