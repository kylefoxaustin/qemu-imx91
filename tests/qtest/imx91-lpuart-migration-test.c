/*
 * QTest: the i.MX91 LPUART IRQ line survives migration (post_load).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin.github@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The LPUART IRQ line level is derived from STAT & CTRL, not carried in the
 * migration stream. Without a .post_load re-deriving it, an interrupt that was
 * pending-but-unacked at savevm is lost across loadvm and a driver blocked on
 * it never wakes. This is the end-to-end proof of that post_load:
 *
 *   source     : TDRE is set at reset (empty TX); set TIE -> line asserts.
 *   destination: booted -incoming defer with the LPUART's output IRQ
 *                intercepted BEFORE the stream loads, so the post_load's
 *                qemu_set_irq is captured; after migrate the line reads HIGH.
 *
 * Mutation-proven: drop imx_lpuart_post_load and the dest line reads LOW.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define LPUART1_BASE    0x44380000ULL
#define LPUART_STAT     0x14
#define LPUART_CTRL     0x18
#define STAT_TDRE       0x00800000  /* TX data reg empty; set at reset */
#define CTRL_TIE        0x00800000      /* TDRE interrupt enable */

/*
 * Intercept the LPUART's single output IRQ (line 0) rather than the GIC input,
 * so we watch exactly one line and dodge the GIC's 320-wide input space.
 */
#define LPUART1_PATH    "/machine/soc/lpuart1"

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

static void test_lpuart_irq_survives_migration(void)
{
    g_autofree char *tmp = g_dir_make_tmp("imx91-lpuart-mig.XXXXXX", NULL);
    g_assert_nonnull(tmp);
    g_autofree char *sock = g_strdup_printf("%s/mig.sock", tmp);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    /*
     * Source: raise the LPUART1 IRQ. STAT.TDRE is set out of reset (empty TX),
     * so enabling CTRL.TIE drives the line high via imx_lpuart_update_irq.
     */
    QTestState *src = qtest_init("-machine imx91-11x11-evk -display none");
    qtest_irq_intercept_out_named(src, LPUART1_PATH, "sysbus-irq");
    g_assert_cmphex(qtest_readl(src, LPUART1_BASE + LPUART_STAT) & STAT_TDRE,
                    ==, STAT_TDRE);
    qtest_writel(src, LPUART1_BASE + LPUART_CTRL, CTRL_TIE);
    g_assert_true(qtest_get_irq(src, 0));      /* asserted on source */

    /*
     * Destination: -incoming defer. Intercept the GIC input BEFORE loading the
     * stream so the qemu_set_irq issued from imx_lpuart_post_load is recorded.
     */
    QTestState *dst = qtest_init("-machine imx91-11x11-evk -display none "
                                 "-incoming defer");
    qtest_irq_intercept_out_named(dst, LPUART1_PATH, "sysbus-irq");

    qtest_qmp_assert_success(dst, "{ 'execute': 'migrate-incoming',"
                                  "  'arguments': { 'uri': %s } }", uri);
    qtest_qmp_assert_success(src, "{ 'execute': 'migrate',"
                                  "  'arguments': { 'uri': %s } }", uri);
    wait_for_migration_completed(src);

    /* The line must be re-asserted on the destination by post_load. */
    g_assert_true(qtest_get_irq(dst, 0));

    qtest_quit(dst);
    qtest_quit(src);
    unlink(sock);
    rmdir(tmp);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/lpuart/irq-survives-migration",
                   test_lpuart_irq_survives_migration);
    return g_test_run();
}
