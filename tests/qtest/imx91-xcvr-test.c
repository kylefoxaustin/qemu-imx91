/*
 * QTest for the i.MX91 XCVR (SPDIF) registration + AI PHY/PLL handshake.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The transmit datapath (FIFO drain via cyclic eDMA) has no register-readable
 * fill, so it's validated end to end under Linux (a real SPDIF playback fills
 * the TX FIFO). What this kernel-free test covers is the probe-critical control
 * surface: the version register and the indirect "AI" PHY/PLL access the
 * fsl_xcvr driver polls - write a sub-register through PHY_AI_CTRL/WDATA, check
 * the DONE bit latches, read it back through PHY_AI_RDATA.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define XCVR_BASE   0x42680000ULL
#define REG_OFF     0x800

#define XCVR_VERSION    (REG_OFF + 0x00)
#define XCVR_AI_CTRL    (REG_OFF + 0x90)
#define XCVR_AI_WDATA   (REG_OFF + 0xa0)
#define XCVR_AI_RDATA   (REG_OFF + 0xa4)

#define AI_CTRL_RWB     (1u << 31)
#define AI_TOG_PLL      (1u << 24)
#define AI_DONE_PLL     (1u << 25)

#define VERSION_VALUE   0x00000000  /* RM: was fabricated 0x00010000 */

static uint32_t rd(QTestState *q, uint64_t off)
{
    return qtest_readl(q, XCVR_BASE + off);
}

static void wr(QTestState *q, uint64_t off, uint32_t v)
{
    qtest_writel(q, XCVR_BASE + off, v);
}

static void test_xcvr_ai(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -display none");

    g_assert_cmphex(rd(q, XCVR_VERSION), ==, VERSION_VALUE);

    /* AI write: stage WDATA, then a PLL-toggle write to sub-register 0x12. */
    wr(q, XCVR_AI_WDATA, 0xcafe1234);
    wr(q, XCVR_AI_CTRL, 0x12 | AI_TOG_PLL);
    g_assert_cmphex(rd(q, XCVR_AI_CTRL) & AI_DONE_PLL, ==, AI_DONE_PLL);

    /* AI read-back of the same sub-register returns what we wrote. */
    wr(q, XCVR_AI_CTRL, 0x12 | AI_CTRL_RWB | AI_TOG_PLL);
    g_assert_cmphex(rd(q, XCVR_AI_CTRL) & AI_DONE_PLL, ==, AI_DONE_PLL);
    g_assert_cmphex(rd(q, XCVR_AI_RDATA), ==, 0xcafe1234);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/xcvr/ai-handshake", test_xcvr_ai);
    return g_test_run();
}
