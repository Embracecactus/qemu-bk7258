/*
 * BK7258 independent hardware regressions, without guest firmware.
 * SPDX-License-Identifier: Apache-2.0
 * Adapted from the Apache-2.0 contest repository's hardware checks at
 * ffe8356bf8b0e4eeaa504b6bcfaaf529bc842b43:
 * tests/host/bk7258/test_bk7258_qemu.py.
 * AI-assisted downstream experiment; not intended for upstream submission.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/sockets.h"

#define SYS 0x44010000
#define CKMN 0x448a0000
#define MBOX 0x41000000
#define FLASH 0x44030000
#define RTC 0x44000200
#define NOR_SIZE (8 * 1024 * 1024)

static QTestState *start(const void *board)
{
    return qtest_initf("-machine %s -serial null", (const char *)board);
}

static void expect(QTestState *qts, uint32_t address, uint32_t value)
{
    g_assert_cmphex(qtest_readl(qts, address), ==, value);
}

static void aon_wdt(QTestState *qts, unsigned count)
{
    qtest_writel(qts, 0x44000600, 0x5a0000 | count);
    qtest_writel(qts, 0x44000600, 0xa50000 | count);
}

static void expect_clock(QTestState *qts, const char *path, unsigned hz)
{
    QDict *response = qtest_qmp(qts, "{'execute':'qom-get', 'arguments':"
                               "{'path':%s, 'property':'qtest-clock-period'}}",
                               path);
    uint64_t period = hz ? (UINT64_C(1000000000) << 32) / hz : 0;

    g_assert_true(qdict_haskey(response, "return"));
    g_assert_cmpuint(qdict_get_int(response, "return"), ==, period);
    qobject_unref(response);
}

#define CORE_CLOCK_ARGS "-global bk7258-soc.experimental-core-clocks=on"
#define SYSTICK 0xe000e010

static QTestState *start_core_clocks(const void *board)
{
    return qtest_initf("-machine %s -serial null " CORE_CLOCK_ARGS,
                       (const char *)board);
}

static void core_clock_mode(QTestState *qts, unsigned mode, bool ns_alias)
{
    uint32_t addr = SYS + 0x20 + (ns_alias ? 0x10000000 : 0);
    uint32_t value = (qtest_readl(qts, addr) & ~0x7fU) | mode;

    qtest_writel(qts, addr, value);
    expect(qts, SYS + 0x20, value);
}

static void core_clock_speeds(QTestState *qts, unsigned speeds, bool ns_alias)
{
    for (unsigned i = 0; i < 3; i++) {
        uint32_t addr = SYS + 0x10 + 4 * i;
        uint32_t value = (qtest_readl(qts, addr) & ~(1U << 4)) |
                         (((speeds >> i) & 1) << 4);

        qtest_writel(qts, addr + (ns_alias ? 0x10000000 : 0), value);
        expect(qts, addr, value);
    }
}

static void expect_core_clocks(QTestState *qts, unsigned cpu0, unsigned cpu1,
                               unsigned cpu2, unsigned bus)
{
    const unsigned hz[] = { cpu0, cpu1, cpu2 };

    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *source = g_strdup_printf("/machine/soc/coreclk%u", i);
        g_autofree char *input =
            g_strdup_printf("/machine/soc/cpu[%u]/cpuclk", i);
        g_autofree char *secure =
            g_strdup_printf("/machine/soc/cpu[%u]/systick-reg-s/cpuclk", i);
        g_autofree char *nonsecure =
            g_strdup_printf("/machine/soc/cpu[%u]/systick-reg-ns/cpuclk", i);

        expect_clock(qts, source, hz[i]);
        expect_clock(qts, input, hz[i]);
        expect_clock(qts, secure, hz[i]);
        expect_clock(qts, nonsecure, hz[i]);
    }
    expect_clock(qts, "/machine/soc/busclk", bus);
    /* The diagnostic DMA input is independent of nominal busclk. */
    expect_clock(qts, "/machine/soc/cpuclk", 26000000);
    expect_clock(qts, "/machine/soc/dma[0]/hclk", 26000000);
    expect_clock(qts, "/machine/soc/dma[1]/hclk", 26000000);
}

static void core_clock_dpll(QTestState *qts, bool enabled)
{
    qtest_writel(qts, SYS + 0x114, enabled ? 1U << 5 : 0);
    qtest_clock_step(qts, 1000);
}

static void test_core_clock_tuples(const void *board)
{
    static const struct {
        unsigned mode, speeds, mhz[3], bus;
    } tuples[] = {
        { 0x00, 0, { 26, 26, 26 }, 26 },
        { 0x00, 7, { 26, 26, 26 }, 26 },
        { 0x30, 6, { 240, 480, 480 }, 240 },
        { 0x20, 6, { 160, 320, 320 }, 160 },
        { 0x31, 7, { 240, 240, 240 }, 240 },
        { 0x33, 7, { 120, 120, 120 }, 120 },
        { 0x35, 7, { 80, 80, 80 }, 80 },
        { 0x37, 7, { 60, 60, 60 }, 60 },
    };
    QTestState *qts = start_core_clocks(board);

    g_assert_true(qtest_qom_get_bool(qts, "/machine/soc",
                                   "experimental-core-clocks"));
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    core_clock_dpll(qts, true);
    for (unsigned i = 0; i < G_N_ELEMENTS(tuples); i++) {
        core_clock_speeds(qts, tuples[i].speeds, i & 1);
        core_clock_mode(qts, tuples[i].mode, !(i & 1));
        expect_core_clocks(qts, tuples[i].mhz[0] * 1000000,
                           tuples[i].mhz[1] * 1000000,
                           tuples[i].mhz[2] * 1000000,
                           tuples[i].bus * 1000000);
    }
    /* ANA5 is committed after transfer; neither edge changes it early. */
    qtest_writel(qts, 0x54010114, 0);
    expect(qts, SYS + 0xe8, 1U << 5);
    qtest_clock_step(qts, 999);
    /* A busy-register rewrite cannot replace the queued source disable. */
    qtest_writel(qts, SYS + 0x114, 1U << 5);
    expect(qts, SYS + 0xe8, 1U << 5);
    expect(qts, SYS + 0x114, 1U << 5);
    expect_core_clocks(qts, 60000000, 60000000, 60000000, 60000000);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0xe8, 0);
    expect(qts, SYS + 0x114, 0);
    expect_core_clocks(qts, 0, 0, 0, 0);
    qtest_writel(qts, SYS + 0x114, 1U << 5);
    qtest_clock_step(qts, 999);
    expect(qts, SYS + 0x114, 0);
    expect_core_clocks(qts, 0, 0, 0, 0);
    qtest_clock_step(qts, 1);
    expect_core_clocks(qts, 60000000, 60000000, 60000000, 60000000);
    qtest_system_reset(qts);
    expect(qts, SYS + 0x114, 0);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    qtest_writel(qts, SYS + 0x114, 1U << 5);
    qtest_clock_step(qts, 999);
    /* Cancel a pending enable, not just its outputs. */
    qtest_system_reset(qts);
    qtest_clock_step(qts, 1000);
    expect(qts, SYS + 0x114, 0);
    expect(qts, SYS + 0xe8, 0);
    core_clock_speeds(qts, 7, true);
    core_clock_mode(qts, 0x31, true);
    expect_core_clocks(qts, 0, 0, 0, 0);
    core_clock_mode(qts, 0, false); /* XTAL does not require DPLL enable. */
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    qtest_quit(qts);
}

static gsize core_clock_log_size(const char *path)
{
    g_autofree char *log = NULL;
    gsize size;

    g_assert_true(g_file_get_contents(path, &log, &size, NULL));
    g_assert_null(strstr(log, "bk7258-sys: write offset"));
    return size;
}

static void test_core_clock_unknown(const void *board)
{
    static const struct {
        unsigned mode, speeds;
    } unknown[] = {
        { 0x10, 7 }, /* DCO source. */
        { 0x32, 7 }, /* Unestablished core divider. */
        { 0x71, 7 }, /* Unestablished bus divider bit. */
        { 0x31, 6 }, /* Wrong per-core speed combination. */
        { 0x30, 7 }, /* A requested 480M does not mean 480M on every core. */
        { 0x00, 3 }, /* Partial XTAL speed mask. */
    };
    g_autofree char *dir = g_dir_make_tmp("bk7258-core-clock-XXXXXX", NULL);
    g_autofree char *path = NULL;
    g_autofree char *log = NULL;
    gsize before;
    QTestState *qts;

    g_assert_nonnull(dir);
    path = g_build_filename(dir, "unimplemented.log", NULL);
    qts = qtest_initf("-machine %s -serial null " CORE_CLOCK_ARGS
                      " -d unimp,guest_errors -D %s",
                      (const char *)board, path);
    core_clock_speeds(qts, 6, false);
    before = core_clock_log_size(path);
    core_clock_mode(qts, 0x30, false);
    expect_core_clocks(qts, 0, 0, 0, 0);
    g_assert_cmpuint(core_clock_log_size(path), ==, before);
    core_clock_dpll(qts, true);
    expect_core_clocks(qts, 240000000, 480000000, 480000000, 240000000);
    g_assert_cmpuint(core_clock_log_size(path), ==, before);
    core_clock_dpll(qts, false);
    expect_core_clocks(qts, 0, 0, 0, 0);
    g_assert_cmpuint(core_clock_log_size(path), ==, before);
    core_clock_dpll(qts, true);
    for (unsigned i = 0; i < G_N_ELEMENTS(unknown); i++) {
        core_clock_speeds(qts, unknown[i].speeds, i & 1);
        before = core_clock_log_size(path);
        core_clock_mode(qts, unknown[i].mode, !(i & 1));
        expect_core_clocks(qts, 0, 0, 0, 0);
        g_assert_cmpuint(core_clock_log_size(path), >, before);
        before = core_clock_log_size(path);
        core_clock_mode(qts, unknown[i].mode, false);
        core_clock_speeds(qts, unknown[i].speeds, true);
        core_clock_dpll(qts, true);
        g_assert_cmpuint(core_clock_log_size(path), ==, before);
        core_clock_speeds(qts, 7, false);
        core_clock_mode(qts, 0x33, true);
        expect_core_clocks(qts, 120000000, 120000000, 120000000, 120000000);
    }
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "experimental core clock tuple"));
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_core_clock_default_off(const void *board)
{
    QTestState *qts = start(board);

    g_assert_false(qtest_qom_get_bool(qts, "/machine/soc",
                                    "experimental-core-clocks"));
    core_clock_dpll(qts, true);
    core_clock_speeds(qts, 6, true);
    core_clock_mode(qts, 0x30, false);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    core_clock_speeds(qts, 7, false);
    core_clock_mode(qts, 0x33, true);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    core_clock_mode(qts, 0x7f, false);
    core_clock_dpll(qts, false);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    qtest_system_reset(qts);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    qtest_quit(qts);
}

static void core_systick_start(QTestState *qts, uint32_t reload)
{
    /* Native qtest accesses CPU0's private, nonsecure SysTick bank. */
    qtest_writel(qts, SYSTICK, 0);
    qtest_writel(qts, SYSTICK + 4, reload);
    qtest_writel(qts, SYSTICK + 8, 0);
    qtest_writel(qts, SYSTICK, 5);
    expect(qts, SYSTICK, 5);
}

static void test_core_clock_systick_live(const void *board)
{
    QTestState *qts = start_core_clocks(board);

    /* Deferred first reload; SysTick's 32.32 deadline rounds down. */
    core_systick_start(qts, 25);
    qtest_clock_step(qts, 39);
    expect(qts, SYSTICK + 8, 25);
    for (unsigned i = 0; i < 32; i++) {
        core_clock_mode(qts, 0, i & 1);
        core_clock_speeds(qts, 0, !(i & 1));
    }
    qtest_clock_step(qts, 959); /* 38 ns reload + floor(25 * 26M period). */
    expect(qts, SYSTICK, 5);
    qtest_clock_step(qts, 1);
    expect(qts, SYSTICK, 0x10005);

    core_clock_dpll(qts, true);
    core_clock_speeds(qts, 7, false);
    core_clock_mode(qts, 0x33, false);
    for (unsigned pause = 0; pause < 2; pause++) {
        core_clock_mode(qts, 0x33, false);
        core_systick_start(qts, 11999);
        qtest_clock_step(qts, 8); /* Deferred reload at 120 MHz. */
        qtest_clock_step(qts, 20000);
        expect(qts, SYSTICK + 8, 9599);
        if (pause) {
            core_clock_mode(qts, 0x10, true);
            expect_core_clocks(qts, 0, 0, 0, 0);
            qtest_clock_step(qts, 1000000);
            expect(qts, SYSTICK + 8, 9599);
            expect(qts, SYSTICK, 5);
        }
        core_clock_mode(qts, 0x31, true);
        /* Preserve the remaining 9599 cycles at 240 MHz, not the reload. */
        qtest_clock_step(qts, 39994);
        expect(qts, SYSTICK, 5);
        qtest_clock_step(qts, 1);
        expect(qts, SYSTICK, 0x10005);
    }
    qtest_quit(qts);
}

static void test_core_clock_systick_stopped(const void *board)
{
    QTestState *qts = start_core_clocks(board);

    core_clock_dpll(qts, true);
    core_clock_speeds(qts, 7, false);
    core_clock_mode(qts, 0x33, false);
    core_systick_start(qts, 11999);
    qtest_clock_step(qts, 20008);
    qtest_writel(qts, SYSTICK, 4);
    expect(qts, SYSTICK + 8, 9599);
    core_clock_mode(qts, 0x10, false);
    core_clock_mode(qts, 0x31, false);
    qtest_clock_step(qts, 1000000);
    expect(qts, SYSTICK, 4);
    expect(qts, SYSTICK + 8, 9599);
    qtest_writel(qts, SYSTICK, 5);
    qtest_clock_step(qts, 39994);
    expect(qts, SYSTICK, 5);
    qtest_clock_step(qts, 1);
    expect(qts, SYSTICK, 0x10005);

    core_clock_mode(qts, 0x33, false);
    core_systick_start(qts, 11);
    qtest_clock_step(qts, 8);
    qtest_writel(qts, SYSTICK + 4, 0);
    qtest_clock_step(qts, 91);
    expect(qts, SYSTICK, 0x10005); /* Reload zero stops, leaving ENABLE set. */
    expect(qts, SYSTICK + 8, 0);
    core_clock_mode(qts, 0x10, false);
    core_clock_mode(qts, 0x31, false);
    qtest_clock_step(qts, 1000000);
    expect(qts, SYSTICK, 5);
    expect(qts, SYSTICK + 8, 0);

    core_systick_start(qts, 3);
    /* LPO-selected SysTick ignores core changes. */
    qtest_writel(qts, SYSTICK, 1);
    qtest_clock_step(qts, 1000);
    core_clock_mode(qts, 0x10, false);
    core_clock_mode(qts, 0x33, false);
    qtest_clock_step(qts, 123999);
    expect(qts, SYSTICK, 1);
    qtest_clock_step(qts, 1);
    expect(qts, SYSTICK, 0x10001);
    qtest_quit(qts);
}

static void expect_lpo(QTestState *qts, unsigned hz, unsigned gated)
{
    expect_clock(qts, "/machine/soc/lpo", hz);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *path = g_strdup_printf("/machine/soc/cpu[%u]/refclk",
                                                i);

        expect_clock(qts, path, gated & (1U << i) ? 0 : hz);
    }
    /* LPO selection must not change the independent CPU clock. */
    expect_clock(qts, "/machine/soc/cpuclk", 26000000);
}

static void test_lpo_mux(const void *board)
{
    QTestState *qts = start(board);
    const uint32_t r41 = 0x44000104;
    const uint32_t ana5 = SYS + 0x114;
    const uint32_t routes = (1U << 29) | (1U << 30) | (1U << 27);
    static const unsigned gate[] = { 29, 30, 27 };

    expect(qts, r41, 0);
    expect_lpo(qts, 32000, 0);
    qtest_writel(qts, 0x54000104, 0x12); /* ROSC via the NS alias. */
    expect(qts, r41, 0x12);
    expect_lpo(qts, 32000, 0);
    qtest_writel(qts, ana5, 1U << 14);
    qtest_clock_step(qts, 999);
    expect_lpo(qts, 32000, 0);
    qtest_clock_step(qts, 1);
    expect_lpo(qts, 0, 0);
    qtest_writel(qts, r41, 0); /* DIVD remains available with ROSC off. */
    expect_lpo(qts, 32000, 0);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writel(qts, SYS + 0x40, routes & ~(1U << gate[i]));
        expect_lpo(qts, 32000, 1U << i);
    }
    qtest_writel(qts, r41, 2);
    expect_lpo(qts, 0, 4);
    qtest_writel(qts, ana5, 0);
    qtest_clock_step(qts, 1000);
    expect_lpo(qts, 32000, 4); /* Source recovery does not undo core gates. */
    qtest_writel(qts, SYS + 0x40, routes);
    expect_lpo(qts, 32000, 0);
    /* No external source is wired on these models. */
    qtest_writel(qts, r41, 1);
    expect_lpo(qts, 0, 0);
    qtest_writel(qts, r41, 3); /* Undefined source is not a fake oscillator. */
    expect(qts, r41, 3);
    expect_lpo(qts, 0, 0);
    qtest_writel(qts, r41, 2);
    expect_lpo(qts, 32000, 0);
    qtest_writel(qts, ana5, 1U << 14);
    qtest_system_reset(qts); /* Reset cancels the pending clock-loss write. */
    qtest_clock_step(qts, 1000);
    expect(qts, r41, 0);
    expect(qts, ana5, 0);
    expect_lpo(qts, 32000, 0);
    qtest_quit(qts);
}

static void test_memory_uart(const void *board)
{
    QTestState *qts = start(board);
    static const uint32_t aliases[] = {0x08000020, 0x18000020, 0x38000020};

    qtest_writel(qts, 0x28000020, 0x12345678);
    for (unsigned i = 0; i < G_N_ELEMENTS(aliases); i++) {
        expect(qts, aliases[i], 0x12345678);
    }
    expect(qts, SYS + 0x14, 2);
    expect(qts, SYS + 0x18, 10);
    expect(qts, 0x44820018, 0x0a0000);
    qtest_writel(qts, SYS + 0x30, 1U << 2);
    expect(qts, 0x44820018, 0x0a0000);
    qtest_writel(qts, 0x44820008, 1);
    qtest_writel(qts, 0x44820010, 3);
    qtest_writel(qts, 0x44820020, 32);
    qtest_writel(qts, 0x4482001c, 'A');
    expect(qts, 0x44820024, 0);
    qtest_clock_step(qts, 270); /* 5N1 at 26 MHz: seven cycles. */
    expect(qts, 0x44820024, 32);
    expect(qts, SYS + 0xa0, 0);
    qtest_writel(qts, 0x54010080, 0x10);
    expect(qts, SYS + 0xa0, 16);
    qtest_writel(qts, 0x44820024, 0x20);
    expect(qts, SYS + 0xa0, 0);
    qtest_system_reset(qts);
    expect(qts, SYS + 0x80, 0);
    expect(qts, 0x44820024, 0);
    qtest_quit(qts);
}

static void test_uart_clocks(const void *board)
{
    QTestState *qts = start(board);
    static const unsigned gate[] = { 2, 10, 11 };
    static const unsigned shift[] = { 8, 11, 14 };
    static const uint32_t base[] = { 0x44820000, 0x45830000, 0x45840000 };

    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *path =
            g_strdup_printf("/machine/soc/uart[%u]/pclk", i);

        expect_clock(qts, path, 0);
        qtest_writel(qts, base[i] + 8, 1);
        qtest_writel(qts, base[i] + 0x10, 3);
        qtest_writel(qts, base[i] + 0x1c, 'X');
        expect(qts, base[i] + 0x24, 0); /* No fabricated TX completion. */
        qtest_writel(qts, SYS + 0x30, 1U << gate[i]);
        for (unsigned div = 0; div < 4; div++) {
            qtest_writel(qts, 0x54010020, div << shift[i]);
            expect_clock(qts, path, 26000000U >> div);
            qtest_writel(qts, base[i] + 0x1c, 'A');
            expect(qts, base[i] + 0x24, 0);
            qtest_clock_step(qts, DIV_ROUND_UP(7000000000ULL,
                                              26000000U >> div));
            expect(qts, base[i] + 0x24, 32);
            qtest_writel(qts, base[i] + 0x24, 32);
        }
        qtest_writel(qts, SYS + 0x20, 7U << shift[i]);
        expect_clock(qts, path, 0); /* Unmodeled APLL is not a fixed clock. */
        g_assert_cmphex(qtest_readl(qts, base[i] + 0x18) & (1U << 20), ==, 0);
        qtest_writel(qts, base[i] + 0x1c, 'X');
        expect(qts, base[i] + 0x24, 0);
        qtest_writel(qts, SYS + 0x20, 3U << shift[i]);
        expect_clock(qts, path, 3250000);
        qtest_writel(qts, SYS + 0x30, 0);
        expect_clock(qts, path, 0);
    }
    qtest_system_reset(qts);
    expect(qts, SYS + 0x20, 0);
    expect(qts, SYS + 0x30, 0);
    for (unsigned i = 0; i < 3; i++) {
        g_autofree char *path =
            g_strdup_printf("/machine/soc/uart[%u]/pclk", i);

        expect_clock(qts, path, 0);
        expect(qts, base[i] + 0x24, 0);
    }
    qtest_quit(qts);
}

