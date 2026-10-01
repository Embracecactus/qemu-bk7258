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
 * BK7258 I2C PIO memory-transfer matrix on native QEMU test EEPROMs.
 * Adapted from the Apache-2.0 BK SDK revision
 * 4ca389311a7ef641f10b94d298dc07ee16b0f79c:
 * ap/middleware/driver/i2c/i2c_driver.c:335-462, 806-859, 914-1073 and
 * ap/middleware/soc/bk7258_ap/hal/i2c_ll.h:229-294, 333-434, 481-493.
 * This is a bounded bare-metal sequence test, not a production SDK build.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile accesses are guest MMIO or ISR/thread observations. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define ACK 0x100u
#define START 0x400u
#define STOP 0x200u
#define MODE 0xc0u
#define SM_INT 1u

/* Main and the two interrupt handlers share transfer state and payload. */
static volatile unsigned ticks, group, phase, reading, wide, done, error;
/* ISR progress is observed by the main transfer loop. */
static volatile unsigned offset, length, irq_count[2], attempted, completed;
/* Completion counters are checked after the interrupt-driven transaction. */
static volatile unsigned successful, naks;
/* The ISR sends the device and memory address selected by main. */
static volatile uint32_t device, address;
/* Bytes cross between the interrupt handler and main's payload checks. */
static volatile uint8_t data[64];
static void start(void), fault(void), systick(void), irq0(void), irq1(void);

enum { MEM_HIGH, MEM_LOW, TX_DATA, RESTART_READ, RX_DATA };

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
    [22] = (uintptr_t)irq0,
    [30] = (uintptr_t)irq1,
};

static uint32_t base(void)
{
    return 0x45850000u + group * 0x10000u;
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
    bk7258_test_console_puts("BK7258 I2C SDK COUNTS ");
    put_hex(attempted);
    put_hex(completed);
    put_hex(successful);
    put_hex(naks);
    put_hex(irq_count[0]);
    put_hex(irq_count[1]);
    bk7258_test_console_puts("END\nBK7258 I2C SDK RESULT ");
    put_hex(status);
    put_hex(group);
    put_hex(wide);
    put_hex(length);
    put_hex(offset);
    put_hex(phase);
    put_hex(REG(0xe000ed28));
    put_hex(REG(0xe000ed38));
    bk7258_test_console_puts("DONE\n");
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
    if (++ticks > 2000) {
        finish(0x11);
    }
}

static void check(int condition, uint32_t code)
{
    if (!condition) {
        finish(code);
    }
}

static void handler(unsigned bus)
{
    unsigned exception;
    uint32_t b = base(), hw = REG(b + 0x14), status = hw & ~START;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(bus == group && exception == 16 + (bus ? 14 : 6) &&
          (hw & SM_INT), 0x12);
    irq_count[bus]++;
    if ((!reading || (hw & START)) && !(hw & ACK)) {
        error = done = 1;
        status |= STOP;
        goto service;
    }
    switch (phase) {
    case MEM_HIGH:
        REG(b + 0x18) = address >> 8;
        phase = MEM_LOW;
        break;
    case MEM_LOW:
        REG(b + 0x18) = address & 0xff;
        phase = reading ? RESTART_READ : TX_DATA;
        break;
    case TX_DATA: {
        unsigned space = 16 - 4 * ((hw >> 6) & 3);
        unsigned remain = length - offset;
        unsigned count = remain < space ? remain : space;

        if (!remain) {
            done = 1;
            status |= STOP;
            break;
        }
        for (unsigned i = 0; i < count; i++) {
            REG(b + 0x18) = data[offset++];
        }
        if (length - offset < space) {
            status &= ~(MODE | START);
        }
        break;
    }
    case RESTART_READ:
        REG(b + 0x18) = (device << 1) | 1;
        REG(b + 0x14) = REG(b + 0x14) | START;
        status |= START;
        phase = RX_DATA;
        break;
    case RX_DATA: {
        static const unsigned levels[] = {12, 8, 4, 1};
        unsigned count = levels[(hw >> 6) & 3];
        unsigned remain;

        if (hw & START) {
            status |= ACK;
            break;
        }
        for (unsigned i = 0; i < count && offset < length; i++) {
            data[offset++] = REG(b + 0x18);
        }
        remain = length - offset;
        if (!remain) {
            status &= ~(ACK | START);
            status |= STOP;
            done = 1;
        } else if (remain < count) {
            status |= ACK | MODE;
        } else {
            status |= ACK;
        }
        break;
    }
    default:
        finish(0x13);
    }
service:
    /* SDK enable_stop() is empty: this combined W0C write commits STOP. */
    REG(b + 0x14) = status & ~SM_INT;
}

