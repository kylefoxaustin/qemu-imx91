/*
 * QTest for the NXP LPSPI controller model (on the i.MX 91 machine).
 *
 * Copyright (c) 2026, Kyle Fox
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Exercises the LPSPI master transfer engine (hw/ssi/imx93_lpspi.c) without a
 * kernel: VERID/PARAM identity, the CR.MEN transfer gate, a TDR write driving
 * a frame onto the SSI bus and latching TCF/FCF + an RX word, and the RX-FIFO
 * reset. (The model bridges TDR writes onto a QEMU SSI bus and returns the
 * shifted-in byte via RDR; with no slave wired on the bus the shifted-in value
 * is the idle-bus default, so this checks the engine/flags, not slave data.)
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/* LPSPI1 register base (i.MX 91, == i.MX 93). */
#define LPSPI1      0x44360000

#define VERID   0x00
#define PARAM   0x04
#define CR      0x10
#define SR      0x14
#define CFGR1   0x24
#define FSR     0x5c
#define TCR     0x60
#define TDR     0x64
#define RDR     0x74

#define CR_MEN   (1u << 0)
#define CR_RRF   (1u << 9)
#define SR_FCF   (1u << 9)
#define SR_TCF   (1u << 10)

#define VERID_VALUE 0x02000004
#define PARAM_VALUE 0x00000404

#define RXCOUNT(fsr) (((fsr) >> 16) & 0xff)

static void test_identity(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -accel qtest");

    g_assert_cmphex(qtest_readl(qts, LPSPI1 + VERID), ==, VERID_VALUE);
    g_assert_cmphex(qtest_readl(qts, LPSPI1 + PARAM), ==, PARAM_VALUE);

    qtest_quit(qts);
}

static void test_men_gate_and_transfer(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -accel qtest");

    /* 8-bit frame, single (non-continuous) so the frame completes (FCF). */
    qtest_writel(qts, LPSPI1 + TCR, 7);

    /* MEN off: a TDR write must be ignored - no transfer, no RX word. */
    qtest_writel(qts, LPSPI1 + CR, 0);
    qtest_writel(qts, LPSPI1 + TDR, 0xab);
    g_assert_false(qtest_readl(qts, LPSPI1 + SR) & SR_TCF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    /* MEN on: the TDR write shifts a frame; TCF+FCF set, one RX word queued. */
    qtest_writel(qts, LPSPI1 + CR, CR_MEN);
    qtest_writel(qts, LPSPI1 + TDR, 0xab);
    g_assert_true(qtest_readl(qts, LPSPI1 + SR) & SR_TCF);
    g_assert_true(qtest_readl(qts, LPSPI1 + SR) & SR_FCF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 1);

    /* Reading RDR pops the RX word, draining the FIFO. */
    (void)qtest_readl(qts, LPSPI1 + RDR);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    qtest_quit(qts);
}

static void test_rxfifo_reset(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -accel qtest");

    qtest_writel(qts, LPSPI1 + TCR, 7);
    qtest_writel(qts, LPSPI1 + CR, CR_MEN);
    qtest_writel(qts, LPSPI1 + TDR, 0x55);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 1);

    /* CR.RRF drains the RX FIFO. */
    qtest_writel(qts, LPSPI1 + CR, CR_MEN | CR_RRF);
    g_assert_cmpuint(RXCOUNT(qtest_readl(qts, LPSPI1 + FSR)), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91-lpspi/identity", test_identity);
    qtest_add_func("/imx91-lpspi/men-gate-and-transfer",
                   test_men_gate_and_transfer);
    qtest_add_func("/imx91-lpspi/rxfifo-reset", test_rxfifo_reset);
    return g_test_run();
}
