/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 single-step digital-input SARADC probe. Uses the adjacent Apache-2.0
 * CKMN/timer/UART fixture patterns and the pinned SDK's mode-poll/data-read
 * ordering. No analog input, calibrated voltage or ADC IRQ is simulated.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Guest bus transactions must reach the device instead of being optimized. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define ADC 0x45890000u
#define TIM 0x44810000u
#define BUSY (1u << 29)
#define EMPTY (1u << 30)
/* Shared with SysTick/HardFault handlers, independently of the main loop. */
static volatile uint32_t ticks, samples, expected_address, before_fault;
static const uint16_t codes[] = {0, 4095, 291, 2048, 2730, 85};
static void start(void);
static void fault(void);
static void systick(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
};

static void hex(uint32_t value)
{
    char text[10];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
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
    bk7258_test_console_puts("BK7258 SARADC RESULT ");
    hex(status);
    hex(samples);
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

static void systick(void)
{
    if (++ticks > 2000) {
        finish(0x11);
    }
}

static uint32_t now(void)
{
    REG(TIM + 0x20) = 1;
    while (REG(TIM + 0x20) & 1) {
    }
    return REG(TIM + 0x24);
}

static void delay_ms(void)
{
    uint32_t begin = now();

    while (now() - begin < 26000) {
    }
}

static uint32_t control(unsigned channel)
{
    return 1 | 4 | (channel << 3) | (63 << 9) | (1 << 7);
}

static void power(int enable)
{
    REG(SYS + 0x108) = enable ? (1u << 15) : 0;
    while (REG(SYS + 0xe8) & 4) {
    }
}

static void consume(unsigned channel)
{
    uint32_t begin = now(), value;

    while (REG(ADC + 0x10) & 3) {
        check(now() - begin < 26000, 0x12);
    }
    check(!(REG(ADC + 0x10) & (BUSY | EMPTY)), 0x13);
    value = REG(ADC + 0x10000000 + 0x20);
    check(value == codes[channel - 1] &&
          (REG(ADC + 0x10) & EMPTY), 0x14);
    samples++;
    bk7258_test_console_puts("BK7258 SARADC SAMPLE ");
    hex(channel);
    hex(value);
    bk7258_test_console_puts("END\n");
}

static void fault(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(TEST_MODE && expected_address && exception == 3 &&
          (REG(0xe000ed2c) & (1u << 30)) &&
          (REG(0xe000ed28) & 0xff00) == 0x8200 &&
          REG(0xe000ed38) == expected_address &&
          REG(ADC + 0x10) == before_fault, 0x15);
    if (TEST_MODE == 1) {
        REG(SYS + 0x30) |= 1u << 5;
        consume(3); /* Rejected channel change did not replace this sample. */
    } else if (TEST_MODE == 3) {
        consume(1); /* Unsupported reset write did not destroy a result. */
    }
    bk7258_test_console_puts("BK7258 SARADC PRECISE FAULT STATE OK\n");
    finish(TEST_MODE);
}

static void start(void)
{
    REG(SYS + 0x30) = (1u << 2) | (1u << 4) | (1u << 5);
    REG(SYS + 0x20) = 1u << 20;
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    __asm__ volatile("cpsie i" : : : "memory");
    REG(TIM + 8) = 1;
    REG(TIM + 0x10) = 0xffffffff;
    REG(TIM + 0x1c) = 1;
    power(1);
    REG(ADC + 0x18) = (1u << 10) | (7u << 5);
    REG(ADC + 0x1c) = 7; /* Explicit [15:0] output selection, no rounding. */
    check(REG(ADC + 0x10) == EMPTY, 0x16);

    if (TEST_MODE) {
        if (TEST_MODE == 1) {
            REG(ADC + 0x10) = control(3);
            REG(SYS + 0x30) &= ~(1u << 5);
            check(REG(ADC + 0x10) & BUSY, 0x17);
            expected_address = ADC + 0x10;
            before_fault = REG(ADC + 0x10);
            REG(ADC + 0x10) = control(4);
        } else if (TEST_MODE == 2) {
            expected_address = ADC + 0x10;
            before_fault = REG(ADC + 0x10);
            /* Host intentionally omitted input6. */
            REG(ADC + 0x10) = control(6);
        } else {
            REG(ADC + 0x10) = control(1);
            while (REG(ADC + 0x10) & 3) {
            }
            expected_address = ADC + 8;
            before_fault = REG(ADC + 0x10);
            REG(ADC + 8) = 1;
        }
        finish(0x18); /* A silently accepted invalid transaction must fail. */
    }

    for (unsigned repeat = 0; repeat < 2; repeat++) {
        for (unsigned channel = 1; channel <= 6; channel++) {
            REG(ADC + 0x10 + (repeat ? 0x10000000 : 0)) = control(channel);
            check(REG(ADC + 0x10) & BUSY, 0x19);
            consume(channel);
        }
    }
    for (unsigned loss = 0; loss < 3; loss++) {
        REG(ADC + 0x10) = control(3);
        if (loss == 0) {
            REG(SYS + 0x30) &= ~(1u << 5);
        } else if (loss == 1) {
            REG(SYS + 0x20) |= 1u << 17;
        } else {
            power(0);
        }
        delay_ms(); /* Independent live TIMG source while ADC has no clock. */
        check((REG(ADC + 0x10) & (BUSY | EMPTY | 3)) ==
              (BUSY | EMPTY | 1), 0x1a);
        if (loss == 0) {
            REG(SYS + 0x30) |= 1u << 5;
        } else if (loss == 1) {
            REG(SYS + 0x20) &= ~(1u << 17);
        } else {
            power(1);
        }
        consume(3);
    }
    REG(ADC + 0x10) = control(4);
    REG(ADC + 0x10) &= ~4u;
    delay_ms();
    check(!(REG(ADC + 0x10) & BUSY) && (REG(ADC + 0x10) & EMPTY), 0x1b);
    REG(ADC + 0x10) = control(4);
    consume(4);
    check(samples == 16, 0x1c);
    bk7258_test_console_puts("BK7258 SARADC INPUT REARM CLOCK CANCEL OK\n");
    finish(0);
}
