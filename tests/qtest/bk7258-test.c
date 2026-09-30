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
    expect(qts, 0x44820018, 0x1a0000);
    qtest_writel(qts, 0x44820008, 1);
    qtest_writel(qts, 0x44820010, 3);
    qtest_writel(qts, 0x44820020, 32);
    qtest_writel(qts, 0x4482001c, 'A');
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
    qtest_writel(qts, 0x44820010, (259U << 8) | 3); /* 100 kbit/s at XTAL. */
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

int main(int argc, char **argv)
{
    static const char *boards[] = {"t5_board", "t5ai_core", "aidk_ai_toy"};
    static const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
        {"memory-uart-reset", test_memory_uart},
        {"uart-clocks-gates-divider", test_uart_clocks},
        {"uart-rx-clock-pause-reset", test_uart_rx_clock_pause},
        {"uart-rx-capacity-wrap-backpressure", test_uart_rx_capacity},
        {"watchdog-keys-expiry", test_watchdog},
        {"watchdog-sources-pause-recovery", test_watchdog_sources},
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
    return g_test_run();
}
