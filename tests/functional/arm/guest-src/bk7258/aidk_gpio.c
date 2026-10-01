/*
 * SPDX-License-Identifier: Apache-2.0
 * AIDK contact-to-GPIO/IRQ55 and GPIO-to-LED bare-metal board probe.
 * Adapted from the adjacent Apache-2.0 timer and UART fixture patterns.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile accesses are guest MMIO or exception-handler communication. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define GPIO(pin) (0x44000400u + 4u * (pin))
#define GPIO_STATUS 0x44000500u
#define GPIO_ROUTE (1u << (55 - 32))
#define INPUT_PULLUP 0x3cu
#define FALLING_IRQ 0x1c3cu

/* SysTick updates time while the main loop renews host-handshake deadlines. */
static volatile uint32_t ticks, deadline;
/* Main and the GPIO exception handler exchange the selected contact/result. */
static volatile uint32_t expected_pin, irq_count, irq_status, irq_exception;
static const unsigned keys[] = {13, 12, 8}; /* KEY1, KEY2, legacy KEY3. */
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

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    REG(0xe000e010) = 0;
    bk7258_test_console_puts(status ? "BK7258 AIDK GPIO PROBE FAILED\n" :
                                    "BK7258 AIDK GPIO PROBE OK\n");
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

static void systick(void)
{
    if (++ticks >= deadline) {
        finish(1);
    }
}

static void gpio_irq(void)
{
    uint32_t exception, status = REG(GPIO_STATUS);

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (MISSING_ROUTE || exception != 71 ||
        status != (1u << expected_pin) ||
        REG(GPIO_STATUS + 4) || (REG(GPIO(expected_pin)) & 1) ||
        !(REG(SYS + 0xa4) & GPIO_ROUTE)) {
        finish(1);
    }
    irq_exception = exception;
    irq_status = status;
    REG(GPIO_STATUS) = status; /* W1C must deassert the routed IRQ. */
    if (REG(GPIO_STATUS) || (REG(SYS + 0xa4) & GPIO_ROUTE)) {
        finish(1);
    }
    irq_count++;
}

static void put_hex(uint32_t value)
{
    char text[9];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = 0;
    bk7258_test_console_puts(text);
}

static void pin_marker(const char *text, unsigned pin)
{
    bk7258_test_console_puts(text);
    put_hex(pin);
    bk7258_test_console_puts("\n");
}

static void wait_byte(unsigned expected)
{
    deadline = ticks + 5000; /* Five seconds for each host interaction. */
    while (!(REG(UART + 0x18) & 0xff00)) {
        __asm__ volatile("wfi");
    }
    if (REG(UART + 0x1c) != expected << 8) {
        finish(1);
    }
}

static void check_inputs(int pressed)
{
    for (unsigned i = 0; i < 3; i++) {
        if ((REG(GPIO(keys[i])) & 1) != (keys[i] != (unsigned)pressed)) {
            finish(1);
        }
    }
}

static void start(void)
{
    ticks = irq_count = irq_status = irq_exception = 0;
    deadline = 5000;
    REG(SYS + 0x30) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(0xe000e014) = 25999; /* One millisecond at the reset CPU clock. */
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7; /* Independent CPU-clock deadline, including WFI. */
    REG(GPIO(40)) = REG(GPIO(41)) = 0x84; /* Both LEDs initially off. */
    for (unsigned i = 0; i < 3; i++) {
        /* All AIDK contacts float when open; there is no board pull-up. */
        REG(GPIO(keys[i])) = 0x2c;
        if (REG(GPIO(keys[i])) & 1) {
            finish(1);
        }
        REG(GPIO(keys[i])) = INPUT_PULLUP;
        if (!(REG(GPIO(keys[i])) & 1)) {
            finish(1);
        }
        REG(GPIO(keys[i])) = FALLING_IRQ;
    }
    REG(GPIO_STATUS) = 0xffffffffu;
    REG(GPIO_STATUS + 4) = 0xffffffffu;
    REG(SYS + 0x84) = MISSING_ROUTE ? 0 : GPIO_ROUTE;
    REG(0xe000e104) = GPIO_ROUTE;
    for (unsigned phase = 0; phase < (MISSING_ROUTE ? 1u : 3u); phase++) {
        unsigned pin = keys[phase];

        expected_pin = pin;
        deadline = ticks + 5000;
        pin_marker("BK7258 AIDK GPIO READY ", pin);
        if (MISSING_ROUTE) {
            uint32_t begin;

            while (!REG(GPIO_STATUS)) {
                __asm__ volatile("wfi");
            }
            begin = ticks;
            while (ticks - begin < 25) {
                __asm__ volatile("wfi");
            }
            if (irq_count || REG(GPIO_STATUS) != (1u << pin) ||
                REG(GPIO_STATUS + 4) || (REG(SYS + 0xa4) & GPIO_ROUTE)) {
                finish(1);
            }
            check_inputs(pin);
            bk7258_test_console_puts("BK7258 AIDK GPIO BLOCKED ");
            put_hex(pin);
            bk7258_test_console_puts(" ");
            put_hex(REG(GPIO_STATUS));
            bk7258_test_console_puts(" ");
            put_hex(irq_count);
            bk7258_test_console_puts(" ");
            put_hex(ticks - begin);
            bk7258_test_console_puts(" SYS ROUTE DEADLINE\n");
            REG(GPIO_STATUS) = 1u << pin;
        } else {
            while (irq_count == phase) {
                __asm__ volatile("wfi");
            }
            if (irq_count != phase + 1 || irq_status != (1u << pin) ||
                irq_exception != 71) {
                finish(1);
            }
            check_inputs(pin);
            /* KEY1: red; KEY2: green; KEY3: both, through guest outputs. */
            REG(GPIO(40)) = phase == 1 ? 0x84 : 0x86;
            REG(GPIO(41)) = phase == 0 ? 0x84 : 0x86;
            bk7258_test_console_puts("BK7258 AIDK GPIO EVENT ");
            put_hex(pin);
            bk7258_test_console_puts(" ");
            put_hex(irq_status);
            bk7258_test_console_puts(" ");
            put_hex(irq_exception);
            bk7258_test_console_puts(" ");
            put_hex(irq_count);
            bk7258_test_console_puts("\n");
        }
        wait_byte('R'); /* Host has observed LEDs and released this contact. */
        check_inputs(-1);
        if (REG(GPIO_STATUS) || REG(GPIO_STATUS + 4) ||
            irq_count != (MISSING_ROUTE ? 0 : phase + 1)) {
            finish(1);
        }
        pin_marker("BK7258 AIDK GPIO RELEASED ", pin);
    }
    REG(GPIO(40)) = REG(GPIO(41)) = 0x84;
    bk7258_test_console_puts("BK7258 AIDK GPIO EXIT READY\n");
    wait_byte('X'); /* No host QMP write can race semihosting process exit. */
    if (MISSING_ROUTE) {
        bk7258_test_console_puts(
            "BK7258 AIDK GPIO EXPECTED SYS ROUTE FAILURE\n");
    }
    finish(MISSING_ROUTE);
}