static void uart_wait_rx(QTestState *qts)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (!(qtest_readl(qts, 0x44820018) & (1U << 21))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void uart_rx_setup(QTestState *qts)
{
    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_writel(qts, 0x44820008, 1);
    /* 100 kbit/s at XTAL, eight data bits. */
    qtest_writel(qts, 0x44820010, (259U << 8) | 0x1b);
    qtest_writel(qts, 0x44820014, 128U << 8);
    qtest_writel(qts, 0x44820020, 64);
    qtest_writel(qts, SYS + 0x80, 16);
}

static void test_uart_rx_clock_pause(const void *board)
{
    int fd;
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    uart_rx_setup(qts);
    g_assert_cmpint(send(fd, "A", 1, 0), ==, 1);
    uart_wait_rx(qts);
    qtest_clock_step(qts, 160000); /* Half of the 32-bit-time idle window. */
    qtest_writel(qts, SYS + 0x30, 0);
    qtest_clock_step(qts, 1000000);
    expect(qts, 0x44820024, 0);
    qtest_writel(qts, SYS + 0x20, 1U << 8); /* Resume at XTAL/2. */
    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_clock_step(qts, 319999);
    expect(qts, 0x44820024, 0);
    qtest_clock_step(qts, 1);
    expect(qts, 0x44820024, 64);
    expect(qts, SYS + 0xa0, 16);
    qtest_writel(qts, 0x44820020, 0);
    expect(qts, 0x44820024, 64);
    expect(qts, SYS + 0xa0, 0);
    qtest_writel(qts, 0x44820024, 64);
    expect(qts, 0x4482001c, 'A' << 8);
    qtest_writel(qts, 0x44820020, 64);
    qtest_writel(qts, SYS + 0x20, 1U << 10);
    g_assert_cmpint(send(fd, "B", 1, 0), ==, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, 0x44820018) & (1U << 21), ==, 0);
    qtest_writel(qts, SYS + 0x20, 0);
    uart_wait_rx(qts);
    qtest_clock_step(qts, 320000);
    expect(qts, 0x44820024, 64);
    expect(qts, 0x4482001c, 'B' << 8);
    qtest_writel(qts, 0x44820024, 64);
    g_assert_cmpint(send(fd, "C", 1, 0), ==, 1);
    uart_wait_rx(qts);
    qtest_clock_step(qts, 160000);
    qtest_system_reset(qts);
    qtest_clock_step(qts, 1000000);
    uart_rx_setup(qts);
    qtest_clock_step(qts, 1000000);
    expect(qts, 0x44820024, 0);
    expect(qts, SYS + 0xa0, 0);
    g_assert_cmphex(qtest_readl(qts, 0x44820018) & (1U << 21), ==, 0);
    close(fd);
    qtest_quit(qts);
}

static void uart_wait_count(QTestState *qts, unsigned count)
{
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    while (((qtest_readl(qts, 0x44820018) >> 8) & 0xff) != count) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_uart_rx_capacity(const void *board)
{
    int fd;
    uint8_t data[129];
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i;
    }
    uart_rx_setup(qts);
    qtest_writel(qts, 0x44820020, 0);
    g_assert_cmpint(qemu_send_full(fd, data, 127), ==, 127);
    uart_wait_count(qts, 127);
    expect(qts, 0x44820024, 0); /* Threshold is 128, not almost full. */
    g_assert_cmpint(qemu_send_full(fd, data + 127, 2), ==, 2);
    uart_wait_count(qts, 128);
    g_assert_cmphex(qtest_readl(qts, 0x44820018) & (1U << 18), ==, 1U << 18);
    expect(qts, 0x44820024, 2);
    expect(qts, SYS + 0xa0, 0); /* Status latches while delivery is masked. */
    qtest_writel(qts, 0x44820020, 2);
    expect(qts, SYS + 0xa0, 16);
    qtest_writel(qts, 0x44820024, 2);
    expect(qts, SYS + 0xa0, 16); /* Still at threshold: immediately reassert. */
    expect(qts, 0x4482001c, 0);
    uart_wait_count(qts, 128); /* The 129th byte was held outside the FIFO. */
    qtest_writel(qts, SYS + 0x30, 0);
    for (unsigned i = 1; i < sizeof(data); i++) {
        /* Ring wrap via the NS alias, with no duplicate or lost byte. */
        expect(qts, 0x5482001c, data[i] << 8);
    }
    expect(qts, 0x44820018, (1U << 17) | (1U << 19));
    expect(qts, 0x44820024, 2); /* Draining does not ACK latched status. */
    qtest_writel(qts, 0x44820024, 2);
    expect(qts, SYS + 0xa0, 0);
    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_clock_step(qts, 1000000);
    expect(qts, 0x44820024, 0); /* Empty FIFO cancels the idle deadline. */
    g_assert_cmpint(qemu_send_full(fd, data, 10), ==, 10);
    uart_wait_count(qts, 10);
    qtest_clock_step(qts, 160000);
    qtest_writel(qts, 0x44820008, 0);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, 0x44820008, 1);
    expect(qts, 0x44820024, 0);
    expect(qts, 0x44820018, (1U << 17) | (1U << 19) | (1U << 20));
    close(fd);
    qtest_quit(qts);
}

static void test_uart_rx_width_snapshot(const void *board)
{
    int fd;
    const uint32_t b = 0x44820000;
    const uint8_t data[] = {0xff, 0x80, 0xe5, 0x55};
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    uart_rx_setup(qts);
    qtest_writel(qts, b + 0x14, sizeof(data) << 8);
    for (unsigned received_bits = 5; received_bits <= 8; received_bits++) {
        for (unsigned read_bits = 5; read_bits <= 8; read_bits++) {
            qtest_writel(qts, b + 0x20, 0);
            qtest_writel(qts, b + 0x10,
                         (259U << 8) | ((received_bits - 5) << 3) | 3);
            g_assert_cmpint(qemu_send_full(fd, data, sizeof(data)), ==,
                            sizeof(data));
            uart_wait_count(qts, sizeof(data));
            expect(qts, b + 0x24, 2);
            expect(qts, SYS + 0xa0, 0); /* Latch while delivery is masked. */
            qtest_writel(qts, b + 0x10,
                         (259U << 8) | ((read_bits - 5) << 3) | 3);
            qtest_writel(qts, b + 0x20, 2);
            expect(qts, SYS + 0xa0, 16);
            qtest_writel(qts, b + 0x24, 2);
            expect(qts, SYS + 0xa0, 16); /* Level reasserts at threshold. */
            for (unsigned i = 0; i < sizeof(data); i++) {
                /* FIFO characters retain their width through CONFIG edits. */
                expect(qts, b + 0x1c + ((i & 1) ? 0x10000000 : 0),
                       (data[i] & ((1U << received_bits) - 1)) << 8);
            }
            expect(qts, b + 0x18, 0x1a0000);
            expect(qts, b + 0x24, 2); /* Draining is not an acknowledgement. */
            qtest_writel(qts, b + 0x24, 2);
            expect(qts, SYS + 0xa0, 0);
        }
    }

    qtest_writel(qts, b + 0x10, (259U << 8) | 3);
    g_assert_cmpint(qemu_send_full(fd, data, sizeof(data)), ==, sizeof(data));
    uart_wait_count(qts, sizeof(data));
    qtest_writel(qts, b + 0x10, (259U << 8) | 1); /* RX directional flush. */
    expect(qts, b + 0x18, 0x1a0000);
    expect(qts, b + 0x24, 2); /* Flush preserves the status latch. */
    qtest_writel(qts, b + 0x24, 2);
    expect(qts, SYS + 0xa0, 0);
    /* Bytes held by the backend while disabled use the later enabled width. */
    g_assert_cmpint(qemu_send_full(fd, data, sizeof(data)), ==, sizeof(data));
    qtest_writel(qts, b + 0x10, (259U << 8) | 0x1b);
    uart_wait_count(qts, sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++) {
        expect(qts, b + 0x1c, data[i] << 8);
    }
    close(fd);
    qtest_quit(qts);
}

static void test_uart_rx_width_backpressure(const void *board)
{
    int fd;
    const uint32_t b = 0x44820000;
    uint8_t data[129];
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = 0x80 | i;
    }
    uart_rx_setup(qts);
    qtest_writel(qts, b + 0x10, (259U << 8) | 3); /* Accept five-bit chars. */
    g_assert_cmpint(qemu_send_full(fd, data, sizeof(data)), ==, sizeof(data));
    uart_wait_count(qts, 128);
    qtest_writel(qts, b + 0x10, (259U << 8) | 0x1b);
    expect(qts, b + 0x1c, (data[0] & 0x1f) << 8);
    /* Only now can the backend's 129th byte enter. */
    uart_wait_count(qts, 128);
    qtest_writel(qts, SYS + 0x30, 0);
    for (unsigned i = 1; i < 128; i++) {
        expect(qts, b + 0x1c, (data[i] & 0x1f) << 8);
    }
    expect(qts, b + 0x1c, data[128] << 8); /* Accepted at eight bits. */
    expect(qts, b + 0x18, 0xa0000);
    close(fd);
    qtest_quit(qts);
}

/* Stream output is observable only when a complete modeled frame expires. */
static void uart_expect_no_output(int fd)
{
    uint8_t byte;
    ssize_t count = recv(fd, &byte, 1, MSG_DONTWAIT);

    g_assert_cmpint(count, ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void uart_expect_output(int fd, const uint8_t *expected, size_t length)
{
    int64_t limit = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    size_t done = 0;
    uint8_t actual[256];

    g_assert_cmpuint(length, <=, sizeof(actual));
    while (done < length) {
        ssize_t count = recv(fd, actual + done, length - done, MSG_DONTWAIT);

        if (count < 0) {
            g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
            g_assert_cmpint(g_get_monotonic_time(), <, limit);
            g_usleep(1000);
        } else {
            g_assert_cmpint(count, >, 0);
            done += count;
        }
    }
    g_assert_cmpmem(actual, length, expected, length);
}

static void test_core_clock_peripheral_deadlines(const void *board)
{
    static const uint32_t timers[] = { 0x44810000, 0x45800000 };
    g_autofree char *args = g_strdup_printf("-machine %s " CORE_CLOCK_ARGS,
                                           (const char *)board);
    int fd;
    QTestState *qts = qtest_init_with_serial(args, &fd);

    core_clock_dpll(qts, true);
    core_clock_speeds(qts, 7, false);
    core_clock_mode(qts, 0x33, false);
    uart_rx_setup(qts);
    qtest_writel(qts, SYS + 0x30, (1U << 2) | (1U << 4) | (1U << 13));
    qtest_writel(qts, SYS + 0x20, (1U << 20) | (1U << 21) | 0x33);
    for (unsigned i = 0; i < G_N_ELEMENTS(timers); i++) {
        qtest_writel(qts, timers[i] + 8, 1);
        qtest_writel(qts, timers[i] + 0x10, 2600); /* 100 us at XTAL. */
        qtest_writel(qts, timers[i] + 0x1c, 1);
    }
    qtest_writel(qts, 0x4482001c, 'T'); /* 8N1 frame: also 100 us. */
    g_assert_cmpint(send(fd, "R", 1, 0), ==, 1);
    uart_wait_rx(qts); /* Its independent 32-bit-time idle window is 320 us. */
    qtest_clock_step(qts, 39);
    for (unsigned i = 0; i < 32; i++) {
        qtest_clock_step(qts, 1);
        core_clock_mode(qts, 0x31, true);
        core_clock_mode(qts, 0x10, false);
        core_clock_mode(qts, 0x33, true);
        core_clock_speeds(qts, 6, true);
        core_clock_speeds(qts, 7, false);
    }
    expect_clock(qts, "/machine/soc/uart[0]/pclk", 26000000);
    expect_clock(qts, "/machine/soc/timer[0]/pclk", 26000000);
    expect_clock(qts, "/machine/soc/timer[1]/pclk", 26000000);
    expect_core_clocks(qts, 120000000, 120000000, 120000000, 120000000);
    qtest_clock_step(qts, 99999 - 39 - 32);
    expect(qts, 0x44820024, 0);
    uart_expect_no_output(fd);
    for (unsigned i = 0; i < G_N_ELEMENTS(timers); i++) {
        expect(qts, timers[i] + 0x1c, 1);
    }
    qtest_clock_step(qts, 1);
    expect(qts, 0x44820024, 32);
    uart_expect_output(fd, (const uint8_t[]){ 'T' }, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(timers); i++) {
        expect(qts, timers[i] + 0x1c, 0x81);
    }
    qtest_writel(qts, 0x44820024, 32);
    qtest_clock_step(qts, 219999);
    expect(qts, 0x44820024, 0);
    qtest_clock_step(qts, 1);
    expect(qts, 0x44820024, 64);
    expect(qts, 0x4482001c, 'R' << 8);
    close(fd);
    qtest_quit(qts);
}

static void test_uart_tx_fifo_frames(const void *board)
{
    int fd;
    uint8_t data[179];
    const uint32_t b = 0x44820000;
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);
    static const unsigned divider[] = {0, 1, 225, 65535};

    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_writel(qts, b + 8, 1);
    qtest_writel(qts, b + 0x10, (259U << 8) | 0x1b); /* 100 us, 8N1. */
    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i;
    }
    qtest_writel(qts, b + 0x1c, data[0]);
    expect(qts, b + 0x18, 0x1a0000); /* Empty queue, occupied shifter. */
    expect(qts, b + 0x24, 0);
    for (unsigned i = 1; i < 129; i++) {
        qtest_writel(qts, b + 0x1c, data[i]);
    }
    expect(qts, b + 0x18, 0x90080); /* 128 queued, full, not write-ready. */
    qtest_writel(qts, b + 0x1c, 255); /* Strict overflow policy, no mutation. */
    expect(qts, b + 0x18, 0x90080);
    uart_expect_no_output(fd);
    qtest_clock_step(qts, 5000000);
    uart_expect_output(fd, data, 50);
    g_assert_cmpuint(qtest_readl(qts, b + 0x18) & 0xff, ==, 78);
    for (unsigned i = 129; i < sizeof(data); i++) {
        qtest_writel(qts, b + 0x1c, data[i]);
    }
    expect(qts, b + 0x18, 0x90080); /* Ring wrap/refill, still ordered. */
    qtest_clock_step(qts, 12900000);
    uart_expect_output(fd, data + 50, sizeof(data) - 50);
    expect(qts, b + 0x18, 0x1a0000);
    expect(qts, b + 0x24, 32);
    uart_expect_no_output(fd);

    for (unsigned bits = 5; bits <= 8; bits++) {
        for (unsigned parity = 0; parity < 2; parity++) {
            for (unsigned stop = 1; stop <= 2; stop++) {
                for (unsigned d = 0; d < G_N_ELEMENTS(divider); d++) {
                    uint32_t config = 1 | ((bits - 5) << 3) |
                        (parity << 5) | ((stop - 1) << 7) | (divider[d] << 8);
                    uint64_t cycles = (1 + bits + parity + stop) *
                                      (divider[d] + 1ULL);
                    uint64_t delay = DIV_ROUND_UP(cycles * 1000000000,
                                                  26000000);
                    uint8_t expected = (1U << bits) - 1;

                    qtest_writel(qts, b + 0x10, config);
                    qtest_writel(qts, b + 0x24, 32);
                    qtest_writel(qts, b + 0x1c, 255);
                    qtest_clock_step(qts, delay - 1);
                    expect(qts, b + 0x24, 0);
                    uart_expect_no_output(fd);
                    qtest_clock_step(qts, 1);
                    uart_expect_output(fd, &expected, 1);
                    expect(qts, b + 0x24, 32);
                }
            }
        }
    }
    qtest_writel(qts, b + 0x10, (259U << 8) | 0x19); /* 8N1, 100 us. */
    qtest_writel(qts, b + 0x24, 32);
    qtest_writel(qts, b + 0x1c, 255);
    qtest_writel(qts, b + 0x1c, 255);
    qtest_writel(qts, b + 0x10, (25U << 8) | 0x81); /* Next: 5N2, 8 us. */
    qtest_writel(qts, b + 0x14, 0);
    qtest_writel(qts, b + 0x20, 32);
    qtest_clock_step(qts, 99999);
    uart_expect_no_output(fd);
    qtest_clock_step(qts, 1);
    uart_expect_output(fd, (const uint8_t[]){255}, 1);
    expect(qts, b + 0x24, 0); /* Last byte still shifting, FIFO empty. */
    qtest_clock_step(qts, 7999);
    uart_expect_no_output(fd);
    qtest_clock_step(qts, 1);
    uart_expect_output(fd, (const uint8_t[]){31}, 1);
    expect(qts, b + 0x24, 32);
    close(fd);
    qtest_quit(qts);
}

static void test_uart_tx_service_routes(const void *board)
{
    QTestState *qts = start(board);
    static const uint32_t bases[] = {0x44820000, 0x45830000, 0x45840000};
    static const unsigned gates[] = {2, 10, 11};
    static const unsigned irqs[] = {4, 15, 16};

    for (unsigned unit = 0; unit < 3; unit++) {
        uint32_t b = bases[unit], irq = 1U << irqs[unit];

        qtest_writel(qts, SYS + 0x30, 1U << gates[unit]);
        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, b + 0x10, (259U << 8) | 0x19);
        qtest_writel(qts, b + 0x1c, 'A');
        qtest_writel(qts, b + 0x1c, 'B');
        qtest_writel(qts, b + 0x14, 1);
        expect(qts, b + 0x24, 0); /* count == threshold is not below it. */
        qtest_clock_step(qts, 100000);
        expect(qts, b + 0x24, 1); /* count zero, but B still shifting. */
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + core * 8, irq);
            expect(qts, SYS + 0xa0 + core * 8, 0);
        }
        qtest_writel(qts, b + 0x20, 1);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + core * 8, irq);
        }
        qtest_writel(qts, b + 0x24, 1);
        expect(qts, b + 0x24, 1); /* Level still eligible: reassert. */
        qtest_writel(qts, b + 0x1c, 'C');
        qtest_writel(qts, b + 0x24, 1);
        expect(qts, b + 0x24, 0); /* Refill removes eligibility. */
        qtest_clock_step(qts, 200000);
        expect(qts, b + 0x24, 33);
        qtest_writel(qts, b + 0x20, 32);
        qtest_writel(qts, b + 0x14, 0);
        qtest_writel(qts, b + 0x24, 1);
        expect(qts, b + 0x24, 32); /* Threshold zero does not assert. */
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + core * 8, irq);
        }
        qtest_writel(qts, b + 0x1c, 'D');
        expect(qts, b + 0x24, 32); /* New burst does not ACK old finish. */
        qtest_writel(qts, b + 0x24, 32);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + core * 8, 0);
            qtest_writel(qts, SYS + 0x80 + core * 8, 0);
        }
        qtest_clock_step(qts, 100000);
        expect(qts, b + 0x24, 32);
        qtest_writel(qts, b + 8, 0);
    }
    qtest_quit(qts);
}

static void test_uart_tx_clock_pause(const void *board)
{
    int fd;
    const uint32_t b = 0x44820000;
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_writel(qts, b + 8, 1);
    qtest_writel(qts, b + 0x10, (225U << 8) | 0x19); /* 2260 cycles. */
    qtest_writel(qts, b + 0x1c, 'A');
    qtest_writel(qts, b + 0x1c, 'B');
    for (unsigned i = 0; i < 64; i++) {
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_writel(qts, SYS + 0x30, 1U << 2);
    }
    qtest_clock_step(qts, 77); /* Exactly two complete 26-MHz cycles. */
    qtest_writel(qts, SYS + 0x30, 0);
    qtest_writel(qts, b + 0x1c, 'X'); /* Stopped-clock writes are ignored. */
    qtest_clock_step(qts, 1000000);
    expect(qts, b + 0x24, 0);
    g_assert_cmpuint(qtest_readl(qts, b + 0x18) & 0xff, ==, 1);
    uart_expect_no_output(fd);
    qtest_writel(qts, SYS + 0x20, 1U << 8); /* Remaining 2258 at 13 MHz. */
    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_clock_step(qts, 173692);
    uart_expect_no_output(fd);
    qtest_clock_step(qts, 1);
    uart_expect_output(fd, (const uint8_t *)"A", 1);
    expect(qts, b + 0x24, 0);
    qtest_writel(qts, SYS + 0x20, 1U << 10); /* Absent APLL stops B. */
    qtest_clock_step(qts, 1000000);
    uart_expect_no_output(fd);
    expect(qts, b + 0x24, 0);
    qtest_writel(qts, SYS + 0x20, 0);
    qtest_clock_step(qts, 86923);
    uart_expect_no_output(fd);
    qtest_clock_step(qts, 1);
    uart_expect_output(fd, (const uint8_t *)"B", 1);
    expect(qts, b + 0x24, 32);
    close(fd);
    qtest_quit(qts);
}

static void test_uart_tx_flush_reset(const void *board)
{
    int fd;
    const uint32_t b = 0x44820000, config = (259U << 8) | 0x1b;
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    qtest_writel(qts, SYS + 0x30, 1U << 2);
    qtest_writel(qts, b + 8, 1);
    qtest_writel(qts, b + 0x10, config);
    qtest_writel(qts, b + 0x14, 1U << 8);
    g_assert_cmpint(send(fd, "r", 1, 0), ==, 1);
    uart_wait_count(qts, 1);
    qtest_writel(qts, b + 0x1c, 'A');
    qtest_writel(qts, b + 0x1c, 'B');
    qtest_writel(qts, b + 0x10, config & ~1U); /* Only TX flushes. */
    g_assert_cmpuint(qtest_readl(qts, b + 0x18) & 0xffff, ==, 0x100);
    expect(qts, b + 0x24, 2);
    qtest_writel(qts, b + 0x10, config & ~3U); /* RX flush, keep IRQ. */
    expect(qts, b + 0x24, 2);
    qtest_clock_step(qts, 1000000);
    uart_expect_no_output(fd);
    expect(qts, b + 0x24, 2);
    qtest_writel(qts, b + 0x10, config);
    qtest_writel(qts, b + 0x1c, 'C');
    g_assert_cmpint(send(fd, "s", 1, 0), ==, 1);
    uart_wait_count(qts, 1);
    qtest_writel(qts, b + 0x10, config & ~2U); /* TX must continue. */
    qtest_clock_step(qts, 100000);
    uart_expect_output(fd, (const uint8_t *)"C", 1);
    expect(qts, b + 0x24, 34);
    qtest_writel(qts, b + 0x24, 34);
    for (unsigned kind = 0; kind < 3; kind++) {
        qtest_writel(qts, b + 0x10, config);
        qtest_writel(qts, b + 0x1c, 'D');
        qtest_writel(qts, b + 0x1c, 'E');
        qtest_clock_step(qts, 50000);
        if (kind == 0) {
            qtest_writel(qts, SYS + 0x30, 0);
            qtest_writel(qts, b + 0x10, config & ~3U);
            qtest_writel(qts, b + 0x10, config);
        } else if (kind == 1) {
            qtest_writel(qts, b + 8, 0);
        } else {
            qtest_system_reset(qts);
        }
        qtest_writel(qts, SYS + 0x30, 1U << 2);
        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, b + 0x10, config);
        qtest_clock_step(qts, 1000000);
        uart_expect_no_output(fd);
        expect(qts, b + 0x24, 0);
        g_assert_cmpuint(qtest_readl(qts, b + 0x18) & 0xffff, ==, 0);
    }
    /* TX-only control changes must not restart a pending RX idle window. */
    qtest_writel(qts, b + 0x14, 128U << 8);
    g_assert_cmpint(send(fd, "t", 1, 0), ==, 1);
    uart_wait_count(qts, 1);
    qtest_clock_step(qts, 160000);
    qtest_writel(qts, b + 0x10, config & ~1U);
    qtest_clock_step(qts, 159999);
    expect(qts, b + 0x24, 0);
    qtest_clock_step(qts, 1);
    expect(qts, b + 0x24, 64);
    expect(qts, b + 0x1c, 't' << 8);
    close(fd);
    qtest_quit(qts);
}

