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
 * BK7258 source-only Flash buffer algorithm test. Adapted from SDK revision
 * 4ca389311a7ef641f10b94d298dc07ee16b0f79c:
 * cp/middleware/driver/flash/flash_driver.c:354-460 and
 * cp/middleware/soc/bk7258/hal/flash_ll.h:43-65, 207-241.
 * Runs from an explicitly encoded NOR image, not the chip's reset ROM.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Guest hardware transactions, never host device-model state. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define FLASH 0x44030000u
#define SECTOR 0x00200000u
#define AREA (SECTOR + 0x1e0)

/* Tick ISR and HardFault handler observe main's progress. */
static volatile uint32_t ticks, cases, commands, programs, reads, erases;
/* HardFault is expected only at the explicitly armed malformed operation. */
static volatile uint32_t fault_armed;
static uint8_t expected[128], actual[128], payload[65];
static void start(void), fault(void), systick(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
    [15] = (uintptr_t)systick,
};

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
    bk7258_test_console_puts("BK7258 FLASH SDK COUNTS ");
    put_hex(cases);
    put_hex(commands);
    put_hex(programs);
    put_hex(reads);
    put_hex(erases);
    bk7258_test_console_puts("END\nBK7258 FLASH SDK RESULT ");
    put_hex(status);
    put_hex(REG(0xe000ed28));
    put_hex(REG(0xe000ed38));
    bk7258_test_console_puts("DONE\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static int complete(void)
{
    return cases == 54 && commands == 926 && programs == 323 &&
           reads == 544 && erases == 55;
}

static void fault(void)
{
    uint32_t exception;

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    if (BAD_FIFO && fault_armed && complete() && exception == 3 &&
        (REG(0xe000ed2c) & (1u << 30)) &&
        REG(0xe000ed28) == 0x8200 && REG(0xe000ed38) == FLASH + 0x10) {
        bk7258_test_console_puts("BK7258 FLASH SDK EXPECTED FIFO FAULT\n");
        finish(1);
    }
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

static void wait_ready(void)
{
    uint32_t begin = ticks;

    while (REG(FLASH + 0x10) & (1u << 31)) {
        check(ticks - begin < 100, 0x12);
    }
}

static void operation(unsigned op, uint32_t address)
{
    wait_ready();
    REG(FLASH + 0x54) = (op << 24) | address;
    REG(FLASH + 0x10) = REG(FLASH + 0x10) | (1u << 29);
    wait_ready();
    commands++;
    programs += op == 12;
    reads += op == 5;
    erases += op == 13;
}

/* SDK read_common: aligned reads, copying only the requested byte range. */
static void read_bytes(uint8_t *buffer, uint32_t address, unsigned length)
{
    uint32_t aligned = address & ~31u;

    while (length) {
        uint32_t words[8];
        uint8_t *bytes = (uint8_t *)words;

        operation(5, aligned);
        for (unsigned i = 0; i < 8; i++) {
            words[i] = REG(FLASH + 0x18);
        }
        for (unsigned i = address % 32; i < 32 && length; i++) {
            *buffer++ = bytes[i];
            address++;
            length--;
        }
        aligned += 32;
    }
}

/* SDK write_common: FF staging preserves existing neighboring NOR bits. */
static void write_bytes(const uint8_t *buffer, uint32_t address,
                        unsigned length)
{
    uint32_t aligned = address & ~31u;

    while (length) {
        uint32_t words[8];
        uint8_t *bytes = (uint8_t *)words;

        for (unsigned i = 0; i < 8; i++) {
            words[i] = 0xffffffff;
        }
        for (unsigned i = address % 32; i < 32 && length; i++) {
            bytes[i] = *buffer++;
            address++;
            length--;
        }
        wait_ready();
        for (unsigned i = 0; i < 8; i++) {
            REG(FLASH + 0x14) = words[i];
        }
        operation(12, aligned);
        aligned += 32;
    }
}

static void check_area(void)
{
    read_bytes(actual, AREA, sizeof(actual));
    for (unsigned i = 0; i < sizeof(actual); i++) {
        check(actual[i] == expected[i], 0x13);
    }
}

static void start(void)
{
    static const unsigned offsets[] = {0, 1, 3, 4, 15, 31};
    static const unsigned lengths[] = {1, 4, 5, 31, 32, 33, 63, 64, 65};

    ticks = cases = commands = programs = reads = erases = fault_armed = 0;
    REG(0x44010030) = 1u << 2;
    REG(0x44820008) = 1;
    REG(0x44820010) = 0xe11b;
    REG(0xe000e014) = 25999;
    REG(0xe000e018) = 0;
    REG(0xe000e010) = 7;
    REG(FLASH + 8) = 1;
    check(REG(0x44010044) == 0, 0x1a);
    operation(20, 0);
    check(REG(FLASH + 0x20) == 0xc86517, 0x14); /* Explicit test part. */
    read_bytes(actual, AREA + 1, 0);
    write_bytes(payload, AREA + 1, 0);
    check(commands == 1, 0x15);
    for (unsigned a = 0; a < sizeof(offsets) / sizeof(*offsets); a++) {
        for (unsigned n = 0; n < sizeof(lengths) / sizeof(*lengths); n++) {
            unsigned offset = offsets[a], length = lengths[n];
            unsigned bit = (cases & 1) << 7;

            /* Known SYS bit readback; wire/line timing is not asserted. */
            REG(0x44010044) = (REG(0x44010044) & ~0x80u) | bit;
            check(REG(0x54010044) == bit, 0x1b);
            operation(13, SECTOR);
            for (unsigned i = 0; i < sizeof(expected); i++) {
                expected[i] = (i * 29 + cases * 7) ^ 0xd3;
            }
            write_bytes(expected, AREA, sizeof(expected));
            check_area();
            for (unsigned i = 0; i < length; i++) {
                payload[i] = (i * 17 + cases * 11) ^ 0x6b;
                expected[offset + i] &= payload[i];
            }
            write_bytes(payload, AREA + offset, length);
            read_bytes(actual, AREA + offset, length);
            for (unsigned i = 0; i < length; i++) {
                check(actual[i] == expected[offset + i], 0x16);
            }
            check_area();
            cases++;
        }
    }
    /* The chosen part protects its lower 4 MiB with SR1=0x38. */
    REG(FLASH + 0x28) = 0x0c000000 | (0x0238u << 10);
    REG(FLASH + 0x10) = 1u << 30;
    operation(7, 0);
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = 0;
    }
    write_bytes(payload, AREA + 31, sizeof(payload));
    check_area();
    operation(13, SECTOR);
    check_area();
    REG(FLASH + 0x28) = 0x0c000000 | (0x0200u << 10);
    operation(7, 0);
    operation(3, 0);
    check(!(REG(FLASH + 0x24) & 0xff), 0x17);
    check(complete(), 0x18);
    if (BAD_FIFO) {
        for (unsigned i = 0; i < 7; i++) {
            REG(FLASH + 0x14) = 0;
        }
        fault_armed = 1;
        REG(FLASH + 0x54) = (12u << 24) | AREA;
        REG(FLASH + 0x10) = REG(FLASH + 0x10) | (1u << 29);
        finish(0x19); /* An ignored malformed command must not pass. */
    }
    bk7258_test_console_puts("BK7258 FLASH SDK 54 CASES OK\n");
    finish(0);
}
