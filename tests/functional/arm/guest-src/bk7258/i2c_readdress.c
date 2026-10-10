/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AI-assisted downstream experiment. Native EEPROMs are test fixtures only. */
#include <stdint.h>
#include "uart_console.h"

/* Volatile accesses are guest MMIO, not host synchronization. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
/* Shared between real IRQ/SysTick handlers and the main program. */
static volatile unsigned ticks, interrupts, sample, stage;
static void start(void), fault(void), systick(void), irq(void);
__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault, [15] = (uintptr_t)systick,
    [22] = (uintptr_t)irq,
};

static void hex(unsigned value)
{
    char text[10];
    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = ' ';
    text[9] = 0;
    bk7258_test_console_puts(text);
}

static __attribute__((noreturn)) void finish(unsigned code)
{
    uint32_t args[] = {0x20026, code};
    __asm__ volatile("cpsid i" : : : "memory");
    REG(0xe000e010) = 0;
    bk7258_test_console_puts("BK7258 I2C READDRESS RESULT ");
    hex(code); hex(stage); hex(interrupts);
    hex(REG(0xe000ed28)); hex(REG(0xe000ed38));
    bk7258_test_console_puts("DONE\n");
    register unsigned r0 __asm__("r0") = 0x20;
    register void *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}
static void check(int yes)
{
    if (!yes) {
        finish(3);
    }
}
static void fault(void)
{
    finish(2);
}
static void systick(void)
{
    if (++ticks > 100) {
        finish(1);
    }
}
static void irq(void)
{
    unsigned exception;
    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(exception == 22);
    sample = REG(0x45850014);
    check(sample & 1);
    interrupts++;
    REG(0xe000e180) = 1u << 6;
}
static void command(unsigned value)
{
    unsigned previous = interrupts;
    stage++;
    REG(0x45850014) = value;
    REG(0xe000e280) = 1u << 6;
    REG(0xe000e100) = 1u << 6;
    while (interrupts == previous) {
        __asm__ volatile("wfi");
    }
}
static void address(unsigned byte, unsigned ack)
{
    REG(0x45850018) = byte;
    command(0x400);
    check((sample & 0x501) == (0x401 | ack << 8));
}
static void send(unsigned byte)
{
    REG(0x45850018) = byte;
    command(0x100);
    check(sample & 0x100);
}
static unsigned receive(void)
{
    command(0x1c0);
    return REG(0x45850018);
}
static void stop(void)
{
    REG(0x45850014) = 0x200;
    while (REG(0x45850014) & 0x8000) {
        __asm__ volatile("wfi");
    }
}
static void start(void)
{
    ticks = interrupts = sample = stage = 0;
    REG(SYS + 0x30) = 5;
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent SysTick bounds every wait. */
    REG(SYS + 0x80) = 1u << 6;
    REG(0x45850008) = 3;
    REG(0x45850010) = 0xcc000000 | (7u << 6);
    REG(0x4585001c) = 3;
    /* Distinct values through native bus writes, never backing-file reads. */
    address(0xa0, 1); send(0x20); send(0x35); stop();
    address(0xa2, 1); send(0x20); send(0xc7); stop();
    address(0xa0, 1); send(0x20);
    address(0xa1, 1); check(receive() == 0x35); /* Same-address Sr. */
    address(0xa2, 1); send(0x20); /* Final read NAK, then cross-address Sr. */
    address(0xa3, 1); check(receive() == 0xc7);
    address(0xa4, 0); /* No target; A and B must not supply their ACK. */
    stop();
    address(0xa0, 1); send(0x20);
    address(0xa1, 1); check(receive() == 0x35); stop();
    check(interrupts == 19);
    finish(0);
}