static void test_watchdog_sources(const void *board)
{
    QTestState *qts = start(board);
    const char *awdt = "/machine/soc/wdt[0]/clk";
    const char *dwdt = "/machine/soc/wdt[1]/clk";

    expect_clock(qts, awdt, 1000);
    expect_clock(qts, dwdt, 0);
    qtest_writel(qts, SYS + 0x30, 1U << 31);
    for (unsigned div = 0; div < 4; div++) {
        qtest_writel(qts, SYS + 0x28, div << 2);
        expect_clock(qts, dwdt, 32000 / (2U << div));
    }
    qtest_writel(qts, 0x44000104, 1); /* Missing LPO stops DWDT, not AWDT. */
    expect_clock(qts, dwdt, 0);
    expect_clock(qts, awdt, 1000);
    qtest_writel(qts, SYS + 0x80, 16);
    aon_wdt(qts, 3);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, SYS + 0x114, 1U << 14);
    qtest_clock_step(qts, 1000);
    expect_clock(qts, awdt, 0);
    qtest_clock_step(qts, 20000000);
    expect(qts, SYS + 0x80, 16); /* Stopped ROSC must prevent reset. */
    qtest_writel(qts, 0x44000104, 0);
    expect_clock(qts, dwdt, 2000); /* DIVD works while ROSC is stopped. */
    expect_clock(qts, awdt, 0);
    qtest_writel(qts, SYS + 0x30, 0);
    expect_clock(qts, dwdt, 0);
    qtest_writel(qts, SYS + 0x114, 0);
    qtest_clock_step(qts, 1000);
    expect_clock(qts, awdt, 1000);
    expect_clock(qts, dwdt, 0); /* Recovery does not undo the APB gate. */
    qtest_clock_step(qts, 1999999);
    expect(qts, SYS + 0x80, 16);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0x80, 0);
    expect_clock(qts, awdt, 1000);
    expect_clock(qts, dwdt, 0);
    qtest_quit(qts);
}

static void test_watchdog(const void *board)
{
    QTestState *qts = start(board);

    qtest_writel(qts, SYS + 0x80, 16);
    qtest_writel(qts, 0x44000600, 0xa5000a); /* Missing first key. */
    qtest_clock_step(qts, 20000000);
    expect(qts, SYS + 0x80, 16);
    qtest_writel(qts, 0x44000600, 0x5a000a);
    qtest_writel(qts, 0x44000600, 0xa5000b); /* Mismatched count. */
    expect(qts, 0x44000600, 0);
    aon_wdt(qts, 10);
    qtest_clock_step(qts, 9000000);
    aon_wdt(qts, 10);
    qtest_clock_step(qts, 9000000);
    expect(qts, SYS + 0x80, 16);
    aon_wdt(qts, 0);
    qtest_clock_step(qts, 20000000);
    expect(qts, SYS + 0x80, 16);
    aon_wdt(qts, 2);
    qtest_clock_step(qts, 1999999);
    expect(qts, SYS + 0x80, 16);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0x80, 0);
    expect(qts, 0x44000600, 0);
    qtest_quit(qts);
}

static const uint32_t spi_base[] = { 0x44870000, 0x45880000 };
static const unsigned spi_gate[] = { 1, 9 };
static const unsigned spi_irq[] = { 7, 17 };

static QTestState *start_spi(const void *board)
{
    return qtest_initf("-machine %s -serial null "
                      "-device w25q32,bus=spi0,cs=0 "
                      "-device w25q32,bus=spi1,cs=0", (const char *)board);
}

static void spi_setup(QTestState *qts, unsigned g)
{
    qtest_writel(qts, SYS + 0x30, (1U << spi_gate[0]) | (1U << spi_gate[1]));
    qtest_writel(qts, SYS + 0x28, 0);
    qtest_writel(qts, spi_base[g] + 8, 3);
    /* XTAL/(2*13), 8-bit master, interval1: model word period is 9000 ns. */
    qtest_writel(qts, spi_base[g] + 0x10, 0x41c00d00);
}

static void spi_wait(QTestState *qts, unsigned g)
{
    for (unsigned step = 0; step < 70; step++) {
        if (qtest_readl(qts, spi_base[g] + 0x18) & 0x2000) {
            return;
        }
        qtest_clock_step(qts, 9000);
    }
    g_assert_not_reached();
}

static void spi_xfer(QTestState *qts, unsigned g, const uint8_t *data,
                     unsigned count, uint8_t *received)
{
    uint32_t base = spi_base[g];

    qtest_writel(qts, base + 0x14, 0);
    qtest_writel(qts, base + 0x18, 0x37f00);
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(qts, base + 0x1c, data[i]);
    }
    qtest_writel(qts, base + 0x14, (count << 8) | (count << 20) | 3);
    spi_wait(qts, g);
    g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x7800, ==, 0x6000);
    for (unsigned i = 0; i < count; i++) {
        received[i] = qtest_readl(qts, base + 0x1c);
    }
    g_assert_cmphex(qtest_readl(qts, base + 0x18) & 4, ==, 0);
}

static void test_spi_lsb_byte_adapter(const void *board)
{
    QTestState *qts = start_spi(board);
    const uint8_t msb_id[] = {0x9f, 0, 0, 0};
    const uint8_t lsb_id[] = {0xf9, 0, 0, 0};
    const uint8_t status[] = {0xa0, 0}; /* Reversed RDSR 0x05. */
    const uint8_t disable[] = {0x20}; /* Reversed WRDI 0x04. */
    const uint32_t control = 0x41c80d00;
    uint8_t rx[4];

    for (unsigned g = 0; g < 2; g++) {
        uint32_t b = spi_base[g], irq = 1U << spi_irq[g];

        spi_setup(qts, g);
        qtest_writel(qts, b + 0x10, control);
        spi_xfer(qts, g, lsb_id, sizeof(lsb_id), rx);
        g_assert_cmphex(rx[1], ==, 0xf7); /* Native EF/40/16, reversed. */
        g_assert_cmphex(rx[2], ==, 0x02);
        g_assert_cmphex(rx[3], ==, 0x68);
        qtest_writel(qts, b + 0x10, control & ~(1U << 19));
        spi_xfer(qts, g, msb_id, sizeof(msb_id), rx);
        g_assert_cmphex(rx[1], ==, 0xef);
        g_assert_cmphex(rx[2], ==, 0x40);
        g_assert_cmphex(rx[3], ==, 0x16);
        qtest_writel(qts, b + 0x10, control);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 0x40, ==, 0);

        /* A real peripheral WEL mutation waits for the pending word. */
        qtest_writel(qts, b + 0x14, 0);
        qtest_writel(qts, b + 0x18, 0x37f00);
        qtest_writel(qts, b + 0x1c, 0x60); /* Reversed WREN 0x06. */
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core, irq);
        }
        qtest_writel(qts, b + 0x14, 0x105);
        qtest_clock_step(qts, 4500);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x6000, ==, 0);
        /* Active-format changes are rejected atomically, including bit19. */
        qtest_writel(qts, b + 0x10, control & ~(1U << 19));
        expect(qts, b + 0x10, control);
        qtest_writel(qts, SYS + 0x30,
                     (1U << spi_gate[0]) | (1U << spi_gate[1]));
        qtest_clock_step(qts, 4499);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core, 0);
        }
        qtest_clock_step(qts, 1);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core, irq);
        }
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 0x40, ==, 0x40);
        spi_xfer(qts, g, disable, sizeof(disable), rx);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 0x40, ==, 0);

        qtest_writel(qts, b + 0x14, 0);
        qtest_writel(qts, b + 0x18, 0x37f00);
        qtest_writel(qts, b + 0x1c, 0x60);
        qtest_writel(qts, b + 0x14, 0x101);
        qtest_clock_step(qts, 4500);
        qtest_writel(qts, b + 8, 0); /* Cancel before SSI mutation. */
        qtest_clock_step(qts, 1000000);
        spi_setup(qts, g);
        qtest_writel(qts, b + 0x10, control);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 0x40, ==, 0);
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core, 0);
        }
    }
    qtest_quit(qts);
}

static void test_spi_native_frames_routes(const void *board)
{
    QTestState *qts = start_spi(board);
    const uint8_t id[] = { 0x9f, 0, 0, 0 };
    const uint8_t enable[] = { 0x06 };
    const uint8_t status[] = { 0x05, 0 };
    const uint8_t disable[] = { 0x04 };
    uint8_t rx[4];

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = spi_base[g], irq = 1U << spi_irq[g];

        spi_setup(qts, g);
        spi_xfer(qts, g, id, sizeof(id), rx);
        g_assert_cmphex(rx[1], ==, 0xef);
        g_assert_cmphex(rx[2], ==, 0x40);
        g_assert_cmphex(rx[3], ==, 0x16);
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core, irq);
            expect(qts, SYS + 0xa0 + 8 * core, 0);
        }
        qtest_writel(qts, base + 0x14, 0x0040040f); /* Unmask latched done. */
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core, irq);
        }
        qtest_writel(qts, base + 0x18, 0x2000);
        expect(qts, SYS + 0xa0, irq);
        qtest_writel(qts, base + 0x10000018, 0x4000);
        expect(qts, SYS + 0xa0, 0);
        spi_xfer(qts, g, enable, sizeof(enable), rx);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 2, ==, 2); /* Native slave retained WEL. */
        spi_xfer(qts, g, disable, sizeof(disable), rx);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 2, ==, 0);
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core, 0);
        }
    }
    qtest_quit(qts);
}

static void test_spi_fifo_errors(const void *board)
{
    QTestState *qts = start_spi(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = spi_base[g];

        spi_setup(qts, g);
        /* Native flash READ command, followed by address zero. */
        qtest_writel(qts, base + 0x1c, 3);
        for (unsigned i = 1; i < 64; i++) {
            qtest_writel(qts, base + 0x1c, 0);
        }
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 2, ==, 0);
        qtest_writel(qts, base + 0x1c, 0xee); /* FIFO overflow is rejected. */
        qtest_writel(qts, base + 0x14, (65U << 8) | (65U << 20) | 3);
        for (unsigned i = 0; i < 64; i++) {
            qtest_clock_step(qts, 9000);
        }
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x7800, ==, 0x800);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x6000, ==, 0);
        qtest_writel(qts, base + 0x1c, 0); /* Recover the starved last word. */
        qtest_clock_step(qts, 9000);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x7800, ==, 0x7800);
        for (unsigned i = 0; i < 64; i++) {
            /* Drop-new preserves the first four command/address responses. */
            expect(qts, base + 0x1c, i < 4 ? 0 : 0xff);
        }
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 4, ==, 0);
        qtest_writel(qts, base + 0x18, 0x800);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x7800, ==, 0x7000);
        qtest_writel(qts, base + 0x18, 0x1700);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x7800, ==, 0x6000);
        qtest_writel(qts, base + 8, 0);
    }
    qtest_quit(qts);
}

static void test_spi_completed_config_rejection(const void *board)
{
    QTestState *qts = start_spi(board);
    static const uint32_t invalid[] = {
        1, 0x100002, 0x200103, 0x103,
    };
    const uint8_t status[] = {0x05, 0};
    uint8_t rx[2];

    for (unsigned g = 0; g < 2; g++) {
        uint32_t b = spi_base[g], before;

        spi_setup(qts, g);
        qtest_writel(qts, b + 0x1c, 4); /* WRDI completes with CFG enabled. */
        qtest_writel(qts, b + 0x14, 0x101);
        spi_wait(qts, g);
        before = qtest_readl(qts, b + 0x18);
        qtest_writel(qts, b + 0x1c, 6); /* Queue WREN without a new frame. */
        for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
            qtest_writel(qts, b + 0x10000014, invalid[i]);
            expect(qts, b + 0x14, 0x101);
            expect(qts, b + 0x18, before);
            qtest_clock_step(qts, 9000);
            expect(qts, b + 0x14, 0x101);
            expect(qts, b + 0x18, before);
        }
        /* Mask changes after completion are valid and must not rearm. */
        qtest_writel(qts, b + 0x14, 0x105);
        expect(qts, b + 0x14, 0x105);
        qtest_clock_step(qts, 9000);
        expect(qts, b + 0x18, before);
        /* Only disable then enable starts the queued next frame. */
        qtest_writel(qts, b + 0x14, 0);
        qtest_writel(qts, b + 0x18, 0x2000);
        qtest_writel(qts, b + 0x14, 0x101);
        spi_wait(qts, g);
        spi_xfer(qts, g, status, sizeof(status), rx);
        g_assert_cmphex(rx[1] & 2, ==, 2);
    }
    qtest_quit(qts);
}

static void test_spi_fifo_thresholds(const void *board)
{
    QTestState *qts = start_spi(board);
    static const unsigned threshold[] = { 1, 16, 32, 48 };

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = spi_base[g];

        for (unsigned mode = 0; mode < 4; mode++) {
            qtest_writel(qts, base + 8, 0);
            spi_setup(qts, g);
            qtest_writel(qts, base + 0x10, 0x41c00d00 | mode | (mode << 2));
            qtest_writel(qts, base + 0x1c, 3);
            for (unsigned i = 1; i < 64; i++) {
                qtest_writel(qts, base + 0x1c, 0);
            }
            qtest_writel(qts, base + 0x14, (64U << 8) | (64U << 20) | 3);
            for (unsigned i = 1; i <= 64; i++) {
                unsigned flags = 0;

                qtest_clock_step(qts, 9000);
                if (i >= threshold[mode]) {
                    flags |= 0x200;
                }
                if (64 - i < threshold[mode]) {
                    flags |= 0x100;
                }
                g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x300,
                                 ==, flags);
            }
            qtest_writel(qts, base + 0x18, 0x200);
            g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x200, ==, 0x200);
            qtest_writel(qts, base + 0x14, 0); /* Disable TX level condition. */
            qtest_writel(qts, base + 0x18, 0x20700); /* RX clear + W1C. */
            g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x700, ==, 0);
        }
        qtest_writel(qts, base + 8, 0);
    }
    qtest_quit(qts);
}

static void test_spi_clock_cancel(const void *board)
{
    QTestState *qts = start_spi(board);
    const uint8_t read_status[] = { 0x05, 0 };
    uint8_t rx[2];

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = spi_base[g];
        g_autofree char *path = g_strdup_printf("/machine/soc/spi[%u]/pclk", g);

        spi_setup(qts, g);
        qtest_writel(qts, base + 0x1c, 4);
        qtest_writel(qts, base + 0x14, 0x101);
        qtest_clock_step(qts, 1);
        qtest_writel(qts, base + 0x10, 0x41c00d10); /* IRQ mask only. */
        qtest_writel(qts, base + 8, 1); /* Bypass readback only. */
        qtest_clock_step(qts, 8998);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0);
        qtest_clock_step(qts, 1); /* Register updates must not restart phase. */
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0x2000);
        qtest_writel(qts, base + 0x14, 0);
        qtest_writel(qts, base + 0x18, 0x37f00);
        spi_setup(qts, g);
        qtest_writel(qts, SYS + 0x30, 0);
        /* The queued write-enable opcode must not reach the slave yet. */
        qtest_writel(qts, base + 0x1c, 6);
        qtest_writel(qts, base + 0x14, 0x101);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0);
        qtest_writel(qts, SYS + 0x30, 1U << spi_gate[g]);
        qtest_clock_step(qts, 4000);
        qtest_writel(qts, SYS + 0x28, 1U << (4 + g)); /* Missing APLL. */
        expect_clock(qts, path, 0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0);
        qtest_writel(qts, base + 0x10, 0x41c00100); /* Live divider rejected. */
        expect(qts, base + 0x10, 0x41c00d00);
        qtest_writel(qts, SYS + 0x28, 0);
        expect_clock(qts, path, 26000000);
        qtest_clock_step(qts, 4999);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, base + 0x18) & 0x2000, ==, 0x2000);
        spi_xfer(qts, g, read_status, sizeof(read_status), rx);
        g_assert_cmphex(rx[1] & 2, ==, 2);
        qtest_writel(qts, base + 0x14, 0);
        qtest_writel(qts, base + 0x18, 0x37f00);
        qtest_writel(qts, base + 0x1c, 4); /* Cancel WRDI before delivery. */
        qtest_writel(qts, base + 0x14, 0x101);
        qtest_clock_step(qts, 4000);
        qtest_writel(qts, base + 8, 0);
        qtest_clock_step(qts, 1000000);
        expect(qts, base + 0x18, 0);
        spi_setup(qts, g);
        spi_xfer(qts, g, read_status, sizeof(read_status), rx);
        g_assert_cmphex(rx[1] & 2, ==, 2); /* Reset did not deliver WRDI. */
        qtest_writel(qts, base + 0x14, 0);
        qtest_writel(qts, base + 0x10, 0x41c00000); /* Raw divider0 unknown. */
        expect(qts, base + 0x10, 0x41c00d00);
        qtest_writel(qts, base + 0x10, 0x41c40d00); /* 16-bit unsupported. */
        expect(qts, base + 0x10, 0x41c00d00);
        qtest_writel(qts, base + 0x14, 0x0000040f); /* Mismatched RX count. */
        expect(qts, base + 0x14, 0);
        qtest_system_reset(qts);
        expect(qts, base + 8, 0);
        expect(qts, base + 0x18, 0);
    }
    qtest_quit(qts);
}

#define SPI_APLL_ARGS "-global bk7258-soc.experimental-spi-apll=on"
#define SPI_APLL_CONTROL 0xc2a0ae86U
#define SPI_APLL_TRIGGER (1U << 18)
#define SPI_APLL_48K 0x8973ca6fU
#define SPI_APLL_44K1 0x88af2ec9U
#define SPI_APLL_GATES ((1U << 1) | (1U << 9))
#define SPI_APLL_SELECT ((1U << 4) | (1U << 5))

static QTestState *start_spi_ideal_apll(const void *board, const char *args)
{
    /* Standard SSI test endpoints, not a claim about any board's BOM. */
    return qtest_initf("-machine %s -serial null " SPI_APLL_ARGS
                      " -device w25q32,bus=spi0,cs=0 "
                      "-device w25q32,bus=spi1,cs=0 %s",
                      (const char *)board, args);
}

static void spi_ideal_apll_write(QTestState *qts, unsigned reg, uint32_t value)
{
    qtest_writel(qts, SYS + 0x100 + 4 * reg, value);
    expect(qts, SYS + 0xe8, 1U << reg);
    qtest_clock_step(qts, 1000);
    expect(qts, SYS + 0xe8, 0);
    expect(qts, SYS + 0x100 + 4 * reg, value);
}

static void spi_ideal_apll_pulse(QTestState *qts)
{
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    expect_clock(qts, "/machine/soc/apllclk", 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
}

static void spi_ideal_apll_program(QTestState *qts, uint32_t coefficient)
{
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    spi_ideal_apll_write(qts, 26, coefficient);
    spi_ideal_apll_pulse(qts);
}

static void expect_spi_ideal_clocks(QTestState *qts, unsigned apll,
                                  unsigned spi0, unsigned spi1)
{
    expect_clock(qts, "/machine/soc/apllclk", apll);
    expect_clock(qts, "/machine/soc/spiclk0", spi0);
    expect_clock(qts, "/machine/soc/spiclk1", spi1);
    expect_clock(qts, "/machine/soc/spi[0]/pclk", spi0);
    expect_clock(qts, "/machine/soc/spi[1]/pclk", spi1);
}

static void spi_ideal_setup(QTestState *qts, unsigned unit, unsigned divider)
{
    qtest_writel(qts, spi_base[unit] + 8, 3);
    /* Eight-bit master, interval0; divider2 keeps nominal SCK below 40 MHz. */
    g_assert_cmpuint(divider, >=, 2);
    qtest_writel(qts, spi_base[unit] + 0x10, 0x40c00000 | (divider << 8));
}

static void spi_ideal_start_opcode(QTestState *qts, unsigned unit,
                                  uint8_t opcode)
{
    uint32_t b = spi_base[unit];

    qtest_writel(qts, b + 0x14, 0);
    qtest_writel(qts, b + 0x18, 0x37f00);
    qtest_writel(qts, b + 0x1c, opcode);
    qtest_writel(qts, b + 0x14, 0x105);
}

static void spi_ideal_finish_after(QTestState *qts, unsigned unit, unsigned ns)
{
    qtest_clock_step(qts, ns - 1);
    g_assert_cmphex(qtest_readl(qts, spi_base[unit] + 0x18) & 0x7800, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, spi_base[unit] + 0x18) & 0x7800,
                   ==, 0x2000);
}

static void spi_ideal_expect_wel(QTestState *qts, unsigned unit, bool enabled)
{
    const uint8_t status[] = { 0x05, 0 };
    uint8_t rx[2];

    spi_xfer(qts, unit, status, sizeof(status), rx);
    g_assert_cmphex(rx[1] & 2, ==, enabled ? 2 : 0);
}

