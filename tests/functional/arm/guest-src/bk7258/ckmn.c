/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 CKMN native IRQ/timing probe, derived from diagnostic.c, rtc.c and
 * timer.c's Apache-2.0 bare-metal exception, timer and UART fixture patterns.
 * The SDK's 64 ROSC-cycle window is 2 ms at the model's fixed 32 kHz source.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Guest MMIO and observations shared with exception handlers. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define RTC 0x44000200u
#define TIM0 0x44810000u
#define CKMN 0x448a0000u
#define ROUTE (1u << 21)
/* Main and handlers exchange timeout and captured IRQ measurements. */
static volatile uint32_t ticks, seen, exception_seen, rtc_seen, timer_seen;
static void start(void);
static void fault(void);
static void systick(void);
static void ckmn_irq(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [16 + 21] = (uintptr_t)ckmn_irq,
};

static void put_hex(uint32_t value)
{
    char text[10];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = ' ';
    text[9] = 0;
    bk7258_test_console_puts(text);
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    REG(0xe000e010) = 0;
    __asm__ volatile("cpsid i" : : : "memory");
    bk7258_test_console_puts("BK7258 CKMN RESULT ");
    put_hex(status);
    bk7258_test_console_puts("DONE\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void check(int condition, uint32_t status)
{
    if (!condition) {
        finish(status);
    }
}

static void fault(void)
{
    finish(0x10);
}

static void systick(void)
{
    if (++ticks > 100) {
        finish(0x11);
    }
}

static uint32_t snapshot(void)
{
    REG(TIM0 + 0x20) = 1;
    while (REG(TIM0 + 0x20) & 1) {
    }
    return REG(TIM0 + 0x24);
}

static void ckmn_irq(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    rtc_seen = REG(RTC + 0x0c);
    timer_seen = snapshot();
    check(exception == 37 && REG(CKMN + 0x20) == 1 &&
          REG(CKMN + 0x18) == 52000 && (REG(SYS + 0xa0) & ROUTE), 0x12);
    exception_seen = exception;
    REG(CKMN + 0x20) = 1;
    check(!REG(CKMN + 0x20) && !(REG(SYS + 0xa0) & ROUTE), 0x13);
    seen++;
}

static void start(void)
{
    uint32_t rtc_begin, timer_begin, begin, rtc_delta, timer_delta;

    ticks = seen = exception_seen = rtc_seen = timer_seen = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 4);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent 1 ms exception deadline. */
    REG(RTC) = 0x43;
    REG(RTC + 0x18) = REG(RTC + 4) = 0xffffffff;
    REG(RTC + 0x1c) = REG(RTC + 8) = 0xffffffff;
    while (REG(RTC + 0x20) != 0xffffffff ||
           REG(RTC + 0x10) != 0xffffffff ||
           REG(RTC + 0x24) != 0xffffffff ||
           REG(RTC + 0x14) != 0xffffffff) {
    }
    REG(RTC) = 0x40;
    REG(SYS + 0x20) |= 1u << 20; /* Select XTAL for timer group 0. */
    REG(TIM0 + 8) = 1;
    REG(TIM0 + 0x10) = 0xffffffff;
    REG(TIM0 + 0x1c) = 1; /* Separate 26 MHz timer counter. */
    REG(CKMN + 8) = 1;
    REG(CKMN + 0x10) = 64;
    REG(SYS + 0x80) = MISSING_ROUTE ? 0 : ROUTE;
    REG(0xe000e100) = ROUTE;
    rtc_begin = REG(RTC + 0x0c);
    timer_begin = snapshot();
    begin = ticks;
    REG(CKMN + 0x14) = 3;
    while (!seen && ticks - begin < 10) {
        __asm__ volatile("wfi");
    }
    if (MISSING_ROUTE) {
        rtc_delta = REG(RTC + 0x0c) - rtc_begin;
        timer_delta = snapshot() - timer_begin;
        check(!seen && !exception_seen && REG(CKMN + 0x20) == 1 &&
              REG(CKMN + 0x18) == 52000 && !(REG(SYS + 0xa0) & ROUTE) &&
              rtc_delta >= 288 && timer_delta >= 234000, 0x14);
        bk7258_test_console_puts("BK7258 CKMN EXPECTED SYS ROUTE FAILURE\n");
    } else {
        rtc_delta = rtc_seen - rtc_begin;
        timer_delta = timer_seen - timer_begin;
        begin = ticks;
        while (ticks - begin < 2) {
            __asm__ volatile("wfi");
        }
        check(seen == 1 && !REG(CKMN + 0x20) &&
              !(REG(SYS + 0xa0) & ROUTE), 0x16);
    }
    bk7258_test_console_puts("BK7258 CKMN SAMPLE ");
    put_hex(rtc_delta);
    put_hex(timer_delta);
    put_hex(exception_seen);
    put_hex(seen);
    put_hex(REG(CKMN + 0x18));
    put_hex(REG(CKMN + 0x20));
    bk7258_test_console_puts("END\n");
    if (!MISSING_ROUTE) {
        check(seen == 1 && exception_seen == 37 &&
              rtc_delta >= 63 && rtc_delta <= 65 &&
              timer_delta >= 51990 && timer_delta <= 52260, 0x15);
        bk7258_test_console_puts("BK7258 CKMN 2MS IRQ21 W1C OK\n");
    }
    finish(MISSING_ROUTE);
}
