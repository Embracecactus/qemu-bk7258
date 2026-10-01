/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2020-2021 Beken
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Long TX FIFO-ready path adapted from BK SDK revision
 * 4ca389311a7ef641f10b94d298dc07ee16b0f79c:
 * ap/middleware/driver/spi/spi_driver.c:351-357, 743-780, 1128-1225.
 * The native w25q32 parts are explicit test endpoints, not board BOMs.
 * This fixture does not implement the SDK's DMA duplex or RX-only APIs.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* All register operations are real guest MMIO transactions. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u

/* Shared between the main polling path and actual interrupt handlers. */
static volatile unsigned ticks, group, done, seen[2], attempts, cancelled;
static uint8_t data[196];
static void start(void), fault(void), systick(void), irq0(void), irq1(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[48] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [23] = (uintptr_t)irq0,
    [33] = (uintptr_t)irq1,
};

static uint32_t base(void)
{
    return 0x44870000u + group * 0x1010000u;
}

static void put_hex(uint32_t value)
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

    __asm__ volatile("cpsid i" : : : "memory");
    REG(0xe000e010) = 0;
    bk7258_test_console_puts("BK7258 SPI SDK RESULT ");
    put_hex(status);
    put_hex(attempts);
    put_hex(seen[0]);
    put_hex(seen[1]);
    put_hex(cancelled);
    put_hex(REG(0xe000ed28));
    put_hex(REG(0xe000ed38));
    bk7258_test_console_puts("DONE\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void check(int condition, unsigned code)
{
    if (!condition) {
        finish(code);
    }
}

static void fault(void)
{
    finish(0x10);
}

static void systick(void)
{
    check(++ticks < 1000, 0x11);
}

static void handler(unsigned bus)
{
    uint32_t b = base(), status = REG(b + 0x18), exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(bus == group && exception == 16 + (bus ? 17 : 7), 0x12);
    check((status & 0x7800) == 0x2000 && !done, 0x13);
    /* SDK ISR captures/W1C first, then disables TX FIFO IRQ and TX. */
    REG(b + 0x18) = status;
    REG(b + 0x10) = REG(b + 0x10) & ~0x40u;
    REG(b + 0x14) = REG(b + 0x14) & ~1u;
    seen[bus]++;
    done = 1;
}

static void irq0(void)
{
    handler(0);
}

static void irq1(void)
{
    handler(1);
}

static void send(unsigned length, unsigned supplied)
{
    uint32_t b = base(), begin = ticks;

    done = 0;
    attempts++;
    /* The SDK enables TX before its FIFO-ready polling loop fills DATA. */
    REG(b + 0x18) = 1u << 16;
    REG(b + 0x14) = (length << 8) | 4;
    REG(b + 0x10) = REG(b + 0x10) & ~0x40u;
    REG(b + 0x14) = REG(b + 0x14) | 1;
    for (unsigned i = 0; i < supplied; i++) {
        while (!(REG(b + 0x18) & 2)) {
            check(ticks - begin < 100, 0x14);
        }
        REG(b + 0x1c) = data[i];
    }
    if (supplied < length) {
        /* Reset deasserts CS: bytes already delivered to SSI are not undone. */
        REG(b + 8) = 0;
        begin = ticks;
        while (ticks - begin < 5) {
            __asm__ volatile("wfi");
        }
        check(!done && seen[group] == 3 && !REG(b + 0x18) &&
              !(REG(SYS + 0xa0) & (1u << (group ? 17 : 7))), 0x15);
        cancelled++;
    } else {
        while (!done && ticks - begin < 100) {
            __asm__ volatile("wfi");
        }
        check(done && !(REG(b + 0x18) & 0x7800) &&
              !(REG(b + 0x14) & 1), 0x16);
    }
}

static void program_data(unsigned address)
{
    data[0] = 2;
    data[1] = address >> 16;
    data[2] = address >> 8;
    data[3] = address;
    for (unsigned i = 4; i < sizeof(data); i++) {
        data[i] = (i * 37 + group * 13) ^ 0xa5;
    }
}

static void start(void)
{
    ticks = attempts = cancelled = seen[0] = seen[1] = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 1) | (1u << 9);
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = (1u << 7) | (1u << 17);
    REG(0xe000e100) = (1u << 7) | (1u << 17);
    for (group = 0; group < 2; group++) {
        REG(base() + 8) = 3;
        REG(base() + 0x10) = 0x41c00d30;
        data[0] = 6;
        send(1, 1);
        program_data(0x010020);
        send(132, 132);
        check(seen[group] == 2, 0x17);
        data[0] = 6;
        send(1, 1);
        program_data(0x020020);
        send(196, 132);
    }
    check(attempts == 8 && seen[0] == 3 && seen[1] == 3 && cancelled == 2,
          0x18);
    finish(0);
}