static void spi_ideal_timed_id(QTestState *qts, unsigned unit, unsigned ns)
{
    const uint8_t id[] = { 0x9f, 0, 0, 0 };
    const uint8_t expected[] = { 0, 0xef, 0x40, 0x16 };
    uint32_t b = spi_base[unit];

    qtest_writel(qts, b + 0x14, 0);
    qtest_writel(qts, b + 0x18, 0x37f00);
    for (unsigned i = 0; i < sizeof(id); i++) {
        qtest_writel(qts, b + 0x1c, id[i]);
    }
    qtest_writel(qts, b + 0x14, 0x00400403);
    for (unsigned i = 0; i < sizeof(id); i++) {
        qtest_clock_step(qts, ns - 1);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7804, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7804,
                       ==, i == sizeof(id) - 1 ? 0x6004 : 4);
        expect(qts, b + 0x1c, expected[i]);
    }
}

static void test_spi_ideal_apll_profiles(const void *board)
{
    static const struct {
        uint32_t coefficient;
        unsigned hz, byte_ns;
    } profiles[] = {
        { SPI_APLL_48K, 98304000, 326 },
        { SPI_APLL_44K1, 90316800, 355 },
    };
    QTestState *qts = start_spi_ideal_apll(board, "");

    g_assert_true(qtest_qom_get_bool(qts, "/machine/soc",
                                   "experimental-spi-apll"));
    for (unsigned i = 0; i < G_N_ELEMENTS(profiles); i++) {
        unsigned hz = profiles[i].hz;

        qtest_writel(qts, SYS + 0x30, 0);
        qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
        spi_ideal_apll_program(qts, profiles[i].coefficient);
        expect_spi_ideal_clocks(qts, hz, 0, 0);
        qtest_writel(qts, SYS + 0x30, 1U << spi_gate[0]);
        expect_spi_ideal_clocks(qts, hz, hz, 0);
        qtest_writel(qts, SYS + 0x30, 1U << spi_gate[1]);
        expect_spi_ideal_clocks(qts, hz, 0, hz);
        qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
        qtest_writel(qts, SYS + 0x28, 1U << 4);
        expect_spi_ideal_clocks(qts, hz, hz, 26000000);
        qtest_writel(qts, SYS + 0x28, 1U << 5);
        expect_spi_ideal_clocks(qts, hz, 26000000, hz);
        qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
        for (unsigned unit = 0; unit < 2; unit++) {
            spi_ideal_setup(qts, unit, 2);
            spi_ideal_timed_id(qts, unit, profiles[i].byte_ns);
            spi_ideal_start_opcode(qts, unit, 0x06);
            spi_ideal_finish_after(qts, unit, profiles[i].byte_ns);
            spi_ideal_expect_wel(qts, unit, true);
            spi_ideal_start_opcode(qts, unit, 0x04);
            spi_ideal_finish_after(qts, unit, profiles[i].byte_ns);
            spi_ideal_expect_wel(qts, unit, false);
        }
    }
    qtest_quit(qts);
}

static void test_spi_ideal_apll_committed_pulse(const void *board)
{
    QTestState *qts = start_spi_ideal_apll(board, "");

    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL);
    qtest_writel(qts, 0x54010168, SPI_APLL_48K);
    expect(qts, SYS + 0xe8, (1U << 25) | (1U << 26));
    qtest_clock_step(qts, 999);
    expect(qts, SYS + 0x164, 0);
    expect(qts, SYS + 0x168, 0);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0xe8, 0);
    expect(qts, SYS + 0x164, SPI_APLL_CONTROL);
    expect(qts, SYS + 0x168, SPI_APLL_48K);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL); /* No pulse. */
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_writel(qts, 0x54010164, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL); /* Busy, not an edge. */
    qtest_clock_step(qts, 999);
    expect(qts, SYS + 0x164, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0x164, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_clock_step(qts, 1000000); /* A high-only pulse never supplies SPI. */
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL);
    qtest_clock_step(qts, 999);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_clock_step(qts, 1);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    /* No-op commits preserve both the active source and an armed pulse. */
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    spi_ideal_apll_write(qts, 26, SPI_APLL_48K);
    spi_ideal_apll_write(qts, 5, 0);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 26, SPI_APLL_48K);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    /* A committed profile change invalidates the old source and pulse. */
    qtest_writel(qts, SYS + 0x168, SPI_APLL_44K1);
    qtest_clock_step(qts, 999);
    qtest_writel(qts, SYS + 0x168, SPI_APLL_48K); /* Cannot replace it. */
    expect(qts, SYS + 0x168, SPI_APLL_48K);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0x168, SPI_APLL_44K1);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 26, SPI_APLL_48K); /* Changes while high. */
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    /* Powerdown commits after 1000 ns and cannot retain calibration state. */
    qtest_writel(qts, SYS + 0x114, 1U << 13);
    qtest_clock_step(qts, 999);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    qtest_clock_step(qts, 1);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 5, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    qtest_quit(qts);
}

static void test_spi_ideal_apll_cycle_budget(const void *board)
{
    QTestState *qts = start_spi_ideal_apll(board, "");

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = spi_base[unit];

        spi_ideal_apll_program(qts, SPI_APLL_48K);
        qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
        qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
        spi_ideal_setup(qts, unit, 2);
        spi_ideal_start_opcode(qts, unit, 0x06);
        for (unsigned n = 0; n < 64; n++) {
            qtest_writel(qts, SYS + 0x30, 0);
            qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
        }
        spi_ideal_finish_after(qts, unit, 326);
        spi_ideal_expect_wel(qts, unit, true);

        spi_ideal_start_opcode(qts, unit, 0x04);
        qtest_clock_step(qts, 100); /* Nine of 32 source cycles elapsed. */
        qtest_writel(qts, SYS + 0x30,
                     SPI_APLL_GATES & ~(1U << spi_gate[unit]));
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7800, ==, 0);
        qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
        spi_ideal_finish_after(qts, unit, 234); /* Remaining 23 cycles. */
        spi_ideal_expect_wel(qts, unit, false);

        spi_ideal_start_opcode(qts, unit, 0x06);
        qtest_clock_step(qts, 100);
        qtest_writel(qts, SYS + 0x28,
                     SPI_APLL_SELECT & ~(1U << (4 + unit)));
        spi_ideal_finish_after(qts, unit, 885); /* 23 cycles at XTAL. */
        spi_ideal_expect_wel(qts, unit, true);

        qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
        qtest_writel(qts, b + 0x14, 0);
        spi_ideal_setup(qts, unit, 13);
        spi_ideal_start_opcode(qts, unit, 0x04);
        qtest_clock_step(qts, 100);
        qtest_writel(qts, SYS + 0x168, SPI_APLL_44K1);
        qtest_clock_step(qts, 1000); /* 108 of 208 cycles elapsed. */
        expect_spi_ideal_clocks(qts, 0, 0, 0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7800, ==, 0);
        spi_ideal_apll_pulse(qts);
        expect_spi_ideal_clocks(qts, 90316800, 90316800, 90316800);
        spi_ideal_finish_after(qts, unit, 1108); /* 100 cycles, new rate. */
        spi_ideal_expect_wel(qts, unit, false);

        spi_ideal_start_opcode(qts, unit, 0x06);
        qtest_clock_step(qts, 100);
        spi_ideal_apll_write(qts, 5, 1U << 13);
        /* 99 cycles elapsed before powerdown; 109 remain at 90.3168 MHz. */
        expect_spi_ideal_clocks(qts, 0, 0, 0);
        spi_ideal_apll_write(qts, 5, 0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7800, ==, 0);
        spi_ideal_apll_pulse(qts);
        spi_ideal_finish_after(qts, unit, 1207);
        spi_ideal_expect_wel(qts, unit, true);
        spi_ideal_start_opcode(qts, unit, 0x04);
        spi_ideal_finish_after(qts, unit, 2304);
    }
    qtest_quit(qts);
}

static void test_spi_ideal_apll_atomic_commit(const void *board)
{
    QTestState *qts = start_spi_ideal_apll(board, "");

    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    spi_ideal_apll_write(qts, 5, 1U << 13);
    /* Ideal policy: a rising-edge snapshot may establish the profile. */
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x168, SPI_APLL_48K);
    qtest_writel(qts, SYS + 0x114, 0);
    qtest_clock_step(qts, 999);
    expect(qts, SYS + 0xe8, (1U << 5) | (1U << 25) | (1U << 26));
    expect(qts, SYS + 0x114, 1U << 13);
    expect(qts, SYS + 0x164, 0);
    expect(qts, SYS + 0x168, 0);
    qtest_clock_step(qts, 1);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 5, 1U << 5); /* Unrelated ANA5 change. */
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    spi_ideal_apll_write(qts, 5, 0);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL);
    qtest_writel(qts, SYS + 0x168, SPI_APLL_44K1);
    qtest_clock_step(qts, 1000);
    /* Changed coefficient on the falling commit invalidates the pulse first. */
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_writel(qts, SYS + 0x168, SPI_APLL_48K);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_clock_step(qts, 1000);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x114, 1U << 13);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL);
    qtest_clock_step(qts, 1000);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL);
    /* Powerup on the falling edge is not enough. */
    qtest_writel(qts, SYS + 0x114, 0);
    qtest_clock_step(qts, 1000);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL ^ 1U);
    qtest_writel(qts, SYS + 0x114, 1U << 5);
    qtest_clock_step(qts, 1000);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    qtest_quit(qts);
}

static void test_spi_ideal_apll_unknown_profile(const void *board)
{
    g_autofree char *dir = g_dir_make_tmp("bk7258-ideal-apll-XXXXXX", NULL);
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    g_autofree char *log = NULL;
    gsize before;
    QTestState *qts;

    g_assert_nonnull(dir);
    path = g_build_filename(dir, "unimplemented.log", NULL);
    args = g_strdup_printf("-d unimp,guest_errors -D %s", path);
    qts = start_spi_ideal_apll(board, args);
    before = core_clock_log_size(path);
    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    spi_ideal_apll_program(qts, SPI_APLL_48K);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 5, 1U << 13);
    spi_ideal_apll_write(qts, 5, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    /* Missing/incomplete pulses, no-ops, and known powerdown are silent. */
    g_assert_cmpuint(core_clock_log_size(path), ==, before);
    spi_ideal_apll_write(qts, 26, 0xdeadbeef);
    g_assert_cmpuint(core_clock_log_size(path), >, before);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 26, SPI_APLL_48K);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    before = core_clock_log_size(path);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL ^ 1U);
    g_assert_cmpuint(core_clock_log_size(path), >, before);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25,
                         (SPI_APLL_CONTROL ^ 1U) | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL ^ 1U);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);

    /* Changing away/back while high must not fabricate another rising edge. */
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 25,
                         (SPI_APLL_CONTROL ^ 1U) | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
    spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    spi_ideal_apll_pulse(qts);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "ideal SPI APLL coefficient 0xdeadbeef "
                                "unsupported"));
    g_assert_nonnull(strstr(log, "ideal SPI APLL control 0xc2a0ae87 "
                                "unsupported"));
    g_assert_null(strstr(log, "control 0xc2a4ae87"));
    g_assert_null(strstr(log, "bk7258-sys: write offset"));
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_spi_ideal_apll_reset_cancel(const void *board)
{
    QTestState *qts = start_spi_ideal_apll(board, "");

    spi_ideal_apll_program(qts, SPI_APLL_48K);
    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    for (unsigned reset_unit = 0; reset_unit < 2; reset_unit++) {
        unsigned other = reset_unit ^ 1;

        for (unsigned unit = 0; unit < 2; unit++) {
            spi_ideal_setup(qts, unit, 2);
            spi_ideal_start_opcode(qts, unit, 0x06);
        }
        qtest_clock_step(qts, 100);
        qtest_writel(qts, spi_base[reset_unit] + 8, 0);
        expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
        spi_ideal_finish_after(qts, other, 226);
        qtest_clock_step(qts, 1000000);
        expect(qts, spi_base[reset_unit] + 0x18, 0);
        spi_ideal_setup(qts, reset_unit, 2);
        spi_ideal_expect_wel(qts, reset_unit, false);
        spi_ideal_expect_wel(qts, other, true);
        spi_ideal_start_opcode(qts, other, 0x04);
        spi_ideal_finish_after(qts, other, 326);

        /* A peripheral reset also leaves a pending SoC pulse intact. */
        spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
        qtest_writel(qts, spi_base[reset_unit] + 8, 0);
        spi_ideal_apll_write(qts, 25, SPI_APLL_CONTROL);
        expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    }

    for (unsigned phase = 0; phase < 2; phase++) {
        /* Reset cancels either edge's uncommitted analog transfer. */
        if (phase) {
            spi_ideal_apll_program(qts, SPI_APLL_48K);
            spi_ideal_apll_write(qts, 25,
                                 SPI_APLL_CONTROL | SPI_APLL_TRIGGER);
        }
        qtest_writel(qts, SYS + 0x30, 0);
        for (unsigned unit = 0; unit < 2; unit++) {
            spi_ideal_setup(qts, unit, 2);
            spi_ideal_start_opcode(qts, unit, 0x06);
        }
        qtest_writel(qts, SYS + 0x164, SPI_APLL_CONTROL |
                     (phase ? 0 : SPI_APLL_TRIGGER));
        qtest_clock_step(qts, 999);
        qtest_system_reset(qts);
        qtest_clock_step(qts, 1000000);
        expect(qts, SYS + 0xe8, 0);
        expect(qts, SYS + 0x114, 0);
        expect(qts, SYS + 0x164, 0);
        expect(qts, SYS + 0x168, 0);
        expect_spi_ideal_clocks(qts, 0, 0, 0);
        for (unsigned unit = 0; unit < 2; unit++) {
            expect(qts, spi_base[unit] + 8, 0);
            expect(qts, spi_base[unit] + 0x18, 0);
        }
        spi_ideal_apll_program(qts, SPI_APLL_48K);
        qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
        qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
        for (unsigned unit = 0; unit < 2; unit++) {
            spi_ideal_setup(qts, unit, 2);
            spi_ideal_expect_wel(qts, unit, false);
        }
    }
    qtest_quit(qts);
}

static void test_spi_ideal_apll_independence(const void *board)
{
    static const unsigned uart_gate[] = { 2, 10, 11 };
    static const unsigned uart_shift[] = { 8, 11, 14 };
    QTestState *qts = start_spi(board);

    g_assert_false(qtest_qom_get_bool(qts, "/machine/soc",
                                    "experimental-spi-apll"));
    spi_ideal_apll_program(qts, SPI_APLL_48K);
    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES);
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    expect_spi_ideal_clocks(qts, 0, 0, 0);
    qtest_writel(qts, SYS + 0x28, 0);
    expect_spi_ideal_clocks(qts, 0, 26000000, 26000000);
    for (unsigned unit = 0; unit < 2; unit++) {
        spi_ideal_setup(qts, unit, 2);
        spi_ideal_timed_id(qts, unit, 1231);
    }
    qtest_quit(qts);

    qts = start_spi_ideal_apll(board, CORE_CLOCK_ARGS);
    core_clock_dpll(qts, true);
    core_clock_speeds(qts, 7, false);
    core_clock_mode(qts, 0x33, false);
    spi_ideal_apll_program(qts, SPI_APLL_48K);
    expect_core_clocks(qts, 120000000, 120000000, 120000000, 120000000);
    expect_clock(qts, "/machine/soc/xtalclk", 26000000);
    qtest_writel(qts, SYS + 0x30, SPI_APLL_GATES | (1U << 2) |
                 (1U << 10) | (1U << 11));
    qtest_writel(qts, SYS + 0x28, SPI_APLL_SELECT);
    for (unsigned unit = 0; unit < 3; unit++) {
        g_autofree char *path =
            g_strdup_printf("/machine/soc/uart[%u]/pclk", unit);

        for (unsigned mode = 0; mode < 8; mode++) {
            qtest_writel(qts, SYS + 0x20, 0x33 | (mode << uart_shift[unit]));
            expect_clock(qts, path, mode & 4 ? 0 : 26000000 >> (mode & 3));
            expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
        }
        qtest_writel(qts, SYS + 0x30,
                     qtest_readl(qts, SYS + 0x30) & ~(1U << uart_gate[unit]));
        expect_clock(qts, path, 0);
    }
    /* DPLL changes and core tuples do not retune the nominal SPI source. */
    core_clock_dpll(qts, false);
    expect_core_clocks(qts, 0, 0, 0, 0);
    expect_spi_ideal_clocks(qts, 98304000, 98304000, 98304000);
    core_clock_mode(qts, 0, false);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    qtest_writel(qts, SYS + 0x28, 0);
    for (unsigned unit = 0; unit < 2; unit++) {
        spi_ideal_setup(qts, unit, 13);
        spi_ideal_start_opcode(qts, unit, 0x06);
    }
    qtest_clock_step(qts, 100);
    spi_ideal_apll_write(qts, 5, 1U << 13);
    expect_spi_ideal_clocks(qts, 0, 26000000, 26000000);
    expect_core_clocks(qts, 26000000, 26000000, 26000000, 26000000);
    expect_clock(qts, "/machine/soc/xtalclk", 26000000);
    spi_ideal_finish_after(qts, 0, 6900); /* Original XTAL deadline. */
    g_assert_cmphex(qtest_readl(qts, spi_base[1] + 0x18) & 0x7800,
                   ==, 0x2000);
    spi_ideal_expect_wel(qts, 0, true);
    spi_ideal_expect_wel(qts, 1, true);
    qtest_quit(qts);
}

static const uint32_t i2c_base[] = { 0x45850000, 0x45860000 };
static const unsigned i2c_gate[] = { 0, 8 };
static const unsigned i2c_irq[] = { 6, 14 };

static QTestState *start_i2c(const void *board)
{
    return qtest_initf("-machine %s -serial null "
                      "-device at24c-eeprom,bus=i2c0,address=0x50,rom-size=256 "
                      "-device at24c-eeprom,bus=i2c1,address=0x50,rom-size=256",
                      (const char *)board);
}

static void i2c_setup(QTestState *qts, unsigned g)
{
    qtest_writel(qts, SYS + 0x30, (1U << i2c_gate[0]) | (1U << i2c_gate[1]));
    qtest_writel(qts, i2c_base[g] + 8, 3);
    qtest_writel(qts, i2c_base[g] + 0x10, 0xcc000000 | (7U << 6));
    qtest_writel(qts, i2c_base[g] + 0x1c, 3);
}

static void i2c_wait_irq(QTestState *qts, unsigned g)
{
    for (unsigned step = 0; step < 32; step++) {
        if (qtest_readl(qts, i2c_base[g] + 0x14) & 1) {
            return;
        }
        qtest_clock_step(qts, 20000);
    }
    g_assert_not_reached();
}

static void i2c_command(QTestState *qts, unsigned g, unsigned value)
{
    qtest_writel(qts, i2c_base[g] + 0x14, value);
    i2c_wait_irq(qts, g);
}

static void i2c_address(QTestState *qts, unsigned g, unsigned byte, bool ack)
{
    qtest_writel(qts, i2c_base[g] + 0x18, byte);
    i2c_command(qts, g, 0x400);
    g_assert_cmphex(qtest_readl(qts, i2c_base[g] + 0x14) & 0x501, ==,
                   0x401 | (ack ? 0x100 : 0));
}

static void i2c_stop(QTestState *qts, unsigned g)
{
    qtest_writel(qts, i2c_base[g] + 0x14, 0x200);
    qtest_clock_step(qts, 20000);
    g_assert_cmphex(qtest_readl(qts, i2c_base[g] + 0x14) & 0xc601, ==, 0);
}

static void i2c_pointer(QTestState *qts, unsigned g, unsigned offset)
{
    i2c_address(qts, g, 0xa0, true);
    qtest_writel(qts, i2c_base[g] + 0x18, offset);
    i2c_command(qts, g, 0x100);
    i2c_address(qts, g, 0xa1, true); /* Repeated START, no STOP. */
}

static void test_i2c_fifo_transactions(const void *board)
{
    QTestState *qts = start_i2c(board);
    static const unsigned receive_count[] = { 12, 8, 4, 1 };

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = i2c_base[g];

        i2c_setup(qts, g);
        for (unsigned mode = 0; mode < 4; mode++) {
            i2c_address(qts, g, 0xa0, true);
            qtest_writel(qts, base + 0x18, 0x20);
            for (unsigned i = 0; i < 15; i++) {
                qtest_writel(qts, base + 0x18, 0x40 + i + 16 * g + mode);
            }
            g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x20, ==, 0x20);
            /* Full FIFO rejects byte 17, preserving the queued payload. */
            qtest_writel(qts, base + 0x18, 0xee);
            i2c_command(qts, g, 0x100 | (mode << 6));
            if (mode) {
                i2c_command(qts, g, 0x100); /* Drain the remaining threshold. */
            }
            i2c_stop(qts, g);
            i2c_pointer(qts, g, 0x20);
            i2c_command(qts, g, 0x100 | (mode << 6));
            for (unsigned i = 0; i < receive_count[mode]; i++) {
                expect(qts, base + 0x10000018, 0x40 + i + 16 * g + mode);
            }
            g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x10, ==, 0x10);
            i2c_stop(qts, g);
            /* Byte 17 must not have slipped through after the last write. */
            i2c_pointer(qts, g, 0x2f);
            i2c_command(qts, g, 0x1c0);
            expect(qts, base + 0x18, 0);
            i2c_stop(qts, g);
        }
    }
    qtest_quit(qts);
}

static void test_i2c_w0c_nak_routes(const void *board)
{
    QTestState *qts = start_i2c(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = i2c_base[g], irq = 1U << i2c_irq[g];

        i2c_setup(qts, g);
        /* Contradictory START and STOP must not change controller state. */
        qtest_writel(qts, base + 0x14, 0x600);
        expect(qts, base + 0x14, 0x10);
        qtest_writel(qts, base + 0x14, 1); /* Cannot invent an interrupt. */
        expect(qts, base + 0x14, 0x10);
        i2c_address(qts, g, 0xa2, false);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core, 0);
            qtest_writel(qts, SYS + 0x80 + 8 * core, irq);
            expect(qts, SYS + 0xa0 + 8 * core, irq);
        }
        /* A write-one preserves pending SM_INT; it is not W1C. */
        qtest_writel(qts, base + 0x14, 0x501);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x501, ==, 0x401);
        qtest_clock_step(qts, 100000);
        expect(qts, SYS + 0xa0, irq);
        qtest_writel(qts, base + 0x14, 0x100);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x101, ==, 1);
        i2c_stop(qts, g);
        expect(qts, SYS + 0xa0, 0);
        i2c_address(qts, g, 0xa0, true);
        qtest_writel(qts, base + 0x18, 0xa2);
        qtest_writel(qts, base + 0x14, 0x400);
        /* Different-address repeated START is explicitly outside the API. */
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x501, ==, 0x501);
        qtest_writel(qts, base + 8, 0); /* Invalid command did not alter bus. */
        expect(qts, SYS + 0xa0, 0);
        expect(qts, base + 0x14, 0x10);
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core, 0);
        }
    }
    qtest_quit(qts);
}

