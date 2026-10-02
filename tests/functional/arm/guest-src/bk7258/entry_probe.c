/*
 * SPDX-License-Identifier: Apache-2.0
 * Bounded tests of an explicitly injected diagnostic snapshot, not hardware.
 * Adapted from the adjacent Apache-2.0 UART/exception/reset fixture patterns.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define PMU 0x44000000u
#define UART 0x44820000u
#define CONTROL 0x4b1002c8u
#define STATUS 0x4b1002c4u
#define WORD242 0x4b1007c8u
#define R7A 0x440001e8u
#define GATE (1u << 15)
#define POWER (1u << 3)
#define MAGIC 0x45505242u
/* Outside all ELF segments: retained across the machine's ELF reset reload. */
#define CHECKPOINT 0x28020000u

enum {
    ABSENT, OTP_RESET, OTP_BUSY, OTP_GATE_LOSS, OTP_POWER_LOSS, R7A_RESET,
};
#ifndef PROBE_MODE
#define PROBE_MODE OTP_RESET
#endif
#ifndef SNAPSHOT_WORD
#define SNAPSHOT_WORD 0xa5c317e2u
#endif
#ifndef SNAPSHOT_R7A
#define SNAPSHOT_R7A 0x91d6a52cu
#endif

static volatile uint32_t expected_address, expected_pc, expected_id;
static volatile uint32_t armed, faults, checks;
static void start(void);
static void fault(void);
static void fault_frame(uint32_t *frame) __attribute__((used));

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[16] = {
    [0] = 0x20004000,
    [1] = (uintptr_t)start,
    [3] = (uintptr_t)fault,
};

/*
 * Each probe starts with exactly one 16-bit memory instruction. The handler
 * verifies its stacked PC before skipping only that instruction; an unrelated
 * exception, silent read-zero, or ignored write cannot pass the test.
 */
typedef uint32_t (*access_fn)(uint32_t, uint32_t);
#define ACCESS(name, instruction) \
    uint32_t name(uint32_t, uint32_t); \
    __asm__(".pushsection .text\n.thumb\n.thumb_func\n" \
            ".global " #name "\n" #name ":\n" instruction "\nbx lr\n" \
            ".popsection\n")
ACCESS(read32, "ldr r0, [r0]");
ACCESS(read16, "ldrh r0, [r0]");
ACCESS(read8, "ldrb r0, [r0]");
ACCESS(write32, "str r1, [r0]");
ACCESS(write16, "strh r1, [r0]");
ACCESS(write8, "strb r1, [r0]");

static void put_hex(uint32_t value)
{
    char text[9];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = 0;
    bk7258_test_console_puts(text);
}

static void field(uint32_t value)
{
    bk7258_test_console_puts(" ");
    put_hex(value);
}

