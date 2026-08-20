/*
 * QTest for the i.MX91 DDR controller + DDR PMU (register compat).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin.github@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The DDR PMU is a register/perf-interface compat block - QEMU cannot measure
 * real DDR bandwidth (DRAM is plain host memory, no controller in the path, no
 * cache model), so the counters always read 0. Check the region is mapped
 * (not data-aborting / not the catch-all), that config registers are
 * read-what-you-write, and that a counter reads back 0.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define DDRC_BASE   0x4e300000ULL
#define PMU_BASE    (DDRC_BASE + 0xdc0)     /* ddr-pmu@4e300dc0 */

#define PMGC0       0x40                    /* global control            */
#define PMLCA0      0x50                    /* counter 0 local control A */
#define PMC0        0x58                    /* counter 0 value           */

static void test_ddr_pmu(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -display none");

    /* Config registers are read-what-you-write (control-bit readback works). */
    qtest_writel(q, PMU_BASE + PMGC0, 0xc0000000);
    g_assert_cmphex(qtest_readl(q, PMU_BASE + PMGC0), ==, 0xc0000000);
    qtest_writel(q, PMU_BASE + PMLCA0, 0x04030000);
    g_assert_cmphex(qtest_readl(q, PMU_BASE + PMLCA0), ==, 0x04030000);

    /* A counter the driver cleared reads back 0 - no traffic measured. */
    qtest_writel(q, PMU_BASE + PMC0, 0);
    g_assert_cmphex(qtest_readl(q, PMU_BASE + PMC0), ==, 0);
    g_assert_cmphex(qtest_readl(q, PMU_BASE + PMC0 + 4), ==, 0);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/ddrc/pmu-compat", test_ddr_pmu);
    return g_test_run();
}