static void test_i2c_clock_reset_cancel(const void *board)
{
    QTestState *qts = start_i2c(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = i2c_base[g];

        i2c_setup(qts, g);
        qtest_writel(qts, base + 0x18, 0xa0);
        qtest_writel(qts, base + 0x14, 0x400);
        qtest_clock_step(qts, 5000);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x501, ==, 0x400);
        qtest_writel(qts, SYS + 0x30, 1U << i2c_gate[g]);
        /* 270 - 130 elapsed = 140 source cycles, or ceil(5384.615 ns). */
        qtest_clock_step(qts, 5384);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 1, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x501, ==, 0x501);
        qtest_writel(qts, base + 0x18, 0x40);
        i2c_command(qts, g, 0x100); /* Set EEPROM pointer, not its data. */
        qtest_writel(qts, base + 0x18, 0xee);
        qtest_writel(qts, base + 0x14, 0x100);
        qtest_clock_step(qts, 5000);
        qtest_writel(qts, base + 8, 0);
        qtest_clock_step(qts, 1000000);
        expect(qts, base + 0x14, 0x10);
        i2c_setup(qts, g);
        i2c_pointer(qts, g, 0x40);
        i2c_command(qts, g, 0x1c0);
        expect(qts, base + 0x18, 0); /* Canceled byte was never delivered. */
        i2c_stop(qts, g);
        qtest_writel(qts, base + 0x10, 0xc0000000 | (7U << 6));
        qtest_writel(qts, base + 0x18, 0xa0);
        qtest_writel(qts, base + 0x14, 0x400);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readl(qts, base + 0x14) & 0x101, ==, 0);
        qtest_system_reset(qts);
        expect(qts, base + 8, 0);
        expect(qts, base + 0x14, 0x10);
    }
    qtest_quit(qts);
}

static void test_i2c_unchanged_deadline(const void *board)
{
    QTestState *qts = start_i2c(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t b = i2c_base[g];

        for (unsigned mode = 0; mode < 3; mode++) {
            i2c_setup(qts, g);
            qtest_writel(qts, b + 0x10, 0x8c000000);
            qtest_writel(qts, b + 0x18, 0xa0);
            qtest_writel(qts, b + 0x14, 0x400);
            for (unsigned i = 0; i < 100; i++) {
                qtest_clock_step(qts, 1);
                if (!mode) {
                    qtest_writel(qts, b + 0x10, 0x8c000000);
                } else {
                    qtest_writel(qts, b + 8,
                                 mode == 1 ? 3 : 1 | ((i & 1) << 1));
                }
            }
            /* 81 cycles at 26 MHz: an unchanged clock completes at 3116 ns. */
            qtest_clock_step(qts, 3015);
            g_assert_cmphex(qtest_readl(qts, b + 0x14) & 0x501, ==, 0x400);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readl(qts, b + 0x14) & 0x501, ==, 0x501);
            i2c_stop(qts, g);
        }
    }
    qtest_quit(qts);
}

static const uint32_t timg_base[] = { 0x44810000, 0x45800000 };
static const unsigned timg_gate[] = { 4, 13 };
static const unsigned timg_irq[] = { 3, 13 };

static void timg_reset(QTestState *qts, unsigned group)
{
    qtest_writel(qts, timg_base[group] + 8, 0);
    qtest_writel(qts, timg_base[group] + 8, 1);
    qtest_writel(qts, SYS + 0x30, 1U << timg_gate[group]);
    qtest_writel(qts, SYS + 0x20, 0); /* CLK32 selection. */
}

static void test_timg_channels(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = timg_base[g], irq = 1U << timg_irq[g];

        timg_reset(qts, g);
        for (unsigned i = 0; i < 3; i++) {
            qtest_writel(qts, base + 0x10 + 4 * i, 3 + 2 * i);
            qtest_writel(qts, SYS + 0x80 + 8 * i, irq);
        }
        qtest_writel(qts, base + 0x1c, 7);
        qtest_clock_step(qts, 3 * 31250);
        expect(qts, base + 0x1c, 0x87);
        for (unsigned i = 0; i < 3; i++) {
            expect(qts, SYS + 0xa0 + 8 * i, irq);
        }
        qtest_writel(qts, SYS + 0x88, 0);
        expect(qts, SYS + 0xa8, 0);
        qtest_clock_step(qts, 2 * 31250);
        expect(qts, base + 0x1c, 0x187);
        qtest_writel(qts, base + 0x1c, 0x87);
        expect(qts, base + 0x1c, 0x107); /* Other channel remains pending. */
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x1c, 0x187); /* ACK did not restart the count. */
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x1c, 0x387);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_clock_step(qts, 1000000);
        expect(qts, base + 0x1c, 0x387);
        qtest_writel(qts, SYS + 0x88, irq);
        expect(qts, SYS + 0xa8, irq); /* Clock gating does not ACK IRQ. */
        qtest_writel(qts, base + 0x1c, 0x387);
        expect(qts, base + 0x1c, 7);
        expect(qts, SYS + 0xa8, 0);
        qtest_writel(qts, base + 8, 0);
    }
    qtest_quit(qts);
}

static void test_timg_clock_snapshot(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = timg_base[g];
        g_autofree char *path = g_strdup_printf("/machine/soc/timer[%u]/pclk",
                                                g);

        timg_reset(qts, g);
        expect_clock(qts, path, 32000);
        qtest_writel(qts, base + 0x10, 1000);
        qtest_writel(qts, base + 0x1c, 1);
        qtest_clock_step(qts, 8 * 31250);
        qtest_writel(qts, base + 0x20, 1);
        expect(qts, base + 0x20, 1);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x20, 0);
        expect(qts, base + 0x24, 9);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_writel(qts, base + 0x20, 1);
        qtest_writel(qts, base + 0x20, 9); /* Busy rewrite is rejected. */
        qtest_clock_step(qts, 1000000);
        expect_clock(qts, path, 0);
        expect(qts, base + 0x20, 1);
        expect(qts, base + 0x24, 9);
        qtest_writel(qts, SYS + 0x30, 1U << timg_gate[g]);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x24, 10);
        qtest_writel(qts, base + 0x20, 13); /* No channel three exists. */
        expect(qts, base + 0x20, 0);
        qtest_writel(qts, 0x44000104, 1); /* External source unconnected. */
        qtest_writel(qts, base + 0x20, 1);
        qtest_clock_step(qts, 1000000);
        expect(qts, base + 0x20, 1);
        expect(qts, base + 0x24, 10);
        qtest_writel(qts, SYS + 0x20, 1U << (20 + g));
        expect_clock(qts, path, 26000000);
        qtest_clock_step(qts, 39);
        expect(qts, base + 0x20, 0);
        expect(qts, base + 0x24, 11);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_writel(qts, base + 0x20, 1);
        qtest_writel(qts, base + 8, 0); /* Pending snapshot canceled. */
        qtest_writel(qts, SYS + 0x30, 1U << timg_gate[g]);
        qtest_clock_step(qts, 1000000);
        expect(qts, base + 0x20, 0);
        expect(qts, base + 0x24, 0);
        expect(qts, base + 0x1c, 0);
        qtest_writel(qts, base + 0x10000008, 1); /* NS alias. */
        expect(qts, base + 8, 1);
        qtest_writel(qts, 0x44000104, 0);
    }
    qtest_system_reset(qts);
    for (unsigned g = 0; g < 2; g++) {
        expect(qts, timg_base[g] + 8, 0);
        expect(qts, timg_base[g] + 0x24, 0);
    }
    qtest_quit(qts);
}

static void test_timg_prescaler(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = timg_base[g];

        for (unsigned divisor = 1; divisor <= 16; divisor++) {
            unsigned control = 1 | ((divisor - 1) << 3);
            uint64_t period = 2 * divisor * 31250;

            timg_reset(qts, g);
            qtest_writel(qts, base + 0x10, 2);
            qtest_writel(qts, base + 0x1c, control);
            qtest_clock_step(qts, period - 1);
            expect(qts, base + 0x1c, control);
            qtest_clock_step(qts, 1);
            expect(qts, base + 0x1c, control | 0x80);
            qtest_writel(qts, base + 0x1c, control | 0x80);
            qtest_clock_step(qts, period - 1);
            expect(qts, base + 0x1c, control);
            qtest_clock_step(qts, 1);
            expect(qts, base + 0x1c, control | 0x80);
        }
        timg_reset(qts, g);
        qtest_writel(qts, base + 0x10, 3);
        qtest_writel(qts, base + 0x1c, 1 | (3U << 3));
        qtest_clock_step(qts, 6 * 31250); /* One tick plus two input edges. */
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_clock_step(qts, 1000000);
        qtest_writel(qts, SYS + 0x30, 1U << timg_gate[g]);
        qtest_clock_step(qts, 5 * 31250);
        expect(qts, base + 0x1c, 1 | (3U << 3));
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x1c, 0x81 | (3U << 3));
        qtest_writel(qts, base + 8, 0);
    }
    qtest_quit(qts);
}

static void test_timg_wrap(const void *board)
{
    QTestState *qts = start(board);
    const uint64_t wrap = UINT64_C(1) << 32;

    for (unsigned g = 0; g < 2; g++) {
        uint32_t base = timg_base[g];

        timg_reset(qts, g);
        qtest_writel(qts, base + 0x1c, 1); /* End zero: natural 32-bit wrap. */
        qtest_clock_step(qts, (wrap - 1) * 31250);
        expect(qts, base + 0x1c, 1);
        qtest_writel(qts, base + 0x20, 1);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x24, 0);
        expect(qts, base + 0x1c, 0x81);
        timg_reset(qts, g);
        qtest_writel(qts, base + 0x10, 100);
        qtest_writel(qts, base + 0x1c, 1);
        qtest_clock_step(qts, 10 * 31250);
        qtest_writel(qts, base + 0x10, 5); /* Below current count, no clamp. */
        qtest_clock_step(qts, (wrap - 10 + 4) * 31250);
        expect(qts, base + 0x1c, 1);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x1c, 0x81);
        /* Millions of elapsed short periods coalesce under a latched IRQ. */
        qtest_clock_step(qts, 100000000 * 31250LL);
        qtest_writel(qts, base + 0x20, 1);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x24, 1);
        qtest_writel(qts, base + 0x1c, 0x80); /* Disable clears counter. */
        qtest_writel(qts, base + 0x20, 1);
        qtest_clock_step(qts, 31250);
        expect(qts, base + 0x24, 0);
        expect(qts, base + 0x1c, 0);
        qtest_writel(qts, base + 8, 0);
    }
    qtest_quit(qts);
}

static void rtc_configure(QTestState *qts, uint64_t upper, uint64_t tick)
{
    /* Enabled synchronizer, counter held reset. */
    qtest_writel(qts, RTC, 0x43);
    qtest_writel(qts, RTC + 0x18, upper >> 32);
    qtest_writel(qts, RTC + 4, upper);
    qtest_writel(qts, RTC + 0x1c, tick >> 32);
    qtest_writel(qts, RTC + 8, tick);
    qtest_clock_step(qts, 3 * 31250);
}

static void test_rtc_counter_clock(const void *board)
{
    QTestState *qts = start(board);

    expect(qts, RTC, 0);
    qtest_writel(qts, RTC + 4, UINT32_MAX);
    qtest_writel(qts, RTC + 0x18, UINT32_MAX);
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC + 0x10, 0); /* Core clock is not enabled. */
    qtest_writel(qts, RTC, 0x43);
    qtest_clock_step(qts, 2 * 31250);
    qtest_writel(qts, RTC + 4, 15); /* Same-half rewrite while synchronizing. */
    expect(qts, RTC + 4, UINT32_MAX);
    expect(qts, RTC + 0x10, 0);
    qtest_clock_step(qts, 31250);
    expect(qts, RTC + 0x10, UINT32_MAX);
    expect(qts, RTC + 0x20, UINT32_MAX);
    qtest_writel(qts, RTC, 0x70);
    qtest_clock_step(qts, 31249);
    expect(qts, RTC + 0x0c, 0);
    qtest_clock_step(qts, 1);
    expect(qts, 0x5400020c, 1);
    qtest_writel(qts, RTC, 0x42);
    qtest_clock_step(qts, 10 * 31250);
    expect(qts, RTC + 0x0c, 1);
    qtest_writel(qts, RTC, 0x40);
    qtest_clock_step(qts, 3 * 31250);
    expect(qts, RTC + 0x0c, 4);
    qtest_writel(qts, 0x44000104, 1);
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC + 0x0c, 4);
    qtest_writel(qts, 0x44000104, 2);
    qtest_clock_step(qts, 2 * 31250);
    expect(qts, RTC + 0x0c, 6);
    qtest_writel(qts, SYS + 0x114, 1U << 14);
    qtest_clock_step(qts, 1000);
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC + 0x0c, 6);
    qtest_writel(qts, 0x44000104, 0);
    qtest_clock_step(qts, 31250);
    expect(qts, RTC + 0x0c, 7);
    qtest_writel(qts, RTC, 0);
    qtest_writel(qts, RTC + 8, 0x12345678);
    qtest_clock_step(qts, 10 * 31250);
    expect(qts, RTC + 0x14, 0);
    expect(qts, RTC + 0x0c, 7);
    qtest_writel(qts, RTC, 0x42);
    qtest_clock_step(qts, 2 * 31250);
    qtest_writel(qts, 0x44000104, 1);
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC + 0x14, 0); /* No fake transfer across a missing source. */
    qtest_writel(qts, 0x44000104, 0);
    qtest_clock_step(qts, 31250);
    expect(qts, RTC + 0x14, 0x12345678);
    expect(qts, RTC + 0x0c, 7);
    qtest_writel(qts, RTC, 0x41);
    qtest_clock_step(qts, 5 * 31250);
    expect(qts, RTC + 0x0c, 0);
    qtest_writel(qts, RTC, 0x40);
    qtest_clock_step(qts, (UINT64_C(1) << 32) * 31250);
    expect(qts, RTC + 0x0c, 0);
    expect(qts, RTC + 0x28, 1);
    qtest_writel(qts, RTC + 8, 5);
    qtest_system_reset(qts);
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC, 0);
    expect(qts, RTC + 0x14, 0);
    expect(qts, RTC + 0x28, 0);
    qtest_writel(qts, RTC + 0x0c, 0x99); /* Counter is read-only. */
    expect(qts, RTC + 0x0c, 0);
    qtest_quit(qts);
}

static void test_rtc_irq_w1c(const void *board)
{
    QTestState *qts = start(board);

    rtc_configure(qts, 5, 3);
    qtest_writel(qts, SYS + 0x84, 1U << 22);
    qtest_writel(qts, SYS + 0x8c, 1U << 22);
    qtest_writel(qts, RTC, 0x40);
    qtest_clock_step(qts, 3 * 31250);
    expect(qts, RTC, 0x60);
    expect(qts, SYS + 0xa4, 0);
    qtest_writel(qts, RTC, 0x48);
    expect(qts, SYS + 0xa4, 1U << 22);
    expect(qts, SYS + 0xac, 1U << 22);
    expect(qts, SYS + 0xb4, 0);
    qtest_writel(qts, RTC, 0x40);
    expect(qts, RTC, 0x60);
    expect(qts, SYS + 0xa4, 0);
    qtest_writel(qts, RTC, 0x68);
    expect(qts, RTC, 0x48);
    qtest_clock_step(qts, 2 * 31250);
    expect(qts, RTC + 0x0c, 5);
    expect(qts, RTC, 0x58);
    qtest_writel(qts, RTC, 0x44);
    expect(qts, SYS + 0xa4, 1U << 22);
    qtest_writel(qts, RTC, 0x54);
    expect(qts, SYS + 0xa4, 0);
    qtest_clock_step(qts, 31250);
    expect(qts, RTC + 0x0c, 0);
    qtest_clock_step(qts, 3 * 31250);
    expect(qts, RTC, 0x64);
    qtest_writel(qts, RTC, 0x6c);
    qtest_writel(qts, RTC + 8, 0);
    qtest_clock_step(qts, 3 * 31250);
    expect(qts, RTC + 0x14, 0);
    expect(qts, RTC, 0x5c); /* New equality does not synthesize an edge. */
    qtest_writel(qts, RTC, 0x5c);
    qtest_clock_step(qts, 6 * 31250);
    expect(qts, RTC, 0x7c);
    qtest_writel(qts, SYS + 0x94, 1U << 22);
    expect(qts, SYS + 0xb4, 1U << 22);
    qtest_writel(qts, RTC, 0x5c); /* Clear only upper; tick must remain. */
    expect(qts, RTC, 0x6c);
    expect(qts, SYS + 0xb4, 1U << 22);
    qtest_writel(qts, RTC, 0x6c);
    expect(qts, RTC, 0x4c);
    expect(qts, SYS + 0xb4, 0);
    qtest_clock_step(qts, 5 * 31250);
    expect(qts, SYS + 0xb4, 1U << 22);
    qtest_system_reset(qts);
    qtest_writel(qts, SYS + 0x94, 1U << 22);
    expect(qts, RTC, 0);
    expect(qts, SYS + 0xb4, 0);
    qtest_quit(qts);
}

static void test_rtc_upper64(const void *board)
{
    QTestState *qts = start(board);

    rtc_configure(qts, (UINT64_C(1) << 32) + 3, (UINT64_C(1) << 32) + 1);
    qtest_writel(qts, RTC, 0x40);
    qtest_clock_step(qts, ((UINT64_C(1) << 32) + 1) * 31250);
    expect(qts, RTC + 0x0c, 1);
    expect(qts, RTC + 0x28, 1);
    expect(qts, RTC, 0x60);
    qtest_clock_step(qts, 2 * 31250);
    expect(qts, RTC + 0x0c, 3);
    expect(qts, RTC + 0x28, 1);
    expect(qts, RTC, 0x70);
    qtest_clock_step(qts, 31250);
    expect(qts, RTC + 0x0c, 0);
    expect(qts, RTC + 0x28, 0);
    qtest_writel(qts, RTC, 0x70);
    qtest_clock_step(qts, 20 * 31250);
    qtest_writel(qts, RTC + 0x18, 0);
    qtest_writel(qts, RTC + 4, 5);
    qtest_clock_step(qts, 3 * 31250);
    expect(qts, RTC + 0x0c, 23); /* Lowered upper does not clamp the count. */
    qtest_clock_step(qts, 2 * 31250);
    expect(qts, RTC + 0x0c, 25);
    expect(qts, RTC, 0x40);
    qtest_system_reset(qts);
    qtest_writel(qts, RTC, 0x4c); /* Explicit inclusive-upper-zero policy. */
    qtest_clock_step(qts, 1000000);
    expect(qts, RTC + 0x0c, 0);
    expect(qts, RTC, 0x7c);
    qtest_writel(qts, RTC, 0x7c);
    qtest_clock_step(qts, 31249);
    expect(qts, RTC, 0x4c);
    qtest_clock_step(qts, 1);
    expect(qts, RTC, 0x7c);
    qtest_quit(qts);
}

static void test_gpio(const void *board)
{
    QTestState *qts = start(board);
    const uint32_t pin = 0x44000408;

    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 0);
    qtest_writel(qts, pin, 0x180c);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 1);
    expect(qts, 0x44000500, 4);
    expect(qts, SYS + 0xa4, 0);
    qtest_writel(qts, SYS + 0x84, 0x800000);
    expect(qts, SYS + 0xa4, 0x800000);
    qtest_writel(qts, pin, 0x080c); /* Mask a latched source. */
    expect(qts, SYS + 0xa4, 0);
    expect(qts, 0x44000500, 4);
    qtest_writel(qts, pin, 0x180c);
    expect(qts, SYS + 0xa4, 0x800000);
    qtest_writel(qts, 0x44000500, 4);
    expect(qts, SYS + 0xa4, 0);
    qtest_writel(qts, pin, 0x140c); /* Level reasserts after W1C. */
    qtest_writel(qts, 0x44000500, 4);
    expect(qts, 0x44000500, 4);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 0);
    qtest_writel(qts, pin, 0x340c);
    expect(qts, 0x44000500, 0);
    qtest_writel(qts, pin, 0x1808); /* Disabled input ignores edges. */
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 1);
    expect(qts, 0x44000500, 0);
    qtest_quit(qts);
}

static bool board_property(QTestState *qts, const char *name)
{
    QDict *response = qtest_qmp(qts, "{'execute':'qom-get', 'arguments':"
                               "{'path':'/machine', 'property':%s}}", name);
    bool value;

    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_bool(response, "return");
    qobject_unref(response);
    return value;
}

static void set_key(QTestState *qts, bool pressed)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set', 'arguments':"
                            "{'path':'/machine', 'property':'user-key-pressed',"
                            "'value':%i}}", pressed);
}