static __attribute__((noreturn)) void finish(uint32_t status)
{
    uint32_t args[2] = {0x20026, status};

    bk7258_test_console_puts("BK7258 ENTRY RESULT");
    field(status);
    field(PROBE_MODE);
    field(faults);
    field(checks);
    bk7258_test_console_puts(" DONE\n");
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void check(int condition, uint32_t error)
{
    if (!condition) {
        finish(error);
    }
    checks++;
}

static __attribute__((naked)) void fault(void)
{
    __asm__ volatile("mrs r0, msp\nb fault_frame");
}

static void fault_frame(uint32_t *frame)
{
    uint32_t exception, hfsr = REG(0xe000ed2c), cfsr = REG(0xe000ed28);
    uint32_t bfar = REG(0xe000ed38);

    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    bk7258_test_console_puts("BK7258 ENTRY FAULT");
    field(expected_id);
    field(exception);
    field(hfsr);
    field(cfsr);
    field(bfar);
    bk7258_test_console_puts(" END\n");
    check(armed == 1 && exception == 3 && hfsr == 0x40000000 &&
          cfsr == 0x8200 && bfar == expected_address &&
          frame[6] == expected_pc, 0x10);
    frame[6] += 2;
    faults++;
    armed = 0;
}

static void expect_fault(access_fn access, uint32_t address,
                         uint32_t value, uint32_t id)
{
    uint32_t before = faults, exception;

    check(!armed && !REG(0xe000ed28) && !REG(0xe000ed2c), 0x11);
    expected_address = address;
    expected_pc = (uintptr_t)access & ~1u;
    expected_id = id;
    armed = 1;
    access(address, value);
    __asm__ volatile("mrs %0, ipsr" : "=r"(exception));
    check(!armed && faults == before + 1 && !exception, 0x12);
    REG(0xe000ed28) = 0x8200;
    REG(0xe000ed2c) = 0x40000000;
    check(!REG(0xe000ed28) && !REG(0xe000ed2c), 0x13);
}

static void snapshot(void)
{
    check(REG(CONTROL) == 3, 0x20);
    check(REG(STATUS) == (PROBE_MODE == OTP_BUSY), 0x21);
    if (PROBE_MODE != OTP_BUSY) {
        check(REG(WORD242) == SNAPSHOT_WORD, 0x22);
    }
}

static void otp_reject(access_fn access, uint32_t address,
                       uint32_t value, uint32_t id)
{
    expect_fault(access, address, value, id);
    /* Every rejected transaction must preserve all visible state. */
    snapshot();
}

static __attribute__((noreturn)) void reset(void)
{
    REG(CHECKPOINT) = MAGIC;
    REG(CHECKPOINT + 4) = PROBE_MODE;
    REG(CHECKPOINT + 8) = faults;
    REG(CHECKPOINT + 12) = checks;
    __asm__ volatile("dsb" : : : "memory");
    bk7258_test_console_puts("BK7258 ENTRY RESET ARMED\n");
    REG(0xe000ed0c) = 0x05fa0004;
    __asm__ volatile("dsb\nisb" : : : "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void invalid_otp(uint32_t first_id)
{
    expect_fault(read32, CONTROL, 0, first_id);
    expect_fault(read32, STATUS, 0, first_id + 1);
    expect_fault(read32, WORD242, 0, first_id + 2);
    expect_fault(write32, CONTROL, 3, first_id + 3);
}

static void run_otp(int restarted)
{
    if (restarted) {
        invalid_otp(0x40);
        REG(SYS + 0x30) |= GATE;
        REG(SYS + 0x40) &= ~POWER;
        /* Reset never reloads or rearms the supplied epoch. */
        invalid_otp(0x44);
        bk7258_test_console_puts("BK7258 ENTRY OTP RESET INVALID OK\n");
        finish(0);
    }
    if (PROBE_MODE == OTP_GATE_LOSS) {
        /* Readiness arms the epoch even when no OTP access has observed it. */
        REG(SYS + 0x40) &= ~POWER;
        REG(SYS + 0x30) |= GATE;
        REG(SYS + 0x30) &= ~GATE;
        REG(SYS + 0x30) |= GATE;
        check((REG(SYS + 0x30) & GATE) &&
              !(REG(SYS + 0x40) & POWER), 0x23);
        invalid_otp(0x30);
        bk7258_test_console_puts("BK7258 ENTRY OTP UNREAD EPOCH INVALID OK\n");
        finish(0);
    }

    expect_fault(read32, CONTROL, 0, 1);
    REG(SYS + 0x40) |= POWER;
    expect_fault(read32, STATUS, 0, 2);
    REG(SYS + 0x30) |= GATE;
    expect_fault(read32, WORD242, 0, 3);
    expect_fault(write32, CONTROL, 3, 4);
    REG(SYS + 0x40) &= ~POWER;
    snapshot();
    REG(CONTROL) = 3;
    snapshot();
    bk7258_test_console_puts("BK7258 ENTRY OTP VISIBLE");
    field(REG(CONTROL));
    field(REG(STATUS));
    if (PROBE_MODE != OTP_BUSY) {
        field(REG(WORD242));
    }
    bk7258_test_console_puts(" END\n");

    otp_reject(write32, CONTROL, 0, 0x10);
    otp_reject(write32, CONTROL, 2, 0x11);
    otp_reject(write32, CONTROL, 7, 0x12);
    otp_reject(write32, STATUS, 1, 0x13);
    otp_reject(write32, WORD242, ~SNAPSHOT_WORD, 0x14);
    otp_reject(read32, WORD242 - 4, 0, 0x15);
    otp_reject(read32, WORD242 + 4, 0, 0x16);
    otp_reject(write32, CONTROL + 4, 3, 0x17);
    otp_reject(read32, WORD242 + 0x10000000, 0, 0x18);
    otp_reject(read8, CONTROL, 0, 0x19);
    otp_reject(read16, STATUS, 0, 0x1a);
    otp_reject(write8, CONTROL, 3, 0x1b);
    otp_reject(write16, WORD242, 0, 0x1c);
    otp_reject(read32, CONTROL + 1, 0, 0x1d);
    otp_reject(write32, CONTROL + 1, 3, 0x1e);
    bk7258_test_console_puts("BK7258 ENTRY OTP ATOMIC REJECTIONS OK\n");
    if (PROBE_MODE == OTP_BUSY) {
        otp_reject(read32, WORD242, 0, 0x20);
        /* Polling cannot turn the immutable busy input into a ready result. */
        for (unsigned i = 0; i < 16; i++) {
            snapshot();
        }
        otp_reject(read32, WORD242, 0, 0x21);
        bk7258_test_console_puts("BK7258 ENTRY OTP BUSY IMMUTABLE OK\n");
        finish(0);
    }
    if (PROBE_MODE == OTP_POWER_LOSS) {
        REG(SYS + 0x40) |= POWER;
        invalid_otp(0x30);
        REG(SYS + 0x30) |= GATE;
        REG(SYS + 0x40) &= ~POWER;
        check((REG(SYS + 0x30) & GATE) &&
              !(REG(SYS + 0x40) & POWER), 0x23);
        invalid_otp(0x34);
        bk7258_test_console_puts("BK7258 ENTRY OTP RESTORE STAYS INVALID OK\n");
        finish(0);
    }
    reset();
}

static void r7a_reject(access_fn access, uint32_t address,
                       uint32_t value, uint32_t id)
{
    expect_fault(access, address, value, id);
    check(REG(R7A) == SNAPSHOT_R7A && REG(PMU + 0x1ec) == 0x12345678,
          0x30);
}

static void run_r7a(int restarted)
{
    if (restarted) {
        expect_fault(read32, R7A, 0, 0x60);
        REG(PMU) = 0x12345678;
        REG(PMU + 0x94) = 0x424b55aa;
        REG(PMU + 0x94) = 0xbdb4aa55;
        check(REG(PMU + 0x1ec) == 0x12345678, 0x31);
        expect_fault(read32, R7A, 0, 0x61);
        expect_fault(write32, R7A, SNAPSHOT_R7A, 0x62);
        bk7258_test_console_puts("BK7258 ENTRY R7A RESET INVALID OK\n");
        finish(0);
    }
    check(REG(R7A) == SNAPSHOT_R7A, 0x32);
    REG(PMU) = 0x12345678;
    check(REG(R7A) == SNAPSHOT_R7A, 0x33);
    REG(PMU + 0x94) = 0x424b55aa;
    REG(PMU + 0x94) = 0xbdb4aa55;
    check(REG(PMU + 0x1ec) == 0x12345678 &&
          REG(R7A) == SNAPSHOT_R7A, 0x34);
    bk7258_test_console_puts("BK7258 ENTRY R7A COMMIT INDEPENDENT");
    field(REG(R7A));
    field(REG(PMU + 0x1ec));
    bk7258_test_console_puts(" END\n");
    r7a_reject(write32, R7A, SNAPSHOT_R7A, 0x50);
    r7a_reject(write32, R7A, ~SNAPSHOT_R7A, 0x51);
    r7a_reject(read8, R7A, 0, 0x52);
    r7a_reject(read16, R7A, 0, 0x53);
    r7a_reject(write8, R7A, 0, 0x54);
    r7a_reject(write16, R7A, 0, 0x55);
    r7a_reject(read32, R7A - 4, 0, 0x56);
    r7a_reject(read32, R7A + 0x10000000, 0, 0x57);
    r7a_reject(read32, WORD242, 0, 0x58); /* Separate option, no OTP mapping. */
    r7a_reject(read32, R7A + 1, 0, 0x59);
    r7a_reject(write32, R7A + 1, ~SNAPSHOT_R7A, 0x5a);
    reset();
}

static void start(void)
{
    int restarted = REG(CHECKPOINT) == MAGIC;

    armed = faults = checks = 0;
    REG(SYS + 0x30) = 1u << 2;
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    if (restarted) {
        check(REG(CHECKPOINT + 4) == PROBE_MODE, 0x40);
        faults = REG(CHECKPOINT + 8);
        checks = REG(CHECKPOINT + 12);
    }
    if (PROBE_MODE == ABSENT) {
        REG(SYS + 0x30) |= GATE;
        REG(SYS + 0x40) &= ~POWER;
        invalid_otp(0x70);
        expect_fault(read32, WORD242 + 0x10000000, 0, 0x74);
        expect_fault(read32, R7A, 0, 0x75);
        expect_fault(write32, R7A, 0, 0x76);
        expect_fault(read32, R7A + 0x10000000, 0, 0x77);
        bk7258_test_console_puts("BK7258 ENTRY DEFAULT UNMAPPED OK\n");
        finish(0);
    } else if (PROBE_MODE == R7A_RESET) {
        run_r7a(restarted);
    } else {
        run_otp(restarted);
    }
    finish(0x41);
}
