/*
 * BK7258 peripheral clock-budget arithmetic tests, without a machine/socket.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * AI-assisted downstream experiment, not an upstream contribution.
 */
#include "qemu/osdep.h"
#include "hw/misc/bk7258_clock.h"

static int64_t deadline(uint64_t budget, unsigned hz, int64_t base)
{
    return base + DIV_ROUND_UP(budget * NANOSECONDS_PER_SECOND, hz);
}

static void test_known_edges(void)
{
    static const struct {
        uint32_t budget, hz;
        int64_t elapsed;
        uint32_t remaining;
    } cases[] = {
        { 7232, 26000000, 0, 7232 },
        { 7232, 26000000, 77, 7230 },
        { 16, 26000000, 39, 15 },
        { 16, 26000000, 385, 6 },
        { 16, 26000000, 615, 1 },
        { 16, 26000000, 616, 0 },
        { 648, 26000000, 39, 647 },
        { 648, 26000000, 385, 638 },
        { 2, 24000000, 42, 1 },
        { 2, 26000000, 0, 2 },
        { 2, 26000000, 77, 0 },
        { 0, 26000000, 0, 0 },
        { 1, 1, 999999999, 1 },
        { 1, 1, 1000000000, 0 },
        { UINT32_MAX, 1, (UINT32_MAX - 1ULL) * 1000000000, 1 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        for (unsigned horizon = 0; horizon < 2; horizon++) {
            int64_t duration = deadline(cases[i].budget, cases[i].hz, 0);
            int64_t base = horizon ? INT64_MAX - duration : 12345;

            g_assert_cmpuint(bk7258_remaining_cycles(
                cases[i].budget, cases[i].hz, base + duration,
                base + cases[i].elapsed), ==, cases[i].remaining);
        }
    }
    g_assert_cmpuint(bk7258_remaining_cycles(16, 26000000, 1000, 100), ==, 16);
}

static void test_zero_time_rearm(void)
{
    static const uint32_t budgets[] = { 2, 16, 648, 7232, 16777216 };
    static const unsigned rates[] = { 24000000, 26000000, 32768 };

    for (unsigned b = 0; b < G_N_ELEMENTS(budgets); b++) {
        uint64_t budget = budgets[b];

        for (unsigned n = 0; n < 1024; n++) {
            unsigned hz = rates[n % G_N_ELEMENTS(rates)];

            budget = bk7258_remaining_cycles(budget, hz,
                                              deadline(budget, hz, 100), 100);
            g_assert_cmpuint(budget, ==, budgets[b]);
        }
    }
}

static void test_exact_reference(void)
{
    uint64_t state = 0x7258a11c;

    for (unsigned i = 0; i < 100000; i++) {
        uint64_t numerator, elapsed, remaining, budget, duration;
        unsigned hz;

        state = state * 6364136223846793005ULL + 1;
        budget = state % (1U << 24);
        hz = 1 + (state >> 32) % 100000000;
        numerator = budget * NANOSECONDS_PER_SECOND;
        duration = DIV_ROUND_UP(numerator, hz);
        elapsed = (state >> 16) % (duration + 2);
        /* Independent exact rational remainder in source-cycle units. */
        remaining = elapsed >= duration ? 0 : DIV_ROUND_UP(
            numerator - elapsed * hz, NANOSECONDS_PER_SECOND);
        g_assert_cmpuint(bk7258_remaining_cycles(
            budget, hz, duration + 1000, elapsed + 1000), ==, remaining);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/bk7258-clock/known-edges", test_known_edges);
    g_test_add_func("/bk7258-clock/zero-time-rearm", test_zero_time_rearm);
    g_test_add_func("/bk7258-clock/exact-reference", test_exact_reference);
    return g_test_run();
}