static void test_board_wiring(const void *board)
{
    QTestState *qts = start(board);
    bool aidk = !strcmp(board, "aidk_ai_toy");
    unsigned led = aidk ? 40 : !strcmp(board, "t5_board") ? 1 : 9;
    unsigned key = aidk ? 8 : !strcmp(board, "t5_board") ? 12 : 29;
    unsigned wrong_led = led == 1 ? 9 : 1;
    uint32_t pin = 0x44000400 + key * 4;
    QDict *error;

    g_assert_false(board_property(qts, "user-led-on"));
    g_assert_false(board_property(qts, "user-key-pressed"));
    /* Reset disables input sampling, including externally pulled-up pins. */
    expect(qts, pin, 0x28);
    qtest_writel(qts, pin, 0x2c);
    /* The AIDK open contact has no external pull-up in its schematic. */
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, aidk ? 0 : 1);
    qtest_writel(qts, 0x44000400 + wrong_led * 4, 0x86);
    g_assert_false(board_property(qts, "user-led-on"));
    qtest_writel(qts, 0x44000400 + led * 4, 0x86);
    g_assert_true(board_property(qts, "user-led-on"));
    qtest_writel(qts, 0x44000400 + led * 4, 0x8e); /* Output disabled. */
    g_assert_false(board_property(qts, "user-led-on"));
    qtest_writel(qts, 0x44000400 + led * 4, 0x86);
    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set', 'arguments':{'path':'/machine',"
        "'property':'user-led-on', 'value':false}}");
    qobject_unref(error);
    g_assert_true(board_property(qts, "user-led-on"));

    qtest_writel(qts, pin, 0x1c3c); /* Input, pull-up, falling-edge IRQ. */
    qtest_writel(qts, SYS + 0x84, 0x800000);
    set_key(qts, true);
    g_assert_true(board_property(qts, "user-key-pressed"));
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 0);
    expect(qts, 0x44000500, 1U << key);
    expect(qts, SYS + 0xa4, 0x800000);
    qtest_writel(qts, 0x44000500, 1U << key);
    expect(qts, SYS + 0xa4, 0);
    qtest_system_reset(qts);
    g_assert_true(board_property(qts, "user-key-pressed"));
    expect(qts, pin, 0x28);
    qtest_writel(qts, pin, 0x3c); /* Re-enable sampling while still held. */
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 0);
    g_assert_false(board_property(qts, "user-led-on"));
    expect(qts, SYS + 0xa4, 0);
    set_key(qts, false);
    qtest_writel(qts, pin, 0x2c);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, aidk ? 0 : 1);
    qtest_writel(qts, pin, 0x3c);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 1);
    qtest_quit(qts);
}

#define GPIO 0x44000400
#define GPIO_STATUS (GPIO + 0x100)
#define GPIO_IRQ55 (1U << 23)

/* AIDK V1.0 schematic: KEY1=P13, KEY2=P12, KEY3=P8. */
static const struct {
    unsigned pin;
    const char *property;
} aidk_keys[] = {
    { 13, "key1-pressed" },
    { 12, "key2-pressed" },
    { 8, "user-key-pressed" },
};

static void aidk_set_key(QTestState *qts, unsigned key, bool pressed)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set', 'arguments':"
                            "{'path':'/machine', 'property':%s, 'value':%i}}",
                            aidk_keys[key].property, pressed);
}

static void aidk_expect_keys(QTestState *qts, unsigned pressed, unsigned high)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        g_assert_cmpint(board_property(qts, aidk_keys[i].property), ==,
                        !!(pressed & (1U << i)));
        g_assert_cmpuint(qtest_readl(qts, GPIO + 4 * aidk_keys[i].pin) & 1,
                         ==, !!(high & (1U << i)));
    }
}

static void test_aidk_key_contacts(const void *board)
{
    static const struct {
        uint32_t config;
        bool released_high;
    } pulls[] = {
        { 0x3c, true },  /* Internal pull-up. */
        { 0x2c, false }, /* Internal pull-down. */
        { 0x0c, false }, /* Floating input has the model's low policy. */
        { 0x1c, false }, /* Pull selection without pull enable. */
    };
    QTestState *qts = start(board);

    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        expect(qts, GPIO + 4 * aidk_keys[i].pin, 0x28);
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x3c);
    }
    /* A separate externally driven input must survive every contact change. */
    qtest_writel(qts, GPIO + 4 * 2, 0x2c);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 1);
    aidk_expect_keys(qts, 0, 7);
    for (unsigned pressed = 0; pressed < 8; pressed++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
            aidk_set_key(qts, i, pressed & (1U << i));
        }
        /* Includes each key alone, every pair, and all three held together. */
        aidk_expect_keys(qts, pressed, pressed ^ 7);
        expect(qts, GPIO + 4 * 2, 0x2d);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        aidk_set_key(qts, i, false);
    }
    for (unsigned p = 0; p < G_N_ELEMENTS(pulls); p++) {
        unsigned high = pulls[p].released_high ? 7 : 0;

        for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
            qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, pulls[p].config);
        }
        aidk_expect_keys(qts, 0, high);
        for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
            aidk_set_key(qts, i, true);
            aidk_set_key(qts, i, true); /* Repeated writes are idempotent. */
            aidk_expect_keys(qts, 1U << i, high & ~(1U << i));
            aidk_set_key(qts, i, false);
            aidk_expect_keys(qts, 0, high);
            expect(qts, GPIO + 4 * 2, 0x2d);
        }
    }
    qtest_quit(qts);
}

static void test_aidk_leds(const void *board)
{
    static const struct {
        unsigned pin;
        const char *property, *path, *color;
    } leds[] = {
        { 40, "user-led-on", "/machine/user-led", "red" },
        { 41, "led2-on", "/machine/led2", "green" },
    };
    static const uint32_t disabled[] = { 0x8e, 0xc6, 0x3c, 0x84 };
    QTestState *qts = start(board);

    for (unsigned i = 0; i < G_N_ELEMENTS(leds); i++) {
        QDict *response = qtest_qmp_assert_success_ref(qts,
            "{'execute':'qom-get', 'arguments':"
            "{'path':%s, 'property':'color'}}", leds[i].path);

        g_assert_cmpstr(qdict_get_str(response, "return"), ==, leds[i].color);
        qobject_unref(response);
        g_assert_true(qtest_qom_get_bool(qts, leds[i].path,
                                        "gpio-active-high"));
        g_assert_false(board_property(qts, leds[i].property));
    }
    qtest_writel(qts, GPIO + 4 * 1, 0x86);
    qtest_writel(qts, GPIO + 4 * 9, 0x86);
    g_assert_false(board_property(qts, "user-led-on"));
    g_assert_false(board_property(qts, "led2-on"));
    for (unsigned on = 0; on < 4; on++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(leds); i++) {
            qtest_writel(qts, GPIO + 4 * leds[i].pin,
                         0x84 | ((on & (1U << i)) ? 2 : 0));
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(leds); i++) {
            g_assert_cmpint(board_property(qts, leds[i].property), ==,
                            !!(on & (1U << i)));
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(leds); i++) {
        QDict *response = qtest_qmp_assert_failure_ref(qts,
            "{'execute':'qom-set', 'arguments':"
            "{'path':'/machine', 'property':%s, 'value':false}}",
            leds[i].property);
        QDict *error = qdict_get_qdict(response, "error");

        g_assert_cmpstr(qdict_get_str(error, "class"), ==, "GenericError");
        g_assert_nonnull(strstr(qdict_get_str(error, "desc"), "not writable"));
        qobject_unref(response);
        g_assert_true(board_property(qts, leds[i].property));
        for (unsigned j = 0; j < G_N_ELEMENTS(disabled); j++) {
            qtest_writel(qts, GPIO + 4 * leds[i].pin, disabled[j]);
            g_assert_false(board_property(qts, leds[i].property));
            g_assert_true(board_property(qts, leds[i ^ 1].property));
            qtest_writel(qts, GPIO + 4 * leds[i].pin, 0x86);
            g_assert_true(board_property(qts, leds[i].property));
        }
    }
    qtest_quit(qts);
}

static void aidk_expect_gpio_irq(QTestState *qts, bool raised)
{
    for (unsigned core = 0; core < 3; core++) {
        expect(qts, SYS + 0xa4 + 8 * core, raised ? GPIO_IRQ55 : 0);
    }
}

static void test_aidk_key_irqs(const void *board)
{
    QTestState *qts = start(board);
    uint32_t status = 0;

    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x1c3c);
    }
    expect(qts, GPIO_STATUS, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        aidk_set_key(qts, i, true);
        status |= 1U << aidk_keys[i].pin;
        expect(qts, GPIO_STATUS, status);
        aidk_expect_gpio_irq(qts, false); /* CPU route masks start clear. */
    }
    for (unsigned core = 0; core < 3; core++) {
        qtest_writel(qts, SYS + 0x84 + 8 * core, GPIO_IRQ55);
    }
    aidk_expect_gpio_irq(qts, true);
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x0c3c);
        expect(qts, GPIO_STATUS, status); /* Masking retains each latch. */
        aidk_expect_gpio_irq(qts, i + 1 < G_N_ELEMENTS(aidk_keys));
    }
    qtest_writel(qts, GPIO + 4 * 12, 0x1c3c);
    aidk_expect_gpio_irq(qts, true);
    qtest_writel(qts, GPIO_STATUS, 1U << 13);
    status &= ~(1U << 13);
    expect(qts, GPIO_STATUS, status);
    aidk_expect_gpio_irq(qts, true); /* KEY2 still owns the asserted route. */
    qtest_writel(qts, GPIO_STATUS, 1U << 2);
    expect(qts, GPIO_STATUS, status);
    aidk_expect_gpio_irq(qts, true);
    qtest_writel(qts, GPIO_STATUS, 1U << 12);
    expect(qts, GPIO_STATUS, 1U << 8);
    aidk_expect_gpio_irq(qts, false); /* KEY3 is still latched but masked. */
    qtest_writel(qts, GPIO + 4 * 8, 0x1c3c);
    aidk_expect_gpio_irq(qts, true);
    qtest_writel(qts, GPIO + 4 * 8, 0x3c3c); /* Per-pin W1C. */
    expect(qts, GPIO_STATUS, 0);
    aidk_expect_gpio_irq(qts, false);
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x1c3c);
        aidk_set_key(qts, i, true); /* Already held: no second edge. */
        aidk_set_key(qts, i, false); /* Falling-edge mode ignores release. */
    }
    expect(qts, GPIO_STATUS, 0);
    aidk_expect_gpio_irq(qts, false);
    qtest_quit(qts);
}

static void test_aidk_held_contacts_reset(const void *board)
{
    QTestState *qts = start(board);
    uint32_t status = 0;

    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x1c3c);
        aidk_set_key(qts, i, true);
        status |= 1U << aidk_keys[i].pin;
    }
    qtest_writel(qts, SYS + 0x84, GPIO_IRQ55);
    expect(qts, GPIO_STATUS, status);
    expect(qts, SYS + 0xa4, GPIO_IRQ55);
    qtest_writel(qts, GPIO + 4 * 40, 0x86);
    qtest_writel(qts, GPIO + 4 * 41, 0x86);
    g_assert_true(board_property(qts, "user-led-on"));
    g_assert_true(board_property(qts, "led2-on"));
    qtest_system_reset(qts);
    aidk_expect_keys(qts, 7, 0);
    expect(qts, GPIO_STATUS, 0);
    aidk_expect_gpio_irq(qts, false);
    g_assert_false(board_property(qts, "user-led-on"));
    g_assert_false(board_property(qts, "led2-on"));
    for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
        expect(qts, GPIO + 4 * aidk_keys[i].pin, 0x28);
        qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x3c);
    }
    aidk_expect_keys(qts, 7, 0); /* All physical contacts remain grounded. */
    for (unsigned i = 0, pressed = 7; i < G_N_ELEMENTS(aidk_keys); i++) {
        aidk_set_key(qts, i, false);
        pressed &= ~(1U << i);
        aidk_expect_keys(qts, pressed, pressed ^ 7);
    }
    expect(qts, GPIO_STATUS, 0);
    aidk_expect_gpio_irq(qts, false);
    qtest_quit(qts);
}

static void test_aidk_cli_contacts(const void *board)
{
    for (unsigned pressed = 0; pressed < 8; pressed++) {
        QTestState *qts = qtest_initf(
            "-machine %s,key1-pressed=%s,key2-pressed=%s,user-key-pressed=%s "
            "-serial null", (const char *)board,
            pressed & 1 ? "on" : "off", pressed & 2 ? "on" : "off",
            pressed & 4 ? "on" : "off");

        for (unsigned reset = 0; reset < 2; reset++) {
            aidk_expect_keys(qts, pressed, 0); /* Input sampling is disabled. */
            for (unsigned i = 0; i < G_N_ELEMENTS(aidk_keys); i++) {
                expect(qts, GPIO + 4 * aidk_keys[i].pin, 0x28);
                qtest_writel(qts, GPIO + 4 * aidk_keys[i].pin, 0x3c);
            }
            aidk_expect_keys(qts, pressed, pressed ^ 7);
            if (!reset) {
                qtest_system_reset(qts);
            }
        }
        qtest_quit(qts);
    }
}

static void test_aidk_properties_absent(const void *board)
{
    static const char *const properties[] = {
        "key1-pressed", "key2-pressed", "led2-on",
    };
    QTestState *qts = start(board);

    for (unsigned i = 0; i < G_N_ELEMENTS(properties); i++) {
        for (unsigned set = 0; set < 2; set++) {
            QDict *response = set ? qtest_qmp_assert_failure_ref(qts,
                "{'execute':'qom-set', 'arguments':"
                "{'path':'/machine', 'property':%s, 'value':true}}",
                properties[i]) : qtest_qmp_assert_failure_ref(qts,
                "{'execute':'qom-get', 'arguments':"
                "{'path':'/machine', 'property':%s}}", properties[i]);
            QDict *error = qdict_get_qdict(response, "error");

            g_assert_cmpstr(qdict_get_str(error, "class"), ==, "GenericError");
            g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                                    properties[i]));
            g_assert_nonnull(strstr(qdict_get_str(error, "desc"), "not found"));
            qobject_unref(response);
        }
    }
    g_assert_false(board_property(qts, "user-key-pressed"));
    g_assert_false(board_property(qts, "user-led-on"));
    qtest_quit(qts);
}

static void test_gpio_external_release(const void *board)
{
    QTestState *qts = start(board);
    const uint32_t pin = 0x44000408;

    qtest_writel(qts, pin, 0x2c); /* Input with pull-down. */
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 1);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 1);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, -1);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 0);
    /* Released contact follows internal pull-up. */
    qtest_writel(qts, pin, 0x3c);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 1);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 0);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 0);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, -1);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 1);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, 2);
    qtest_set_irq_in(qts, "/machine/soc/aon", "gpio-in", 2, -2);
    g_assert_cmpuint(qtest_readl(qts, pin) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_analog(const void *board)
{
    QTestState *qts = start(board);

    qtest_writel(qts, SYS + 0x100, 0x12345678);
    expect(qts, SYS + 0xe8, 1);
    expect(qts, SYS + 0x100, 0);
    qtest_writel(qts, SYS + 0x100, 0xdeadbeef);
    qtest_clock_step(qts, 999);
    expect(qts, SYS + 0xe8, 1);
    qtest_clock_step(qts, 1);
    expect(qts, SYS + 0xe8, 0);
    expect(qts, 0x54010100, 0x12345678);
    qtest_writel(qts, SYS + 0x100, 0xa5);
    qtest_system_reset(qts);
    qtest_clock_step(qts, 1000);
    expect(qts, SYS + 0x100, 0);
    expect(qts, SYS + 0xe8, 0);
    qtest_quit(qts);
}

static void test_clock_monitor(const void *board)
{
    QTestState *qts = start(board);
    static const unsigned windows[] = {32, 64, 128, 1023};

    qtest_writel(qts, CKMN + 8, 1);
    qtest_writel(qts, SYS + 0x80, 0x200000);
    qtest_writel(qts, SYS + 0x88, 0x200000);
    for (unsigned i = 0; i < G_N_ELEMENTS(windows); i++) {
        qtest_writel(qts, CKMN + 0x14, 0);
        qtest_writel(qts, CKMN + 0x20, 7);
        qtest_writel(qts, CKMN + 0x10, windows[i]);
        qtest_writel(qts, CKMN + 0x14, 1);
        qtest_clock_step(qts, windows[i] * 31250 - 1);
        expect(qts, CKMN + 0x20, 0);
        qtest_clock_step(qts, 1);
        expect(qts, 0x548a0018, 26000000ULL * windows[i] / 32000);
        expect(qts, CKMN + 0x20, 1);
        expect(qts, SYS + 0xa0, 0);
        qtest_writel(qts, CKMN + 0x14, 3);
        expect(qts, SYS + 0xa0, 0x200000);
        expect(qts, SYS + 0xa8, 0x200000);
        qtest_writel(qts, SYS + 0x80, 0);
        expect(qts, SYS + 0xa0, 0);
        expect(qts, SYS + 0xa8, 0x200000);
        qtest_writel(qts, SYS + 0x80, 0x200000);
        qtest_writel(qts, CKMN + 0x20, 1);
        expect(qts, SYS + 0xa8, 0);
        qtest_clock_step(qts, 50000000);
        expect(qts, CKMN + 0x20, 0);
    }
    qtest_quit(qts);
}

static void test_clock_cancel(const void *board)
{
    QTestState *qts = start(board);

    qtest_writel(qts, CKMN + 8, 1);
    qtest_writel(qts, CKMN + 0x10, 64);
    qtest_writel(qts, CKMN + 0x14, 3);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, SYS + 0x114, 0x4000); /* Lose the ROSC source. */
    qtest_clock_step(qts, 5000000);
    expect(qts, CKMN + 0x20, 0);
    qtest_writel(qts, SYS + 0x114, 0);
    qtest_clock_step(qts, 5000000);
    expect(qts, CKMN + 0x20, 0); /* Restoring a source cannot fabricate done. */
    qtest_writel(qts, CKMN + 0x14, 0);
    qtest_writel(qts, CKMN + 0x14, 3);
    qtest_clock_step(qts, 2000000);
    expect(qts, CKMN + 0x20, 1);
    expect(qts, CKMN + 0x18, 52000);
    qtest_writel(qts, CKMN + 8, 0);
    qtest_writel(qts, CKMN + 0x10, 32);
    expect(qts, CKMN + 0x10, 0); /* Held reset. */
    qtest_writel(qts, CKMN + 8, 1);
    qtest_writel(qts, CKMN + 0x14, 3); /* Zero window. */
    qtest_clock_step(qts, 5000000);
    expect(qts, CKMN + 0x20, 0);
    qtest_quit(qts);
}

static void mailbox_setup(QTestState *qts)
{
    static const unsigned starts[] = {0, 2, 5};
    static const unsigned lengths[] = {2, 3, 3};

    qtest_writel(qts, MBOX + 8, 5);
    for (unsigned i = 0; i < 3; i++) {
        qtest_writel(qts, MBOX + 0x40 + i * 0x40, 0x100 | starts[i]);
        qtest_writel(qts, MBOX + 0x44 + i * 0x40, lengths[i] * 2 + 1);
        qtest_writel(qts, SYS + 0x84 + i * 8, 0x80000000);
    }
}

static void mailbox_send(QTestState *qts, unsigned source, unsigned dest,
                         uint32_t data)
{
    uint32_t base = MBOX + 0x40 + source * 0x40;

    qtest_writel(qts, base + 8, data);
    qtest_writel(qts, base + 12, 16);
    qtest_writel(qts, base + 16, dest);
}

static void mailbox_receive(QTestState *qts, unsigned dest, unsigned source,
                            uint32_t data)
{
    uint32_t base = MBOX + 0x40 + dest * 0x40;

    expect(qts, base + 20, source);
    expect(qts, base + 24, data);
    expect(qts, base + 28, 16);
}

static void test_mailbox(const void *board)
{
    QTestState *qts = start(board);

    mailbox_setup(qts);
    for (unsigned source = 0; source < 3; source++) {
        for (unsigned dest = 0; dest < 3; dest++) {
            mailbox_send(qts, source, dest, 0x28001000 + source);
            for (unsigned core = 0; core < 3; core++) {
                expect(qts, SYS + 0xa4 + core * 8,
                       core == dest ? 0x80000000 : 0);
            }
            mailbox_receive(qts, dest, source, 0x28001000 + source);
        }
    }
    for (unsigned i = 1; i <= 3; i++) {
        mailbox_send(qts, 0, 1, i);
    }
    mailbox_send(qts, 0, 1, 99); /* Full FIFO preserves the original three. */
    g_assert_true(qtest_readl(qts, MBOX + 0x40) & (1 << 18));
    mailbox_receive(qts, 1, 0, 1);
    mailbox_send(qts, 2, 1, 4); /* Wrap around. */
    mailbox_receive(qts, 1, 0, 2);
    mailbox_receive(qts, 1, 0, 3);
    mailbox_receive(qts, 1, 2, 4);
    expect(qts, MBOX + 0xa0, 2);
    qtest_writel(qts, MBOX + 0x40, 0x40100);
    g_assert_false(qtest_readl(qts, MBOX + 0x40) & (1 << 18));
    qtest_quit(qts);
}

static void test_mailbox_protection(const void *board)
{
    QTestState *qts = start(board);

    mailbox_setup(qts);
    mailbox_send(qts, 0, 1, 0x1234);
    qtest_writel(qts, MBOX + 8, 1); /* Enforce physical CPU0 bus identity. */
    expect(qts, MBOX + 0x94, 0xf);
    g_assert_true(qtest_readl(qts, MBOX + 0x80) & (1 << 17));
    expect(qts, MBOX + 0xa0, 4);
    qtest_writel(qts, MBOX + 0x90, 2);
    g_assert_true(qtest_readl(qts, MBOX + 0x80) & (1 << 16));
    expect(qts, MBOX + 0xe0, 2);
    qtest_writel(qts, MBOX + 8, 0);
    for (unsigned i = 0; i < 3; i++) {
        expect(qts, MBOX + 0x60 + i * 0x40, 2);
        expect(qts, SYS + 0xa4 + i * 8, 0);
    }
    qtest_quit(qts);
}

static void flash_op(QTestState *qts, unsigned op, unsigned address)
{
    qtest_writel(qts, FLASH + 0x54, op << 24 | address);
    qtest_writel(qts, FLASH + 0x10, 0x60000000);
    g_assert_true(qtest_readl(qts, FLASH + 0x10) & 0x80000000);
    qtest_clock_step(qts, 1000);
    g_assert_false(qtest_readl(qts, FLASH + 0x10) & 0x80000000);
}

static void flash_program(QTestState *qts, unsigned address, uint32_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(qts, FLASH + 0x14, value);
    }
    flash_op(qts, 12, address);
}

static void flash_expect(QTestState *qts, unsigned address, uint32_t value)
{
    flash_op(qts, 5, address);
    for (unsigned i = 0; i < 8; i++) {
        expect(qts, FLASH + 0x18, value);
    }
}

