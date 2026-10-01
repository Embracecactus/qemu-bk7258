/*
 * SPDX-License-Identifier: Apache-2.0
 * QEMU warm-reset retained-pad regression, not a silicon reset-domain claim.
 * Adapted from the adjacent Apache-2.0 UART/exception fixture patterns.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile accesses target guest MMIO or reset-surviving SRAM. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define PMU 0x44000000u
#define PIN 0x44000408u /* P2 has no board contact or external pull. */
#define STATUS 0x44000500u
#define ROUTE (1u << (55 - 32))
#define LOCK (1u << 31)
#define MAGIC 0x47505253u

/*
 * Outside every loaded ELF segment, including diagnostic.ld's .bss at
 * 0x28010000. QEMU reloads the ELF on reset; this shared SRAM checkpoint
 * deliberately survives that reload under the existing machine model.
 */
#define CHECKPOINT 0x28020000u
/* Main and exception handlers exchange deadline and GPIO IRQ observations. */
static volatile uint32_t ticks, irq_count, irq_status, irq_exception;
static void start(void);
static void fault(void);
static void systick(void);
static void gpio_irq(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [16 + 55] = (uintptr_t)gpio_irq,
};

static void put_hex(uint32_t value)
{
    char text[9];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = 0;
    bk7258_test_console_puts(text);
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    REG(0xe000e010) = 0;
    __asm__ volatile("cpsid i" : : : "memory");
    bk7258_test_console_puts("BK7258 GPIO RESET RESULT ");
    put_hex(status);
    bk7258_test_console_puts(" END\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(0x10);
}

static void systick(void)
{
    if (++ticks >= 1000) {
        finish(0x11);
    }
}

static void check(int condition, unsigned error)
{
    if (!condition) {
        finish(error);
    }
}

static void commit(uint32_t value)
{
    REG(PMU) = value;
    REG(PMU + 0x94) = 0x424b55aa;
    REG(PMU + 0x94) = 0xbdb4aa55;
    check(REG(PMU + 0x1ec) == value, 0x12);
}

static __attribute__((noreturn)) void reset_to(unsigned phase)
{
    REG(CHECKPOINT) = MAGIC;
    REG(CHECKPOINT + 4) = phase;
    REG(CHECKPOINT + 8) = ~phase;
    __asm__ volatile("dsb" : : : "memory");
    REG(0xe000ed0c) = 0x05fa0004; /* Guest AIRCR.SYSRESETREQ. */
    __asm__ volatile("dsb\nisb" : : : "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void gpio_irq(void)
{
    uint32_t exception, status = REG(STATUS);

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(exception == 71 && status == 4 && !REG(STATUS + 4) &&
          (REG(PIN) & 1) && (REG(SYS + 0xa4) & ROUTE), 0x13);
    irq_exception = exception;
    irq_status = status;
    REG(STATUS) = 4;
    check(!REG(STATUS) && !(REG(SYS + 0xa4) & ROUTE), 0x14);
    irq_count++;
}

static void start(void)
{
    unsigned phase = 0;

    ticks = irq_count = irq_status = irq_exception = 0;
    REG(SYS + 0x30) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent 1ms exception deadline. */
    if (REG(CHECKPOINT) == MAGIC) {
        phase = REG(CHECKPOINT + 4);
        check(REG(CHECKPOINT + 8) == ~phase && phase >= 1 && phase <= 5,
              0x15);
    }
    check(!REG(STATUS + 4) && !REG(SYS + 0xa4), 0x16);
    if (phase == 0) {
        check(REG(PIN) == 0x28 && !REG(STATUS) && !REG(PMU + 0x1ec),
              0x20);
        REG(PIN) = 0x183c; /* Internal pull-up, rising IRQ, sampled high. */
        check(REG(PIN) == 0x183d && REG(STATUS) == 4, 0x21);
        REG(STATUS) = 4;
        check(!REG(STATUS), 0x22);
        commit(LOCK);
        check(!REG(STATUS) && REG(PIN) == 0x183d, 0x23);
        bk7258_test_console_puts("BK7258 GPIO RESET HIGH EDGE ARMED\n");
        reset_to(1);
    }
    if (phase == 1) {
        /* Software shadow resets; the locked effective pad remains high. */
        check(REG(PIN) == 0x29 && REG(PMU + 0x1ec) == LOCK, 0x24);
        if (REG(STATUS) == 4) {
            bk7258_test_console_puts(
                "BK7258 GPIO RESET PHANTOM RISING EDGE\n");
            finish(2); /* Unique observed regression, never a generic fault. */
        }
        check(!REG(STATUS), 0x25);
        bk7258_test_console_puts("BK7258 GPIO RESET RETAINED RISE OK\n");
        REG(PIN) = 0x182c; /* Locked write changes only the shadow. */
        check(REG(PIN) == 0x182d && !REG(STATUS), 0x26);
        commit(0); /* Apply pull-down: a genuine falling transition. */
        check(REG(PIN) == 0x182c && !REG(STATUS), 0x27);
        REG(SYS + 0x84) = ROUTE;
        REG(0xe000e104) = ROUTE;
        REG(PIN) = 0x183c; /* A genuine rising transition must still IRQ55. */
        while (!irq_count) {
            __asm__ volatile("wfi");
        }
        check(irq_count == 1 && irq_status == 4 && irq_exception == 71 &&
              !REG(STATUS) && !(REG(SYS + 0xa4) & ROUTE), 0x28);
        bk7258_test_console_puts("BK7258 GPIO RESET REAL IRQ55 OK\n");
        REG(0xe000e184) = ROUTE;
        REG(SYS + 0x84) = 0;
        REG(PIN) = 0x1c2c; /* Falling IRQ at a retained low pad. */
        check(REG(STATUS) == 4 && REG(PIN) == 0x1c2c, 0x29);
        REG(STATUS) = 4;
        commit(LOCK);
        check(!REG(STATUS), 0x2a);
        reset_to(2);
    }
    if (phase == 2) {
        check(REG(PIN) == 0x28 && !REG(STATUS) &&
              REG(PMU + 0x1ec) == LOCK, 0x30);
        bk7258_test_console_puts("BK7258 GPIO RESET RETAINED FALL OK\n");
        REG(PIN) = 0x143c;
        commit(0); /* Level-high still latches without an edge requirement. */
        check(REG(PIN) == 0x143d && REG(STATUS) == 4, 0x31);
        commit(LOCK);
        reset_to(3);
    }
    if (phase == 3) {
        check(REG(PIN) == 0x29 && REG(STATUS) == 4 &&
              REG(PMU + 0x1ec) == LOCK, 0x40);
        REG(STATUS) = 4;
        check(REG(STATUS) == 4, 0x41); /* Active level reasserts after W1C. */
        bk7258_test_console_puts(
            "BK7258 GPIO RESET RETAINED HIGH LEVEL OK\n");
        REG(PIN) = 0x102c;
        commit(0);
        check(REG(PIN) == 0x102c && REG(STATUS) == 4, 0x42);
        commit(LOCK);
        reset_to(4);
    }
    if (phase == 4) {
        check(REG(PIN) == 0x28 && REG(STATUS) == 4 &&
              REG(PMU + 0x1ec) == LOCK, 0x50);
        REG(STATUS) = 4;
        check(REG(STATUS) == 4, 0x51);
        bk7258_test_console_puts(
            "BK7258 GPIO RESET RETAINED LOW LEVEL OK\n");
        REG(PIN) = 0x183c;
        commit(0);
        REG(STATUS) = 4;
        check(REG(PIN) == 0x183d && !REG(STATUS), 0x52);
        reset_to(5); /* No GPIO retention lock. */
    }
    check(REG(PIN) == 0x28 && !REG(STATUS) && !REG(PMU + 0x1ec), 0x60);
    bk7258_test_console_puts("BK7258 GPIO RESET UNLOCKED DEFAULT OK\n");
    finish(0);
}
