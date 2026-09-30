/*
 * SPDX-License-Identifier: Apache-2.0
 * BK7258 native-bus I2C probe with externally supplied test EEPROMs.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>

/* Volatile guest MMIO accesses and exception-handler communication. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define ACK 0x100u
#define START 0x400u
#define STOP 0x200u
/* CPU-clock deadline published by the SysTick handler. */
static volatile uint32_t ticks;
/* Completion observations published by actual IRQ6/14 exception handlers. */
static volatile uint32_t seen[2];
static void start(void);
static void fault(void);
static void systick(void);
static void irq0(void);
static void irq1(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [22] = (uintptr_t)irq0,
    [30] = (uintptr_t)irq1,
};

static uint32_t base(unsigned g)
{
    return 0x45850000u + 0x10000u * g;
}

static unsigned irq(unsigned g)
{
    return g ? 14 : 6;
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    const char *text = status ? "BK7258 I2C PROBE FAILED\n" :
                               "BK7258 I2C BUS IRQ RESTART NAK OK\n";
    uint32_t args[2] = {0x20026, status};
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;

    while (*text) {
        REG(UART + 0x1c) = (uint8_t)*text++;
    }
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(1);
}

static void systick(void)
{
    if (++ticks > 1000) {
        finish(1);
    }
}

static void handler(unsigned g)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (exception != 16 + irq(g) || !(REG(base(g) + 0x14) & 1)) {
        finish(1);
    }
    REG(0xe000e180) = 1u << irq(g);
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

static void issue(unsigned g, uint32_t command)
{
    uint32_t begin = ticks, previous = seen[g];

    REG(base(g) + 0x14) = command;
    /* The handler masked a level source before the thread serviced it. */
    REG(0xe000e280) = 1u << irq(g);
    __asm__ volatile("dsb sy; isb" : : : "memory");
    REG(0xe000e100) = 1u << irq(g);
    while (seen[g] == previous && ticks - begin < 20) {
        __asm__ volatile("wfi");
    }
    if (seen[g] != previous + 1 || !(REG(base(g) + 0x14) & 1)) {
#if MISSING_ROUTE
        if (g == 1 && seen[0] && seen[1] == 0 &&
            (REG(base(g) + 0x14) & 1) && !(REG(SYS + 0x80) & (1u << 14))) {
            const char *text = "BK7258 MISSING I2C1 ROUTE DETECTED\n";

            while (*text) {
                REG(UART + 0x1c) = (uint8_t)*text++;
            }
        }
#endif
        finish(1);
    }
}

static void address(unsigned g, uint32_t byte, int ack)
{
    REG(base(g) + 0x18) = byte;
    issue(g, START);
    if (!!(REG(base(g) + 0x14) & ACK) != ack ||
        !(REG(base(g) + 0x14) & START)) {
        finish(1);
    }
}

static void stop(unsigned g)
{
    uint32_t begin = ticks;

    REG(base(g) + 0x14) = STOP;
    while ((REG(base(g) + 0x14) & 0x8000) && ticks - begin < 10) {
    }
    if (REG(base(g) + 0x14) & (0x8000 | START | STOP | 1)) {
        finish(1);
    }
}

static void start(void)
{
    ticks = seen[0] = seen[1] = 0;
    REG(SYS + 0x30) = (1u << 2) | 1 | (1u << 8);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = (1u << 6) | (MISSING_ROUTE ? 0 : (1u << 14));
    for (unsigned g = 0; g < 2; g++) {
        REG(base(g) + 8) = 3;
        REG(base(g) + 0x10) = 0xcc000000 | (84u << 6);
        REG(base(g) + 0x1c) = 3;
        address(g, 0xa0, 1);
        REG(base(g) + 0x18) = 0x30;
        for (unsigned i = 0; i < 15; i++) {
            REG(base(g) + 0x18) = 0xa0 + i + g;
        }
        issue(g, ACK); /* TX FIFO empties only after actual bus writes. */
        if (!(REG(base(g) + 0x14) & ACK)) {
            finish(1);
        }
        stop(g);
        address(g, 0xa0, 1);
        REG(base(g) + 0x18) = 0x30;
        issue(g, ACK);
        address(g, 0xa1, 1); /* Repeated START without a STOP. */
        issue(g, ACK); /* Mode0 receives 12 bytes. */
        for (unsigned i = 0; i < 15; i++) {
            if (i >= 12) {
                issue(g, ACK | 0xc0); /* Mode3 receives exactly one byte. */
            }
            if (REG(base(g) + 0x18) != 0xa0 + i + g) {
                finish(1);
            }
        }
        stop(g); /* ACK clear sends the final receive NAK. */
        address(g, 0xa2, 0); /* No device at address 0x51. */
        REG(base(g) + 0x14) = ACK | 1;
        if (REG(base(g) + 0x14) & ACK) {
            finish(1); /* A write-one cannot replace the latched response. */
        }
        REG(base(g) + 0x14) = ACK;
        if (REG(base(g) + 0x14) & ACK) {
            finish(1); /* Software cannot turn a missing slave into ACK. */
        }
        stop(g);
    }
    finish(0);
}
