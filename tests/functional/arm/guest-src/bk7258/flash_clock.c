/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Independent clock-availability/cancellation guest, executing from CRC NOR.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"
/* Real guest MMIO transactions; these are not host model-state accesses. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define FLASH 0x44030000u
#define AREA 0x200000u
/* Shared between the SysTick interrupt and foreground deadline checks. */
static volatile uint32_t ticks;
static void start(void), fault(void), tick(void);
__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000, [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault, [15] = (uintptr_t)tick,
};
static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};
    __asm__ volatile("cpsid i" : : : "memory");
    REG(0xe000e010) = 0;
    bk7258_test_console_puts(status ? "BK7258 FLASH CLOCK FAILED\n" :
                                    "BK7258 FLASH CLOCK OK\n");
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
    if (++ticks > 100) {
        finish(2);
    }
}
static void check(int value)
{
    if (!value) {
        finish(3);
    }
}
static int busy(void)
{
    return !!(REG(FLASH + 0x10) & (1u << 31));
}
static void wait_ready(void)
{
    uint32_t begin = ticks;
    while (busy()) {
        check(ticks - begin < 5);
    }
}
static void expect_blocked(void)
{
    uint32_t begin = ticks;
    while (ticks - begin < 2) {
        check(busy());
    }
}
static void begin(unsigned op, uint32_t address)
{
    REG(FLASH + 0x54) = (op << 24) | address;
    REG(FLASH + 0x10) = 1u << 29;
}
static void source(int enabled)
{
    REG(SYS + 0x114) = enabled ? (1u << 5) : 0;
    while (REG(SYS + 0xe8) & (1u << 5)) {
    }
}
static void controller_reset(void)
{
    REG(FLASH + 8) = 0;
    check(!busy());
    REG(FLASH + 8) = 1;
    REG(FLASH + 0x28) = 0x0c000005;
}
static void program(uint32_t address, uint32_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        REG(FLASH + 0x14) = value;
    }
    begin(12, address);
}
static void expect_data(uint32_t address, uint32_t value)
{
    begin(5, address);
    wait_ready();
    for (unsigned i = 0; i < 8; i++) {
        check(REG(FLASH + 0x18) == value);
    }
}
static void start(void)
{
    ticks = 0;
    REG(SYS + 0x30) = 1u << 2;
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    controller_reset();
    REG(SYS + 0x24) = 0x05000000;
    begin(20, 0);
    expect_blocked();
    check(REG(FLASH + 0x20) == 0);
    source(1);
    wait_ready();
    check(REG(FLASH + 0x20) == 0xc86517);
    program(AREA, 0x55aa1234);
    wait_ready();
    expect_data(AREA, 0x55aa1234);

    REG(0x54010024) = 0x0d000000;
    expect_data(AREA, 0x55aa1234);
    source(0);
    program(AREA + 32, 0);
    expect_blocked();
    controller_reset();
    source(1);
    expect_data(AREA + 32, 0xffffffff);

    /* Unsupported source and controller modes cannot complete a transaction. */
    REG(SYS + 0x24) = 0x02000000;
    program(AREA + 64, 0);
    expect_blocked();
    controller_reset();
    REG(SYS + 0x24) = 0x05000000;
    expect_data(AREA + 64, 0xffffffff);
    REG(FLASH + 0x28) = 0x0c000004;
    begin(20, 0);
    expect_blocked();
    controller_reset();
    expect_data(AREA, 0x55aa1234);
    finish(0);
}