static void test_volatile_nor(const void *board)
{
    QTestState *qts = start(board);

    g_assert_false(qtest_qom_get_bool(qts, "/machine/soc", "diagnostic-xip"));
    /* No host file is needed for a present, process-local physical NOR. */
    qtest_writel(qts, FLASH + 8, 1);
    flash_op(qts, 20, 0);
    expect(qts, FLASH + 0x20, 0xc86517);
    flash_program(qts, 0x1000, 0x96969696);
    flash_expect(qts, 0x1000, 0x96969696);
    qtest_system_reset(qts);
    qtest_writel(qts, FLASH + 8, 1);
    flash_expect(qts, 0x1000, 0x96969696);
    flash_op(qts, 13, 0x1000);
    flash_expect(qts, 0x1000, 0xffffffff);
    flash_program(qts, 0x1000, 0);
    qtest_quit(qts);

    qts = start(board);
    qtest_writel(qts, FLASH + 8, 1);
    flash_expect(qts, 0x1000, 0xffffffff); /* New process, not persistence. */
    qtest_quit(qts);
}

static void expect_nor_link(QTestState *qts, const char *path, const char *prop)
{
    QDict *reply = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", path, prop);

    g_assert_true(qdict_haskey(reply, "return"));
    g_assert_cmpstr(qdict_get_str(reply, "return"), ==, "/machine/nor");
    qobject_unref(reply);
}

static void test_board_owned_nor(const void *board)
{
    QTestState *qts = start(board);
    QDict *error;

    expect_nor_link(qts, "/machine/soc", "flash-nor");
    expect_nor_link(qts, "/machine/soc/flashctrl", "nor");
    g_assert_true(qtest_qom_get_bool(qts, "/machine/nor", "realized"));
    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc',"
        "'property':'flash-nor','value':''}}");
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "after it was realized"));
    qobject_unref(error);
    error = qtest_qmp_assert_failure_ref(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/soc/flashctrl',"
        "'property':'nor','value':'/machine/soc/aon'}}");
    qobject_unref(error);
    expect_nor_link(qts, "/machine/soc", "flash-nor");
    qtest_writel(qts, FLASH + 8, 1);
    flash_op(qts, 20, 0);
    expect(qts, FLASH + 0x20, 0xc86517);
    flash_program(qts, 0x3000, 0x73737373);
    qtest_system_reset(qts);
    expect_nor_link(qts, "/machine/soc", "flash-nor");
    qtest_writel(qts, FLASH + 8, 1);
    flash_expect(qts, 0x3000, 0x73737373);
    qtest_quit(qts);
}

static void reject_start(const char *args)
{
    QTestState *qts = qtest_init_ext(NULL, args, NULL, false);

    qtest_set_expected_status(qts, 1);
    qtest_wait_qemu(qts);
    qtest_quit(qts);
}

static void test_spi_bus_wiring_rejection(const void *board)
{
    for (unsigned g = 0; g < 2; g++) {
        g_autofree char *wrong = g_strdup_printf(
            "-machine %s -device w25q32,bus=spi%u,cs=1",
            (const char *)board, g);
        g_autofree char *multiple = g_strdup_printf(
            "-machine %s -device w25q32,bus=spi%u,cs=0 "
            "-device w25q32,bus=spi%u,cs=0", (const char *)board, g, g);

        reject_start(wrong);
        reject_start(multiple);
    }
}

static void test_explicit_xip_mode(const void *board)
{
    static const char vector[] = {
        0, 0x10, 0, 0x28, 9, 0, 1, 2, (char)0xfe, (char)0xe7, 0, 0,
    };
    g_autofree char *dir = g_dir_make_tmp("bk7258-loader-XXXXXX", NULL);
    g_autofree char *kernel = NULL;
    g_autofree char *array = NULL;
    g_autofree char *args = NULL;
    g_autofree char *data = g_malloc(NOR_SIZE);
    QTestState *qts;

    g_assert_nonnull(dir);
    kernel = g_build_filename(dir, "logical.bin", NULL);
    array = g_build_filename(dir, "nor.bin", NULL);
    g_assert_true(g_file_set_contents(kernel, vector, sizeof(vector), NULL));
    memset(data, 0xff, NOR_SIZE);
    g_assert_true(g_file_set_contents(array, data, NOR_SIZE, NULL));
    qts = qtest_initf("-machine %s -serial null -kernel %s",
                      (const char *)board, kernel);
    g_assert_true(qtest_qom_get_bool(qts, "/machine/soc", "diagnostic-xip"));
    expect(qts, 0x02010000, 0x28001000);
    expect(qts, 0x02010004, 0x02010009);
    qtest_quit(qts);

    args = g_strdup_printf("-machine %s -kernel %s "
                           "-drive if=pflash,unit=0,format=raw,file=%s",
                           (const char *)board, kernel, array);
    reject_start(args); /* A logical kernel must not shadow physical NOR. */
    g_clear_pointer(&args, g_free);
    args = g_strdup_printf("-machine %s "
                           "-global bk7258-soc.xip-size=4096",
                           (const char *)board);
    reject_start(args); /* Physical geometry is independent of host files. */
    g_assert_cmpint(unlink(kernel), ==, 0);
    g_assert_cmpint(unlink(array), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static QTestState *start_nor(const void *board, const char *array,
                            const char *status, bool readonly)
{
    QTestState *qts = qtest_initf(
        "-machine %s -serial null "
        "-drive if=pflash,unit=0,format=raw,file=%s,readonly=%s "
        "-drive if=pflash,unit=1,format=raw,file=%s",
        (const char *)board, array, readonly ? "on" : "off", status);

    qtest_writel(qts, FLASH + 8, 1);
    return qts;
}

static void test_nor(const void *board)
{
    g_autofree char *dir = g_dir_make_tmp("bk7258-qtest-XXXXXX", NULL);
    g_autofree char *array = NULL;
    g_autofree char *status = NULL;
    g_autofree char *data = g_malloc(NOR_SIZE);
    char sr[512] = {0, 0, 0x20};
    QTestState *qts;
    QDict *response;
    g_autofree char *committed = NULL;
    size_t committed_size;

    g_assert_nonnull(dir);
    array = g_build_filename(dir, "nor.bin", NULL);
    status = g_build_filename(dir, "status.bin", NULL);
    memset(data, 0xff, NOR_SIZE);
    g_assert_true(g_file_set_contents(array, data, NOR_SIZE, NULL));
    g_assert_true(g_file_set_contents(status, sr, sizeof(sr), NULL));
    qts = start_nor(board, array, status, false);
    expect(qts, FLASH + 0x20, 0);
    flash_op(qts, 20, 0);
    expect(qts, FLASH + 0x20, 0xc86517);
    flash_program(qts, 0x1000, 0x5a5a5a5a);
    flash_program(qts, 0x1000, 0xf0f0f0f0);
    flash_expect(qts, 0x1000, 0x50505050);
    qtest_writel(qts, FLASH + 0x28, 0x0c000000 | (0x0238 << 10));
    flash_op(qts, 7, 0); /* Lower 4 MiB protected, QE enabled. */
    flash_op(qts, 13, 0x1000);
    flash_expect(qts, 0x1000, 0x50505050);
    flash_program(qts, 0x500000, 0x12121212);
    flash_expect(qts, 0x500000, 0x12121212);
    qtest_quit(qts);

    qts = start_nor(board, array, status, false);
    flash_expect(qts, 0x500000, 0x12121212);
    flash_op(qts, 3, 0);
    expect(qts, FLASH + 0x24, 0x38);
    qtest_writel(qts, FLASH + 0x28, 0x0c000000 | (0x0200 << 10));
    flash_op(qts, 7, 0);
    flash_op(qts, 13, 0x1000);
    flash_expect(qts, 0x1000, 0xffffffff);
    flash_program(qts, 0, 0);
    qtest_readl(qts, 0x02000000); /* Data without a valid CRC must fail. */
    g_assert_cmpuint((qtest_readl(qts, FLASH + 0x24) >> 8) & 0xff, ==, 1);
    /* CRC16(poly 0x8005, init 0xffff) of 32 zero bytes is 0x8029. */
    qtest_writel(qts, FLASH + 0x14, 0xffff2980);
    for (unsigned i = 1; i < 8; i++) {
        qtest_writel(qts, FLASH + 0x14, 0xffffffff);
    }
    flash_op(qts, 12, 32);
    expect(qts, 0x02000000, 0);
    expect(qts, 0x1200001c, 0);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writel(qts, FLASH + 0x14, 0xa5a5a5a5);
    }
    qtest_writel(qts, FLASH + 0x54, 0x0c002000);
    qtest_writel(qts, FLASH + 0x10, 0x60000000);
    qtest_clock_step(qts, 999);
    qtest_writel(qts, FLASH + 8, 0); /* Reset cancels uncommitted mutation. */
    qtest_clock_step(qts, 1000);
    qtest_writel(qts, FLASH + 8, 1);
    flash_expect(qts, 0x2000, 0xffffffff);
    qtest_quit(qts);

    qts = start_nor(board, array, status, true);
    flash_program(qts, 0x2000, 0);
    response = qtest_qmp_assert_success_ref(qts, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(response, "status"), ==, "io-error");
    qobject_unref(response);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(array, &committed, &committed_size, NULL));
    g_assert_cmpuint(committed_size, ==, NOR_SIZE);
    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex((uint8_t)committed[0x2000 + i], ==, 0xff);
    }
    g_assert_cmpint(unlink(array), ==, 0);
    g_assert_cmpint(unlink(status), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

#define DMA_FIN (1U << 18)
#define DMA_HALF (1U << 19)
#define DMA_ERR (1U << 20)
#define DMA_FLAGS (DMA_FIN | DMA_HALF | DMA_ERR)

static uint32_t dma_channel(unsigned unit, unsigned channel)
{
    return 0x45020040 + unit * 0x10000 + channel * 0x40;
}

static void dma_start(QTestState *qts, uint32_t channel, uint32_t source,
                      uint32_t destination, unsigned bytes, unsigned config)
{
    qtest_writel(qts, channel + 4, destination);
    qtest_writel(qts, channel + 8, source);
    qtest_writel(qts, channel, ((bytes - 1) << 16) | config | 1);
}

static void test_dma_widths_aliases(const void *board)
{
    QTestState *qts = start(board);
    uint8_t source[32], actual[32], expected[32];
    static const uint32_t alias[] = {
        0x08000000, 0x18000000, 0x28000000, 0x38000000,
    };

    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = 0x91 ^ (i * 17);
    }
    for (unsigned unit = 0; unit < 2; unit++) {
        qtest_writel(qts, 0x45020008 + unit * 0x10000, 1);
        for (unsigned ch = 0; ch < 8; ch++) {
            uint32_t c = dma_channel(unit, ch);
            uint32_t src = alias[ch % 4] + 0x1000;
            uint32_t dst = alias[(ch + 1) % 4] + 0x2000;

            for (unsigned w = 0; w < 3; w++) {
                unsigned width = 1U << w;

                for (unsigned inc = 0; inc < 4; inc++) {
                    memset(actual, 0xcc, sizeof(actual));
                    memset(expected, 0xcc, sizeof(expected));
                    for (unsigned n = 0; n < sizeof(source); n += width) {
                        memcpy(expected + (inc & 2 ? n : 0),
                               source + (inc & 1 ? n : 0), width);
                    }
                    qtest_memwrite(qts, src, source, sizeof(source));
                    qtest_memwrite(qts, dst, actual, sizeof(actual));
                    dma_start(qts, c, src, dst, sizeof(source),
                              (w << 4) | (w << 6) | (inc << 8));
                    expect(qts, c + 0x30, sizeof(source));
                    qtest_clock_step(qts, 10000);
                    qtest_memread(qts, dst, actual, sizeof(actual));
                    g_assert_cmpmem(actual, sizeof(actual), expected,
                                    sizeof(expected));
                    expect(qts, c + 0x28,
                           src + (inc & 1 ? sizeof(source) : 0));
                    expect(qts, c + 0x2c,
                           dst + (inc & 2 ? sizeof(source) : 0));
                    expect(qts, c + 0x30, 0x110c0000);
                    qtest_writel(qts, c + 0x30, DMA_FLAGS);
                    expect(qts, c + 0x30, 0);
                }
            }
        }
    }
    qtest_quit(qts);
}

static void test_dma_half_mask_routes(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = 0x45020000 + unit * 0x10000;
        uint32_t c = dma_channel(unit, 7);
        unsigned bank = unit ? 4 : 0;
        uint32_t mask = unit ? 1U << 25 : 1;

        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, 0x28001000, 0x12345678);
        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core + bank, mask);
        }
        dma_start(qts, c, 0x28001000, 0x38002000, 4, 0x300);
        qtest_clock_step(qts, 77);
        expect(qts, c + 0x30, 3);
        qtest_clock_step(qts, 77);
        expect(qts, c + 0x30, 0x10080002);
        expect(qts, b + 0x1c, 0); /* Locally masked, status still latches. */
        qtest_writel(qts, c, 0x00030305); /* Enable HALF without rearm. */
        expect(qts, b + 0x1c, 0x80);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core + bank, mask);
        }
        /* W0 must not clear W1C flags; alias write acknowledges HALF. */
        qtest_writel(qts, c + 0x30, 0);
        expect(qts, b + 0x1c, 0x80);
        qtest_writel(qts, c + 0x10000030, DMA_HALF);
        expect(qts, b + 0x1c, 0);
        qtest_clock_step(qts, 154);
        expect(qts, c + 0x30, 0x01040000);
        expect(qts, b + 0x1c, 0); /* FINISH is independently masked. */
        qtest_writel(qts, c, 0x00030302);
        expect(qts, b + 0x1c, 0x80);
        expect(qts, b + 0x18, 0); /* This slice has no secure channel. */
        expect(qts, 0x28002000, 0x12345678);
        qtest_writel(qts, c + 0x30, DMA_FIN);
        for (unsigned core = 0; core < 3; core++) {
            expect(qts, SYS + 0xa0 + 8 * core + bank, 0);
        }
    }
    qtest_quit(qts);
}

static void test_dma_fault_progress(const void *board)
{
    QTestState *qts = start(board);
    static const uint32_t inaccessible[] = {
        0x20000000, 0x30000000, 0x00000000, 0x10000000, /* Private TCM. */
        0x4482001c, 0x44010080, 0x02000000, 0x60000000, /* Not DMA RAM. */
        0xffffffff,
    };

    qtest_writel(qts, 0x2809fffc, 0x87654321);
    qtest_writel(qts, 0x28001000, 0xdeadbeef);
    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = 0x45020000 + unit * 0x10000;
        uint32_t c = dma_channel(unit, 0);
        unsigned bank = unit ? 4 : 0;
        uint32_t mask = unit ? 1U << 25 : 1;

        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, SYS + 0x80 + bank, mask);
        qtest_writel(qts, c + 0x1c, 1U << 22);
        qtest_writel(qts, 0x28002004, 0xdeadbeef);
        dma_start(qts, c, 0x1809fffc, 0x38002000, 8, 0x3a0);
        qtest_clock_step(qts, 154);
        expect(qts, 0x28002000, 0x87654321);
        expect(qts, 0x28002004, 0xdeadbeef);
        expect(qts, c + 0x30, 0x10180004); /* HALF and ERR, never FINISH. */
        expect(qts, c + 0x28, 0x180a0000);
        expect(qts, c + 0x2c, 0x38002004);
        expect(qts, SYS + 0xa0 + bank, mask);
        qtest_writel(qts, c + 0x1c, 0); /* Mask without clearing error. */
        expect(qts, SYS + 0xa0 + bank, 0);
        qtest_writel(qts, c + 0x30, DMA_FLAGS);
        for (unsigned i = 0; i < G_N_ELEMENTS(inaccessible); i++) {
            for (unsigned write = 0; write < 2; write++) {
                dma_start(qts, c, write ? 0x28001000 : inaccessible[i],
                          write ? inaccessible[i] : 0x28002000, 1, 0);
                qtest_clock_step(qts, 77);
                expect(qts, c + 0x30, DMA_ERR | 1);
                g_assert_cmphex(qtest_readl(qts, c) & 1, ==, 0);
                expect(qts, 0x28001000, 0xdeadbeef);
                qtest_writel(qts, c + 0x30, DMA_ERR);
            }
        }
        expect(qts, SYS + 0x80 + bank, mask); /* No MMIO side effects. */
    }
    qtest_quit(qts);
}

static void test_dma_schedule_cancel(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = 0x45020000 + unit * 0x10000;

        qtest_writel(qts, b + 8, 1);
        for (unsigned ch = 0; ch < 8; ch++) {
            uint32_t c = dma_channel(unit, ch);

            qtest_writel(qts, 0x28001000 + ch * 4, 0xcafef000 | ch);
            qtest_writel(qts, 0x28002000 + ch * 4, 0);
            dma_start(qts, c, 0x28001000 + ch * 4,
                      0x28002000 + ch * 4, 4, 0xa0);
        }
        for (unsigned ch = 0; ch < 8; ch++) {
            qtest_clock_step(qts, 77);
            expect(qts, 0x28002000 + ch * 4, 0xcafef000 | ch);
            if (ch < 7) {
                expect(qts, 0x28002004 + ch * 4, 0);
            }
        }
        qtest_writel(qts, b + 8, 0);
        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, 0x28002000, 0);
        dma_start(qts, b + 0x40, 0x28001000, 0x28002000, 4, 0x3a0);
        qtest_clock_step(qts, 76);
        qtest_writel(qts, b + 0x40, 0x000303a2); /* Disable cancels beat. */
        qtest_clock_step(qts, 1000);
        expect(qts, 0x28002000, 0);
        expect(qts, b + 0x70, 4);
        dma_start(qts, b + 0x40, 0x28001000, 0x28002000, 4, 0x3a0);
        qtest_clock_step(qts, 76);
        qtest_writel(qts, b + 8, 0); /* Global reset cancels too. */
        qtest_clock_step(qts, 1000);
        expect(qts, 0x28002000, 0);
        expect(qts, b + 0x70, 0);
        qtest_writel(qts, b + 8, 1);
        dma_start(qts, b + 0x40, 0x28001000, 0x28002000, 4, 0x3a0);
        qtest_system_reset(qts);
        qtest_clock_step(qts, 1000);
        expect(qts, 0x28002000, 0);
        expect(qts, b + 8, 0);
    }
    qtest_quit(qts);
}

static void test_dma_max_length_rearm(const void *board)
{
    QTestState *qts = start(board);
    g_autofree uint8_t *source = g_malloc(65536);
    g_autofree uint8_t *actual = g_malloc0(65536);

    for (unsigned n = 0; n < 65536; n++) {
        source[n] = (n >> 8) ^ n ^ 0x5a;
    }
    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = 0x45020000 + unit * 0x10000, c = b + 0x40;
        unsigned config = unit ? 0x300 : 0x3a0;

        qtest_writel(qts, b + 8, 1);
        qtest_memwrite(qts, 0x28010000, source, 65536);
        memset(actual, 0, 65536);
        qtest_memwrite(qts, 0x28020000, actual, 65536);
        dma_start(qts, c, 0x18010000, 0x08020000, 65536, config);
        expect(qts, c + 0x30, 65536);
        qtest_clock_step(qts, 100000000); /* Bounded catch-up, not a loop. */
        expect(qts, c + 0x30, 0x110c0000);
        qtest_memread(qts, 0x38020000, actual, 65536);
        g_assert_cmpmem(actual, 65536, source, 65536);
        qtest_writel(qts, c, 0xffff0001 | config);
        g_assert_cmphex(qtest_readl(qts, c) & 1, ==, 0);
        qtest_writel(qts, c + 0x30, DMA_FIN);
        qtest_writel(qts, c, 0xffff0001 | config);
        g_assert_cmphex(qtest_readl(qts, c) & 1, ==, 0);
        qtest_writel(qts, c + 0x30, DMA_HALF);
        dma_start(qts, c, 0x28010000, 0x28020000, 1, 0);
        qtest_clock_step(qts, 77);
        expect(qts, c + 0x30, 0x110c0000); /* One-byte HALF and FINISH. */
        qtest_writel(qts, b + 8, 0);
    }
    qtest_quit(qts);
}

static void test_dma_unsupported_modes(const void *board)
{
    QTestState *qts = start(board);
    static const uint32_t unsupported[] = {
        8, 0x10, 0x30, 0x400, 0x800, 0x1000, 0x8000,
    };

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = 0x45020000 + unit * 0x10000, c = b + 0x40;

        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, c + 4, 0x28002000);
        qtest_writel(qts, c + 8, 0x28001000);
        for (unsigned i = 0; i < G_N_ELEMENTS(unsupported); i++) {
            qtest_writel(qts, c, 0x00030001 | unsupported[i]);
            expect(qts, c, 0);
        }
        qtest_writel(qts, c + 0x1c, 1); /* No fabricated peripheral ready. */
        qtest_writel(qts, c, 0x00030001);
        expect(qts, c, 0);
        qtest_writel(qts, c + 0x1c, 0);
        qtest_writel(qts, c + 4, 0x28002001); /* Unaligned word. */
        qtest_writel(qts, c, 0x000303a1);
        expect(qts, c, 0);
        qtest_writel(qts, b + 0x10, 1);
        qtest_writel(qts, b + 0x14, 1);
        qtest_writel(qts, b + 0x28, 1);
        expect(qts, b + 0x10, 0);
        expect(qts, b + 0x14, 0);
        expect(qts, b + 0x28, 0);
        qtest_clock_step(qts, 10000);
        expect(qts, c + 0x30, 0);
        expect(qts, b + 0x1c, 0);
    }
    qtest_quit(qts);
}

static uint32_t pwm_base(unsigned unit)
{
    return 0x458a0000 + unit * 0x50000;
}

static void pwm_setup(QTestState *qts, unsigned unit)
{
    qtest_writel(qts, SYS + 0x30, (1U << 3) | (1U << 12));
    qtest_writel(qts, SYS + 0x20, (1U << 18) | (1U << 19));
    qtest_writel(qts, pwm_base(unit) + 8, 0);
    qtest_writel(qts, pwm_base(unit) + 8, 1);
}

