/*
 * QTest for the i.MX91 watchdog (WDOG3) countdown RATE.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Linux fsl,imx93-wdt driver ALWAYS enables the /256 prescaler (CS.PRES)
 * and programs TOVAL = 125 * timeout_seconds -- it assumes the counter ticks at
 * 125 Hz (a 32 kHz LPO divided by 256).  So a guest that wants a T-second
 * watchdog writes TOVAL = 125*T; the model must fire after T seconds of virtual
 * time or the watchdog is a lie.
 *
 * This arms a 1-second watchdog the driver's way (TOVAL=125, PRES set), then
 * brackets the deadline: at 0.9 s it must NOT have fired (rejects a too-FAST
 * rate), and by 1.1 s it MUST have fired (rejects the too-SLOW rate this model
 * shipped with -- WDOG_HZ=1000 gave 1000/256 = 3 Hz, ~42x too late).  A fire is
 * a system reset, after which CS reads its reset value 0x2900 instead of the
 * armed value; that read is non-blocking, so the too-slow bug fails the
 * assertion cleanly rather than hanging.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/timer.h"

#define WDOG3_BASE      0x42490000ULL

#define WDOG_CS         0x0
#define WDOG_CNT        0x4
#define WDOG_TOVAL      0x8

#define CS_EN           (1u << 7)
#define CS_PRES         (1u << 12)
#define CS_CMD32EN      (1u << 13)

#define CS_RESET        0x00002900u     /* value CS reads after a device reset */

#define UNLOCK          0xd928c520u

/* TOVAL the driver would write for a 1-second timeout: 125 counts at 125 Hz. */
#define TOVAL_1S        125

static void test_prescaled_1s(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -display none "
                                 "-action watchdog=reset");

    /* Sanity: out of reset CS is the RM value and the watchdog is disarmed. */
    g_assert_cmphex(qtest_readl(qts, WDOG3_BASE + WDOG_CS), ==, CS_RESET);

    /* Arm it the driver's way: unlock, set TOVAL, then CS (EN|PRES) last. */
    qtest_writel(qts, WDOG3_BASE + WDOG_CNT, UNLOCK);
    qtest_writel(qts, WDOG3_BASE + WDOG_TOVAL, TOVAL_1S);
    qtest_writel(qts, WDOG3_BASE + WDOG_CS, CS_EN | CS_PRES | CS_CMD32EN);

    /* 0.9 s in: must still be armed (a too-fast rate would have fired). */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND * 9 / 10);
    g_assert_cmphex(qtest_readl(qts, WDOG3_BASE + WDOG_CS), !=, CS_RESET);

    /* Cross 1.0 s (total 1.1 s): must have fired -> system reset -> CS reset. */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND * 2 / 10);
    g_assert_cmphex(qtest_readl(qts, WDOG3_BASE + WDOG_CS), ==, CS_RESET);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/wdog/prescaled-1s", test_prescaled_1s);
    return g_test_run();
}
