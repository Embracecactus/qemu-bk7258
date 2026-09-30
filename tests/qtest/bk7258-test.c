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
        qtest_clock_step(qts, 5423); /* Remaining cycles rounded upward. */
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

int main(int argc, char **argv)
{
    static const char *boards[] = {"t5_board", "t5ai_core", "aidk_ai_toy"};
    static const struct {
        const char *name;
        GTestDataFunc test;
    } tests[] = {
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
        {"watchdog-keys-expiry", test_watchdog},
        {"watchdog-sources-pause-recovery", test_watchdog_sources},
        {"spi-native-frames-irq-routes", test_spi_native_frames_routes},
        {"spi-fifo-overflow-starvation", test_spi_fifo_errors},
        {"spi-fifo-threshold-boundaries", test_spi_fifo_thresholds},
        {"spi-clock-loss-reset-cancel", test_spi_clock_cancel},
        {"spi-invalid-cs-wiring", test_spi_bus_wiring_rejection},
        {"i2c-fifo-native-bus-transactions", test_i2c_fifo_transactions},
        {"i2c-w0c-nak-irq-routes", test_i2c_w0c_nak_routes},
        {"i2c-clock-reset-cancel", test_i2c_clock_reset_cancel},
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
