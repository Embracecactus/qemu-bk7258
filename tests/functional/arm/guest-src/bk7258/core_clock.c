/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Bare-metal core-clock exception fixture, not production firmware.
 * Uses diagnostic.c's three-core vectors, startup and UART pattern, adapted
 * from contest tests/host/bk7258/qemu/diagnostic.c at
 * ffe8356bf8b0e4eeaa504b6bcfaaf529bc842b43; see the adjacent LICENSE.
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include <stdint.h>
#include "uart_console.h"

/* Volatile guest MMIO accesses preserve hardware transaction ordering. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define SYS 0x44010000u
#define UART 0x44820000u
#define RTC 0x44000200u
#define TIM0 0x44810000u
#define TIM1 0x45800000u
#define SYST_CSR 0xe000e010u
#define SYST_RVR 0xe000e014u
#define SYST_CVR 0xe000e018u
#define ICSR 0xe000ed04u
#define TIMER_XTAL ((1u << 20) | (1u << 21))
#define CYCLES 9600000u
#define RTC_TOLERANCE 64u /* Two milliseconds at the independent 32-kHz LPO. */

/* Shared SRAM commands/results; each handler executes on the real CPU. */
static volatile uint32_t ready[3];
/* CPU0 publishes a sequence consumed by the secondary cores. */
static volatile uint32_t command;
/* Shared selection checked by the executing interrupt handler. */
static volatile uint32_t active_core;
/* CPU0 selects the phase sequence executed by the active core's timer IRQ. */
static volatile uint32_t scenario;
/* SysTick publishes its first exception to the interrupted main loop. */
static volatile uint32_t done;
/* The active core acknowledges after its interrupt handler has returned. */
static volatile uint32_t completed;
/* Main code supplies the start timestamp to the asynchronous handler. */
static volatile uint32_t begun;
/* SysTick publishes its elapsed RTC measurement to CPU0. */
static volatile uint32_t elapsed;
/* Timer IRQ advances the phase checked by main code. */
static volatile uint32_t phase;
/* Timer IRQ preserves this sample for its subsequent pause checks. */
static volatile uint32_t frozen;

static void cp_start(void);
static void ap1_start(void);
static void ap2_start(void);
static void fault(void);
static void tick0(void);
static void tick1(void);
static void tick2(void);
static void phase0(void);
static void phase1(void);
static void phase2(void);
static void deadline(void);

__attribute__((section(".vectors.cp"), used)) const uintptr_t cp_vectors[80] = {
    [0] = 0x20004000, [1] = (uintptr_t)cp_start,
    [3] = (uintptr_t)fault, [4] = (uintptr_t)fault,
    [5] = (uintptr_t)fault, [6] = (uintptr_t)fault,
    [15] = (uintptr_t)tick0, [19] = (uintptr_t)phase0,
    [29] = (uintptr_t)deadline,
};

static const uintptr_t ap1_vectors[80]
    __attribute__((section(".vectors.ap1"), used)) = {
    [0] = 0x20004000, [1] = (uintptr_t)ap1_start,
    [3] = (uintptr_t)fault, [4] = (uintptr_t)fault,
    [5] = (uintptr_t)fault, [6] = (uintptr_t)fault,
    [15] = (uintptr_t)tick1, [19] = (uintptr_t)phase1,
};

static const uintptr_t ap2_vectors[80]
    __attribute__((section(".vectors.ap2"), used)) = {
    [0] = 0x20004000, [1] = (uintptr_t)ap2_start,
    [3] = (uintptr_t)fault, [4] = (uintptr_t)fault,
    [5] = (uintptr_t)fault, [6] = (uintptr_t)fault,
    [15] = (uintptr_t)tick2, [19] = (uintptr_t)phase2,
};

static void barrier(void)
{
    __asm__ volatile("dmb sy" : : : "memory");
}

static void wake_peers(void)
{
    barrier();
    __asm__ volatile("sev" : : : "memory");
}

static __attribute__((noreturn)) void finish(uint32_t status, const char *text)
{
    uint32_t args[2] = {0x20026, status};

    REG(SYST_CSR) = 0;
    REG(TIM0 + 0x1c) = REG(TIM1 + 0x1c) = 0x380;
    bk7258_test_console_puts(text);
    register uint32_t r0 __asm__("r0") = 0x20;
    register uint32_t *r1 __asm__("r1") = args;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    for (;;) {
    }
}