static void irq0(void)
{
    handler(0);
}

static void irq1(void)
{
    handler(1);
}

static void transfer(unsigned read, unsigned mem_addr, unsigned size,
                     unsigned expect_nak)
{
    uint32_t b = base(), begin = ticks;
    unsigned mode = read ? (size > 12 ? 0 : size > 8 ? 1 : size > 4 ? 2 : 3) :
                           (size < 4 ? 0 : 1);

    reading = read;
    address = mem_addr;
    length = size;
    offset = done = error = 0;
    phase = wide ? MEM_HIGH : MEM_LOW;
    attempted++;
    __asm__ volatile("cpsid i" : : : "memory");
    REG(b + 0x14) = REG(b + 0x14) & ~STOP;
    REG(b + 0x18) = device << 1;
    REG(b + 0x14) = REG(b + 0x14) | START;
    REG(b + 0x14) = (REG(b + 0x14) & ~MODE) | (mode << 6);
    __asm__ volatile("cpsie i" : : : "memory");
    while (!done && ticks - begin < 100) {
        __asm__ volatile("wfi");
    }
    if (MISSING_ROUTE && group == 1 && !done) {
        check(attempted == 55 && completed == 54 && successful == 52 &&
              naks == 2 && irq_count[0] && !irq_count[1] &&
              (REG(b + 0x14) & 0x8501) == 0x8501 &&
              !(REG(SYS + 0x80) & (1u << 14)) &&
              !(REG(SYS + 0xa0) & (1u << 14)), 0x14);
        bk7258_test_console_puts("BK7258 I2C SDK EXPECTED ROUTE FAILURE\n");
        finish(1);
    }
    while ((REG(b + 0x14) & 0x8000) && ticks - begin < 110) {
    }
    check(done && !(REG(b + 0x14) & (0x8000 | START | STOP | SM_INT)), 0x15);
    completed++;
    if (error) {
        naks++;
    } else {
        successful++;
    }
    if (BAD_ADDRESS && group == 1 && error && !expect_nak) {
        check(attempted == 55 && completed == 55 && successful == 52 &&
              naks == 3 && irq_count[0] && irq_count[1] == 1 &&
              device == 0x52 && !(REG(b + 0x14) & ACK), 0x16);
        bk7258_test_console_puts("BK7258 I2C SDK EXPECTED ADDRESS FAILURE\n");
        finish(2);
    }
    check(error == expect_nak, 0x17);
}

static uint8_t pattern(unsigned index, unsigned size)
{
    return (index * 37u + size + group * 3 + wide * 7) ^ 0xa5u;
}

static void start(void)
{
    static const unsigned sizes[] = {1, 4, 5, 8, 9, 12, 13, 16, 17,
                                     31, 32, 33, 64};

    ticks = irq_count[0] = irq_count[1] = 0;
    attempted = completed = successful = naks = 0;
    REG(SYS + 0x30) = (1u << 2) | 1 | (1u << 8);
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(SYS + 0x80) = (1u << 6) | (MISSING_ROUTE ? 0 : (1u << 14));
    REG(0xe000e100) = (1u << 6) | (1u << 14);
    for (group = 0; group < 2; group++) {
        uint32_t b = base();

        REG(b + 8) = 3;
        REG(b + 0x10) = 0xfc000023u | (84u << 6);
        REG(b + 0x1c) = 3;
        for (wide = 0; wide < 2; wide++) {
            device = BAD_ADDRESS && group == 1 ? 0x52 : 0x50 + wide;
            for (unsigned n = 0; n < sizeof(sizes) / sizeof(*sizes); n++) {
                unsigned size = sizes[n], addr = wide ? 0x180 : 0x30;

                for (unsigned i = 0; i < size; i++) {
                    data[i] = pattern(i, size);
                }
                transfer(0, addr, size, 0);
                for (unsigned i = 0; i < size; i++) {
                    data[i] = 0;
                }
                transfer(1, addr, size, 0);
                for (unsigned i = 0; i < size; i++) {
                    check(data[i] == pattern(i, size), 0x18);
                }
            }
        }
        device = 0x52;
        transfer(0, 0x20, 1, 1);
        transfer(1, 0x20, 1, 1);
    }
    check(attempted == 108 && completed == 108 && successful == 104 &&
          naks == 4 && irq_count[0] && irq_count[1], 0x19);
    bk7258_test_console_puts("BK7258 I2C SDK 108 TRANSACTIONS OK\n");
    finish(0);
}