static void test_pwm_counters_compares_routes(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = pwm_base(unit), bank = unit ? 4 : 0;
        uint32_t irq = 1U << (unit ? 11 : 5);

        for (unsigned core = 0; core < 3; core++) {
            qtest_writel(qts, SYS + 0x80 + 8 * core + bank, irq);
        }
        for (unsigned timer = 0; timer < 3; timer++) {
            uint32_t flags = (7U << (3 * timer)) | (1U << (9 + timer));

            pwm_setup(qts, unit);
            qtest_writel(qts, b + 0x3c + 4 * timer, 51);
            for (unsigned n = 0; n < 3; n++) {
                qtest_writel(qts, b + 0x54 + 12 * timer + 4 * n,
                             13 * (n + 1));
            }
            qtest_writel(qts, b + 0x10, 1U << (2 - timer));
            for (unsigned n = 0; n < 3; n++) {
                qtest_clock_step(qts, 500);
                expect(qts, b + 0x2c + 4 * timer, 13 * (n + 1));
                expect(qts, b + 0x20, ((1U << (n + 1)) - 1) << (3 * timer));
            }
            qtest_clock_step(qts, 500);
            expect(qts, b + 0x2c + 4 * timer, 0);
            expect(qts, b + 0x20, flags);
            expect(qts, SYS + 0xa0 + bank, 0); /* Locally masked. */
            qtest_writel(qts, b + 0x1c, flags);
            for (unsigned core = 0; core < 3; core++) {
                expect(qts, SYS + 0xa0 + 8 * core + bank, irq);
            }
            qtest_writel(qts, b + 0x20, 0); /* W0 is not W1C. */
            expect(qts, b + 0x20, flags);
            qtest_writel(qts, b + 0x10000020, 7U << (3 * timer));
            expect(qts, SYS + 0xa0 + bank, irq);
            qtest_writel(qts, b + 0x20, 1U << (9 + timer));
            expect(qts, SYS + 0xa0 + bank, 0);
            qtest_writel(qts, b + 0x10, 0);
        }
    }
    qtest_quit(qts);
}

static void test_pwm_dividers_gates(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = pwm_base(unit), gate = 1U << (unit ? 12 : 3);
        g_autofree char *path = g_strdup_printf("/machine/soc/pwm[%u]/pclk",
                                                unit);

        pwm_setup(qts, unit);
        expect_clock(qts, path, 26000000);
        qtest_writel(qts, b + 0x38, 0xff7f00); /* /1, /128, /256. */
        for (unsigned n = 0; n < 3; n++) {
            qtest_writel(qts, b + 0x3c + 4 * n, UINT32_MAX);
        }
        qtest_writel(qts, b + 0x10, 7);
        qtest_clock_step(qts, 128000);
        expect(qts, b + 0x2c, 3328);
        expect(qts, b + 0x30, 26);
        expect(qts, b + 0x34, 13);
        qtest_writel(qts, SYS + 0x30, 0);
        expect_clock(qts, path, 0);
        qtest_clock_step(qts, 1000000000);
        expect(qts, b + 0x2c, 3328);
        /* Unconnected CLK32 is not guessed. */
        qtest_writel(qts, SYS + 0x20, 0);
        qtest_writel(qts, SYS + 0x30, gate);
        expect_clock(qts, path, 0);
        qtest_clock_step(qts, 1000000);
        expect(qts, b + 0x2c, 3328);
        qtest_writel(qts, SYS + 0x20, 1U << (18 + unit));
        qtest_clock_step(qts, 128000);
        expect(qts, b + 0x2c, 6656);
        expect(qts, b + 0x30, 52);
        expect(qts, b + 0x34, 26);
        qtest_writel(qts, b + 0x10, 0);
        qtest_clock_step(qts, 1000000);
        expect(qts, b + 0x2c, 6656); /* Disable retains the counter. */
        qtest_system_reset(qts);
        expect_clock(qts, path, 0);
        expect(qts, b + 0x2c, 0);
    }
    qtest_quit(qts);
}

static void test_pwm_preload_update_cancel(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = pwm_base(unit);

        pwm_setup(qts, unit);
        qtest_writel(qts, b + 0x10, 0x120); /* ARR/CCR preload for counter1. */
        qtest_writel(qts, b + 0x3c, 51);
        qtest_writel(qts, b + 0x54, 13);
        expect(qts, b + 0x7c, 0);
        expect(qts, b + 0x94, 0);
        qtest_writel(qts, b + 0x24, 0x200);
        qtest_clock_step(qts, 1);
        expect(qts, b + 0x24, 0x200);
        qtest_clock_step(qts, 38);
        expect(qts, b + 0x24, 0);
        expect(qts, b + 0x20, 0x200);
        expect(qts, b + 0x7c, 51);
        expect(qts, b + 0x94, 13);
        qtest_writel(qts, b + 0x20, 0xfff);
        qtest_writel(qts, b + 0x10, 0x124);
        qtest_clock_step(qts, 500);
        expect(qts, b + 0x20, 1);
        qtest_writel(qts, b + 0x54, 15);
        qtest_writel(qts, b + 0x3c, 77);
        expect(qts, b + 0x7c, 51);
        expect(qts, b + 0x94, 13);
        qtest_clock_step(qts, 1500);
        expect(qts, b + 0x7c, 77);
        expect(qts, b + 0x94, 15);
        expect(qts, b + 0x2c, 0);
        /* Stop; URS excludes UG IRQ. */
        qtest_writel(qts, b + 0x10, 0x10000120);
        qtest_writel(qts, b + 0x20, 0xfff);
        qtest_writel(qts, b + 0x24, 0x200);
        qtest_clock_step(qts, 39);
        expect(qts, b + 0x20, 0);
        qtest_writel(qts, SYS + 0x30, 0);
        qtest_writel(qts, b + 0x3c, 103);
        qtest_writel(qts, b + 0x24, 0x200);
        qtest_clock_step(qts, 100000);
        expect(qts, b + 0x24, 0x200);
        expect(qts, b + 0x7c, 77);
        qtest_writel(qts, b + 8, 0);
        qtest_writel(qts, SYS + 0x30, (1U << 3) | (1U << 12));
        qtest_clock_step(qts, 100000);
        expect(qts, b + 0x24, 0);
        expect(qts, b + 0x7c, 0);
        expect(qts, b + 0x20, 0);
        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, b + 0x24, 0xe00);
        qtest_system_reset(qts);
        qtest_clock_step(qts, 100000);
        expect(qts, b + 0x24, 0);
        expect(qts, b + 0x20, 0);
    }
    qtest_quit(qts);
}

static void test_pwm_wrap_and_rejection(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = pwm_base(unit);

        pwm_setup(qts, unit);
        qtest_writel(qts, b + 0x3c, UINT32_MAX);
        qtest_writel(qts, b + 0x54, 2);
        qtest_writel(qts, b + 0x10, 4);
        qtest_clock_step(qts, 165191049847LL); /* 2^32 XTAL cycles. */
        expect(qts, b + 0x2c, 0);
        expect(qts, b + 0x20, 0x201);
        qtest_writel(qts, b + 0x10, 0);
        qtest_writel(qts, b + 8, 0);
        qtest_writel(qts, b + 8, 1);
        qtest_writel(qts, b + 0x3c, 25);
        qtest_writel(qts, b + 0x54, 13);
        qtest_writel(qts, b + 0x10, 4);
        qtest_clock_step(qts, 100000000000LL); /* 100M periods, coalesced. */
        expect(qts, b + 0x20, 0x201);
        /* Rejected active config does not change an existing valid compare. */
        qtest_writel(qts, b + 0x54, 1);
        expect(qts, b + 0x54, 13);
        qtest_writel(qts, b + 0x54, 26); /* ARR+1 is outside this slice. */
        expect(qts, b + 0x54, 13);
        qtest_writel(qts, b + 0x10, 0x80000004); /* Down-counting. */
        expect(qts, b + 0x10, 4);
        qtest_writel(qts, b + 0x28, 0x00201000); /* Pad/waveform mode. */
        expect(qts, b + 0x28, 0);
        qtest_writel(qts, b + 0x48, 1); /* Repetition counter. */
        expect(qts, b + 0x48, 0);
        qtest_writel(qts, b + 0x78, 1); /* Deadtime. */
        expect(qts, b + 0x78, 0);
        qtest_writel(qts, b + 0x10, 0);
    }
    qtest_quit(qts);
}

static void test_pwm_mixed_preload_atomic(const void *board)
{
    QTestState *qts = start(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = pwm_base(unit);

        for (unsigned timer = 0; timer < 3; timer++) {
            uint32_t arpe = 1U << (5 - timer), ocpe = 1U << (8 - timer);
            uint32_t enable = 1U << (2 - timer);
            uint32_t arr = b + 0x3c + 4 * timer;
            uint32_t ccr = b + 0x54 + 12 * timer;
            uint32_t arr_shadow = b + 0x7c + 4 * timer;
            uint32_t ccr_shadow = b + 0x94 + 12 * timer;

            for (unsigned keep_arr = 0; keep_arr < 2; keep_arr++) {
                uint32_t old_arr = keep_arr ? 9 : 25;
                uint32_t old_ccr = keep_arr ? 2 : 13;
                uint32_t new_arr = keep_arr ? 25 : 9;
                uint32_t new_ccr = keep_arr ? 13 : 2;

                pwm_setup(qts, unit);
                qtest_writel(qts, b + 0x10, arpe | ocpe);
                qtest_writel(qts, arr, old_arr);
                qtest_writel(qts, ccr, old_ccr);
                qtest_writel(qts, b + 0x24, 1U << (9 + timer));
                qtest_clock_step(qts, 39);
                expect(qts, arr_shadow, old_arr);
                expect(qts, ccr_shadow, old_ccr);
                qtest_writel(qts, b + 0x20, 0xfff);
                qtest_writel(qts, arr, new_arr);
                qtest_writel(qts, ccr, new_ccr);
                qtest_writel(qts, b + 0x10,
                             enable | (keep_arr ? arpe : ocpe));
                expect(qts, b + 0x10, arpe | ocpe);
                expect(qts, arr, new_arr);
                expect(qts, ccr, new_ccr);
                expect(qts, arr_shadow, old_arr);
                expect(qts, ccr_shadow, old_ccr);
                qtest_clock_step(qts, 1000000);
                expect(qts, b + 0x2c + 4 * timer, 0);
                expect(qts, b + 0x20, 0);
                /* The unchanged staged pair can still be enabled coherently. */
                qtest_writel(qts, b + 0x10, enable);
                expect(qts, arr_shadow, new_arr);
                expect(qts, ccr_shadow, new_ccr);
                qtest_clock_step(qts, 1000);
                expect(qts, b + 0x20,
                       (1U << (3 * timer)) | (1U << (9 + timer)));
                expect(qts, b + 0x2c + 4 * timer, keep_arr ? 0 : 6);
            }
        }
    }
    qtest_quit(qts);
}

static void test_watchdog_deadline_horizon(const void *board)
{
    QTestState *qts = start(board);
    int64_t now = qtest_clock_step(qts, 1);

    qtest_clock_step(qts, INT64_MAX - now - 1000000);
    qtest_writel(qts, SYS + 0x80, 0x20);
    aon_wdt(qts, 2); /* Future expiry is beyond representable virtual time. */
    qtest_clock_step(qts, 999999);
    expect(qts, SYS + 0x80, 0x20); /* An overflow must not cause early reset. */
    expect(qts, 0x44000600, 2);
    aon_wdt(qts, 0);
    qtest_quit(qts);
}

static void test_uart_clock_budget(const void *board)
{
    int fd;
    g_autofree char *args = g_strdup_printf("-machine %s", (const char *)board);
    QTestState *qts = qtest_init_with_serial(args, &fd);

    uart_rx_setup(qts);
    qtest_writel(qts, 0x44820010, (225U << 8) | 0x1b);
    for (unsigned partial = 0; partial < 2; partial++) {
        unsigned elapsed = partial ? 77 : 0;

        g_assert_cmpint(send(fd, "A", 1, 0), ==, 1);
        uart_wait_rx(qts);
        if (elapsed) {
            qtest_clock_step(qts, elapsed);
        }
        for (unsigned n = 0; n < (partial ? 1 : 64); n++) {
            qtest_writel(qts, SYS + 0x30, 0);
            qtest_writel(qts, SYS + 0x30, 1U << 2);
        }
        /* 7232 cycles at 26 MHz; neither pause case may add a source cycle. */
        qtest_clock_step(qts, 278154 - elapsed - 1);
        expect(qts, 0x44820024, 0);
        qtest_clock_step(qts, 1);
        expect(qts, 0x44820024, 64);
        expect(qts, 0x4482001c, 'A' << 8);
        qtest_writel(qts, 0x44820024, 64);
    }
    close(fd);
    qtest_quit(qts);
}

static void test_spi_clock_budget(const void *board)
{
    QTestState *qts = start_spi(board);
    const uint8_t status[] = { 0x05, 0 };
    uint8_t received[2];

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = spi_base[unit];

        spi_setup(qts, unit);
        /* Divider1, interval0: 16 source cycles per byte, rounded to 616 ns. */
        qtest_writel(qts, b + 0x10, 0x40c00100);
        for (unsigned partial = 0; partial < 2; partial++) {
            unsigned elapsed = partial ? 385 : 0;

            qtest_writel(qts, b + 0x14, 0);
            qtest_writel(qts, b + 0x18, 0x37f00);
            qtest_writel(qts, b + 0x1c, partial ? 0x04 : 0x06);
            qtest_writel(qts, b + 0x14, 0x105);
            if (elapsed) {
                qtest_clock_step(qts, elapsed);
            }
            for (unsigned n = 0; n < (partial ? 1 : 64); n++) {
                qtest_writel(qts, SYS + 0x30, 0);
                qtest_writel(qts, SYS + 0x30, (1U << 1) | (1U << 9));
            }
            qtest_clock_step(qts, 616 - elapsed - 1);
            g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x2000, ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readl(qts, b + 0x18) & 0x7800, ==, 0x2000);
            spi_xfer(qts, unit, status, sizeof(status), received);
            g_assert_cmphex(received[1] & 2, ==, partial ? 0 : 2);
        }
    }
    qtest_quit(qts);
}

static void test_i2c_clock_budget(const void *board)
{
    QTestState *qts = start_i2c(board);

    for (unsigned unit = 0; unit < 2; unit++) {
        uint32_t b = i2c_base[unit];

        for (unsigned partial = 0; partial < 2; partial++) {
            unsigned elapsed = partial ? 39 : 0;

            i2c_setup(qts, unit);
            qtest_writel(qts, b + 0x10, 0xcc000000 | (21U << 6));
            qtest_writel(qts, b + 0x18, 0xa0);
            qtest_writel(qts, b + 0x14, 0x400);
            if (elapsed) {
                qtest_clock_step(qts, elapsed);
            }
            for (unsigned n = 0; n < (partial ? 1 : 64); n++) {
                qtest_writel(qts, SYS + 0x30, 0);
                qtest_writel(qts, SYS + 0x30, (1U << 0) | (1U << 8));
            }
            /* 648 source cycles for this address operation at 26 MHz. */
            qtest_clock_step(qts, 24924 - elapsed - 1);
            g_assert_cmphex(qtest_readl(qts, b + 0x14) & 1, ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readl(qts, b + 0x14) & 0x101, ==, 0x101);
            i2c_stop(qts, unit);
        }
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const char *boards[] = {"t5_board", "t5ai_core", "aidk_ai_toy"};
    static const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
        {"core-clocks-known-tuples-analog-reset", test_core_clock_tuples},
        {"core-clocks-unknown-source-off-logs", test_core_clock_unknown},
        {"core-clocks-default-off", test_core_clock_default_off},
        {"core-clocks-peripheral-deadlines",
         test_core_clock_peripheral_deadlines},
        {"core-clocks-systick-live-pause-repeat", test_core_clock_systick_live},
        {"core-clocks-systick-disabled-zero-lpo",
         test_core_clock_systick_stopped},
        {"uart-tx-fifo-framing-order", test_uart_tx_fifo_frames},
        {"uart-tx-service-mask-w1c-routes", test_uart_tx_service_routes},
        {"uart-tx-clock-pause-rounding", test_uart_tx_clock_pause},
        {"uart-tx-directional-flush-reset", test_uart_tx_flush_reset},
        {"watchdog-deadline-time-horizon", test_watchdog_deadline_horizon},
        {"uart-clock-budget-rounding", test_uart_clock_budget},
        {"spi-clock-budget-rounding", test_spi_clock_budget},
        {"spi-ideal-apll-profiles-gates-transactions",
         test_spi_ideal_apll_profiles},
        {"spi-ideal-apll-committed-pulse-lifecycle",
         test_spi_ideal_apll_committed_pulse},
        {"spi-ideal-apll-atomic-commit-policy",
         test_spi_ideal_apll_atomic_commit},
        {"spi-ideal-apll-unknown-profile-logs",
         test_spi_ideal_apll_unknown_profile},
        {"spi-ideal-apll-remaining-cycle-preservation",
         test_spi_ideal_apll_cycle_budget},
        {"spi-ideal-apll-reset-cancellation", test_spi_ideal_apll_reset_cancel},
        {"spi-ideal-apll-default-off-uart-core-xtal",
         test_spi_ideal_apll_independence},
        {"i2c-clock-budget-rounding", test_i2c_clock_budget},
        {"pwm-mixed-preload-enable-atomic", test_pwm_mixed_preload_atomic},
        {"pwm-counters-compares-routes", test_pwm_counters_compares_routes},
        {"pwm-prescalers-gates", test_pwm_dividers_gates},
        {"pwm-preload-update-reset-cancel", test_pwm_preload_update_cancel},
        {"pwm-wrap-coalesce-unsupported", test_pwm_wrap_and_rejection},
        {"dma-widths-aliases-all-channels", test_dma_widths_aliases},
        {"dma-half-mask-w1c-routes", test_dma_half_mask_routes},
        {"dma-bus-error-partial-progress", test_dma_fault_progress},
        {"dma-round-robin-disable-reset", test_dma_schedule_cancel},
        {"dma-max-length-event-rearm", test_dma_max_length_rearm},
        {"dma-unsupported-modes-no-progress", test_dma_unsupported_modes},
        {"memory-uart-reset", test_memory_uart},
        {"uart-clocks-gates-divider", test_uart_clocks},
        {"uart-rx-clock-pause-reset", test_uart_rx_clock_pause},
        {"uart-rx-capacity-wrap-backpressure", test_uart_rx_capacity},
        {"uart-rx-width-snapshot-mask-flush", test_uart_rx_width_snapshot},
        {"uart-rx-width-backpressure", test_uart_rx_width_backpressure},
        {"watchdog-keys-expiry", test_watchdog},
        {"watchdog-sources-pause-recovery", test_watchdog_sources},
        {"spi-lsb-byte-order-cancel-routes", test_spi_lsb_byte_adapter},
        {"spi-native-frames-irq-routes", test_spi_native_frames_routes},
        {"spi-fifo-overflow-starvation", test_spi_fifo_errors},
        {"spi-completed-config-atomic-rejection",
         test_spi_completed_config_rejection},
        {"spi-fifo-threshold-boundaries", test_spi_fifo_thresholds},
        {"spi-clock-loss-reset-cancel", test_spi_clock_cancel},
        {"spi-invalid-cs-wiring", test_spi_bus_wiring_rejection},
        {"i2c-fifo-native-bus-transactions", test_i2c_fifo_transactions},
        {"i2c-w0c-nak-irq-routes", test_i2c_w0c_nak_routes},
        {"i2c-clock-reset-cancel", test_i2c_clock_reset_cancel},
        {"i2c-register-write-deadline", test_i2c_unchanged_deadline},
        {"timer-groups-channels-routes-w1c", test_timg_channels},
        {"timer-clock-snapshot-cancel", test_timg_clock_snapshot},
        {"timer-prescaler-boundaries", test_timg_prescaler},
        {"timer-32bit-wrap-lowered-end", test_timg_wrap},
        {"rtc-counter-clock-sync-reset", test_rtc_counter_clock},
        {"rtc-compare-routes-w1c", test_rtc_irq_w1c},
        {"rtc-64bit-upper-wrap", test_rtc_upper64},
        {"gpio-mask-w1c", test_gpio},
        {"board-led-key-wiring", test_board_wiring},
        {"gpio-external-release", test_gpio_external_release},
        {"analog-busy-cancel", test_analog},
        {"clock-ratio-routes", test_clock_monitor},
        {"clock-source-loss-reset", test_clock_cancel},
        {"lpo-mux-source-loss-gates-reset", test_lpo_mux},
        {"mailbox-order-full", test_mailbox},
        {"mailbox-protection-reset", test_mailbox_protection},
        {"nor-persistence-protection-cancel-readonly", test_nor},
        {"nor-without-host-backing", test_volatile_nor},
        {"board-owned-nor-immutable-link", test_board_owned_nor},
        {"explicit-logical-xip-mode", test_explicit_xip_mode},
    };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(boards); i++) {
        g_assert_true(qtest_has_machine(boards[i]));
        for (unsigned j = 0; j < G_N_ELEMENTS(tests); j++) {
            g_autofree char *path = g_strdup_printf("bk7258/%s/%s",
                                                   boards[i], tests[j].name);
            qtest_add_data_func(path, boards[i], tests[j].test);
        }
    }
    qtest_add_data_func("bk7258/aidk_ai_toy/key-contacts-pulls-independence",
                        "aidk_ai_toy", test_aidk_key_contacts);
    qtest_add_data_func("bk7258/aidk_ai_toy/leds-independent-read-only",
                        "aidk_ai_toy", test_aidk_leds);
    qtest_add_data_func("bk7258/aidk_ai_toy/key-irq55-mask-w1c",
                        "aidk_ai_toy", test_aidk_key_irqs);
    qtest_add_data_func("bk7258/aidk_ai_toy/held-contacts-reset",
                        "aidk_ai_toy", test_aidk_held_contacts_reset);
    qtest_add_data_func("bk7258/aidk_ai_toy/cli-contact-presets",
                        "aidk_ai_toy", test_aidk_cli_contacts);
    qtest_add_data_func("bk7258/t5_board/aidk-properties-absent",
                        "t5_board", test_aidk_properties_absent);
    qtest_add_data_func("bk7258/t5ai_core/aidk-properties-absent",
                        "t5ai_core", test_aidk_properties_absent);
    return g_test_run();
}
