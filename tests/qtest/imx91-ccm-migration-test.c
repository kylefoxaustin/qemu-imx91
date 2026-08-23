/*
 * QTest: the i.MX91 MICFIL audio clock/gate state survives migration.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin.github@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 93emulator's audit #5 raised a ccm clock-recompute gap: consumers that read a
 * ccm root LIVE (clock_get_hz) with no VMSTATE_CLOCK see the reset rate
 * after loadvm unless ccm re-derives its outputs in a post_load. This checks
 * whether that gap reaches the i.MX 91 -- and it does NOT: our MICFIL migrates
 * its own mclk via VMSTATE_CLOCK, so the migrated rate (here: 0 Hz, from a
 * cleared PDM LPCG gate) survives directly, no ccm post_load required.
 *
 * Observable borrowed from imx91-lpcg-test's gate-stops-micfil: with the PDM
 * LPCG cleared, mclk reads 0 and MICFIL DATACH0 goes (and stays) 0. Then
 * migrate with the gate cleared; the dest DATACH0 must still read 0. If the
 * mclk did NOT migrate, the dest would come up at reset (LPCG on, nonzero) and
 * DATACH0 would read the running sawtooth.
 *
 * Mutation-proven: drop VMSTATE_CLOCK(mclk) from vmstate_imx93_micfil and the
 * destination reverts to the reset rate -- DATACH0 reads nonzero, case fails.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/timer.h"

#define MICFIL_BASE     0x44520000ULL
#define MICFIL_CTRL1    0x00
#define MICFIL_DATACH0  0x24
#define MICFIL_PDMIEN   (1u << 29)

/* MICFIL PDM LPCG DIRECT gate (ccm@44450000 + 0x9ac0, bit0 = on). */
#define CCM_PDM_DIRECT  0x44459ac0ULL
#define GATE_ON         0x1u

static void wait_for_migration_completed(QTestState *who)
{
    while (true) {
        QDict *rsp = qtest_qmp(who, "{ 'execute': 'query-migrate' }");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_get_str(ret, "status");
        bool done = g_str_equal(status, "completed");
        bool failed = g_str_equal(status, "failed");

        qobject_unref(rsp);
        if (failed) {
            g_test_message("migration failed");
            g_assert_not_reached();
        }
        if (done) {
            return;
        }
        g_usleep(1000);
    }
}

static void test_micfil_gate_survives_migration(void)
{
    g_autofree char *tmp = g_dir_make_tmp("imx91-ccm-mig.XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *sock = g_strdup_printf("%s/mig.sock", tmp);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    int i;

    /* Source: enable MICFIL (pdm_root = osc_24m at reset, feed runs). */
    QTestState *src = qtest_init("-machine imx91-11x11-evk -display none");
    g_assert_cmphex(qtest_readl(src, CCM_PDM_DIRECT) & GATE_ON, ==, GATE_ON);
    qtest_writel(src, MICFIL_BASE + MICFIL_CTRL1, MICFIL_PDMIEN);
    qtest_clock_step(src, NANOSECONDS_PER_SECOND / 10000);  /* ~0.1ms feed */

    /* Running: DATACH0 yields a varying stream (the sawtooth), not stuck. */
    g_assert_cmpuint(qtest_readl(src, MICFIL_BASE + MICFIL_DATACH0), !=,
                     qtest_readl(src, MICFIL_BASE + MICFIL_DATACH0));

    /* Clear PDM LPCG: mclk -> 0, feed freezes, DATACH0 drains to 0. */
    qtest_writel(src, CCM_PDM_DIRECT, 0);
    for (i = 0; i < 40; i++) {         /* drain the held FIFO (depth 32) */
        qtest_readl(src, MICFIL_BASE + MICFIL_DATACH0);
    }
    g_assert_cmpuint(qtest_readl(src, MICFIL_BASE + MICFIL_DATACH0), ==, 0);

    /* Migrate with the gate cleared. */
    QTestState *dst = qtest_init("-machine imx91-11x11-evk -display none "
                                 "-incoming defer");
    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                                  "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                                  "  'arguments': { 'uri': %s } }", uri);
    wait_for_migration_completed(src);

    /*
     * Destination: mclk migrated as 0 (cleared gate), so MICFIL is still gated
     * and DATACH0 reads 0. A reset-era mclk (gate on) would give the sawtooth.
     */
    g_assert_cmpuint(qtest_readl(dst, MICFIL_BASE + MICFIL_DATACH0), ==, 0);
    g_assert_cmpuint(qtest_readl(dst, MICFIL_BASE + MICFIL_DATACH0), ==, 0);

    qtest_quit(dst);
    qtest_quit(src);
    unlink(sock);
    rmdir(tmp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/ccm/micfil-gate-survives-migration",
                   test_micfil_gate_survives_migration);
    return g_test_run();
}