static void fault(void)
{
    finish(1, "BK7258 CORE CLOCK PROBE FAILED\n");
}

static uint32_t exception(void)
{
    uint32_t value;

    __asm__ volatile("mrs %0, ipsr" : "=r"(value));
    return value;
}

static void deadline(void)
{
    if (exception() != 29 || !(REG(TIM1 + 0x1c) & 0x80)) {
        fault();
    }
    finish(1, "BK7258 CORE CLOCK XTAL DEADLINE FAILED\n");
}

static void tick(unsigned core)
{
    if (exception() != 15 || active_core != core || done) {
        fault();
    }
    elapsed = REG(RTC + 0x0c) - begun;
    REG(SYST_CSR) = 0; /* First exception only, no COUNTFLAG polling. */
    done = 1;
    wake_peers();
}

static void tick0(void) { tick(0); }
static void tick1(void) { tick(1); }
static void tick2(void) { tick(2); }

static void phase_irq(unsigned core)
{
    if (exception() != 19 || active_core != core ||
        !(REG(TIM0 + 0x1c) & 0x80)) {
        fault();
    }
    /* Stop and acknowledge before changing a running timer's end value. */
    REG(TIM0 + 0x1c) = 0x80;
    if (scenario == 6 && phase == 0) {
        REG(SYS + 0x20) = TIMER_XTAL | 0x31; /* Live 120 -> 240 MHz. */
        phase = 1;
    } else if (scenario == 7 && phase == 0) {
        REG(SYS + 0x20) = TIMER_XTAL | 0x73; /* Unknown bus divider: pause. */
        frozen = REG(SYST_CVR);
        if (!frozen || frozen >= CYCLES) {
            fault();
        }
        phase = 1;
        REG(TIM0 + 0x10) = 260000; /* Check frozen CVR at 30 ms. */
        REG(TIM0 + 0x1c) = 1;
    } else if (scenario == 7 && (phase == 1 || phase == 2)) {
        if (REG(SYST_CVR) != frozen || done) {
            fault();
        }
        if (phase == 1) {
            phase = 2;
            REG(TIM0 + 0x10) = 520000; /* Restore at 50 ms. */
            REG(TIM0 + 0x1c) = 1;
        } else {
            REG(SYS + 0x20) = TIMER_XTAL | 0x33;
            phase = 3;
        }
    } else {
        fault();
    }
}

static void phase0(void) { phase_irq(0); }
static void phase1(void) { phase_irq(1); }
static void phase2(void) { phase_irq(2); }

static void run_epoch(void)
{
    REG(SYST_CSR) = 0;
    REG(ICSR) = 1u << 25; /* Clear stale pending SysTick before arming. */
    REG(SYST_RVR) = CYCLES - 1;
    REG(SYST_CVR) = 0;
    begun = REG(RTC + 0x0c);
    REG(SYST_CSR) = 7;
    if (scenario >= 6) {
        REG(TIM0 + 0x10) = 520000;
        REG(TIM0 + 0x1c) = 1;
    }
    while (!done) {
        __asm__ volatile("wfi" : : : "memory");
    }
    barrier();
    completed = command;
    wake_peers();
}

static __attribute__((noreturn)) void secondary(unsigned core)
{
    uint32_t previous = 0;

    REG(0xe000e100) = 1u << 3;
    ready[core] = 1;
    wake_peers();
    for (;;) {
        barrier();
        if (command != previous && active_core == core) {
            previous = command;
            run_epoch();
        }
        __asm__ volatile("wfe" : : : "memory");
    }
}

static void ap1_start(void) { secondary(1); }
static void ap2_start(void) { secondary(2); }

static void clocks(uint32_t mode, uint32_t speeds)
{
    for (unsigned core = 0; core < 3; core++) {
        uintptr_t control = SYS + 0x10 + 4 * core;

        REG(control) = (REG(control) & ~0x10u) |
                       (((speeds >> core) & 1) << 4);
    }
    REG(SYS + 0x20) = TIMER_XTAL | mode;
}

static void sample(uint32_t value)
{
    char text[9];

    for (unsigned i = 0; i < 8; i++) {
        text[i] = "0123456789abcdef"[(value >> (28 - i * 4)) & 15];
    }
    text[8] = 0;
    bk7258_test_console_puts(text);
}

static void cp_start(void)
{
    static const uint8_t modes[] = {0x30, 0x20, 0x31, 0x33,
                                    0x35, 0x37, 0x33, 0x33};
    static const uint16_t expected_ms[][3] = {
        {40, 20, 20}, {60, 30, 30}, {40, 40, 40}, {80, 80, 80},
        {120, 120, 120}, {160, 160, 160}, {50, 50, 50}, {110, 110, 110},
    };

    ready[0] = ready[1] = ready[2] = 0;
    command = active_core = scenario = done = begun = elapsed = phase = 0;
    frozen = completed = 0;
    REG(SYS + 0x30) = (1u << 2) | (1u << 4) | (1u << 13);
    REG(UART + 8) = 1;
    REG(UART + 0x10) = 0xe11b;
    REG(SYS + 0x20) = TIMER_XTAL;
    REG(TIM0 + 8) = REG(TIM1 + 8) = 1;
    REG(SYS + 0x80) = 1u << 13;
    REG(0xe000e100) = (1u << 3) | (1u << 13);
    REG(TIM1 + 0x10) = 13000000; /* XTAL deadline independent of every CPU. */
    REG(TIM1 + 0x1c) = 1;

    REG(RTC) = 0x43;
    REG(RTC + 4) = REG(RTC + 0x18) = 0xffffffff;
    while (REG(RTC + 0x10) != 0xffffffff ||
           REG(RTC + 0x20) != 0xffffffff) {
    }
    REG(RTC) = 0x40;
#if !OMIT_DPLL
    REG(SYS + 0x114) = REG(SYS + 0x114) | (1u << 5);
    while (REG(SYS + 0xe8) & (1u << 5)) {
    }
    if (!(REG(SYS + 0x114) & (1u << 5))) {
        fault();
    }
#endif

    /* CPU release must retain the configured speed bit in each control. */
    clocks(0, 7);
    REG(SYS + 0x14) = (uintptr_t)ap1_vectors |
                      (REG(SYS + 0x14) & 0x10) | 1;
    REG(SYS + 0x18) = (uintptr_t)ap2_vectors |
                      (REG(SYS + 0x18) & 0x10) | 1;
    while (!ready[1] || !ready[2]) {
        __asm__ volatile("wfe" : : : "memory");
    }
    REG(TIM1 + 0x1c) = 0x80;

    for (unsigned test = 0; test < sizeof(modes); test++) {
        clocks(modes[test], test < 2 ? 6 : 7);
        for (unsigned core = 0; core < 3; core++) {
            uint32_t expected = expected_ms[test][core] * 32;

            REG(SYS + 0x80) = (1u << 13) | (core == 0 ? 1u << 3 : 0);
            REG(SYS + 0x88) = core == 1 ? 1u << 3 : 0;
            REG(SYS + 0x90) = core == 2 ? 1u << 3 : 0;
            /* Live-transition epochs must start at 120 MHz on every core. */
            REG(SYS + 0x20) = TIMER_XTAL | modes[test];
            active_core = core;
            scenario = test;
            done = phase = elapsed = 0;
            REG(TIM1 + 0x1c) = 1;
            barrier();
            command++;
            wake_peers();
            if (core == 0) {
                run_epoch();
            } else {
                while (completed != command) {
                    __asm__ volatile("wfe" : : : "memory");
                }
            }
            barrier();
            REG(TIM1 + 0x1c) = 0x80;
            bk7258_test_console_puts("BK7258 CORE CLOCK SAMPLE ");
            sample(test);
            bk7258_test_console_puts(" ");
            sample(core);
            bk7258_test_console_puts(" ");
            sample(elapsed);
            bk7258_test_console_puts("\n");
            if (elapsed + RTC_TOLERANCE < expected ||
                elapsed > expected + RTC_TOLERANCE) {
                finish(1, "BK7258 CORE CLOCK RTC TIMING FAILED\n");
            }
            if ((test == 6 && phase != 1) || (test == 7 && phase != 3)) {
                fault();
            }
        }
    }
    finish(0, "BK7258 THREE CORE CLOCK SYSTICK IRQ OK\n");
}
