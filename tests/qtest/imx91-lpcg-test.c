/*
 * QTest for i.MX91 LPCG clock gating reaching a consumer.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The LPCG DIRECT bit used to be storage the gating never reached: a block whose
 * gate the guest cleared kept ticking, because the CCM fed peripherals the raw
 * clock root.  Now each modelled consumer is fed the root GATED by its own LPCG,
 * so clearing DIRECT stops exactly that block.  This drives TPM2 (which has a
 * readable free-running counter): with its gate on the counter advances; clear
 * the TPM2 LPCG DIRECT and the counter HOLDS its last value (no clock, no edge); set it
 * again and the counter resumes.  A model that ignores the gate keeps counting.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/timer.h"

#define TPM2_BASE       0x44320000ULL
#define TPM_SC          0x10
#define TPM_CNT         0x14
#define TPM_MOD         0x18
#define SC_CMOD_1       (1u << 3)    /* clock mode 01: module clock, PS=0 */

/* CCM LPCG DIRECT for the TPM2 gate (ccm@44450000 + 0x8b40, bit0 = clock on). */
#define CCM_TPM2_DIRECT 0x44458b40ULL
#define GATE_ON         0x1u

static void test_gate_stops_tpm(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -display none");

    /* Sanity: the gate comes up RUNNING out of reset (DIRECT = 1). */
    g_assert_cmphex(qtest_readl(qts, CCM_TPM2_DIRECT) & GATE_ON, ==, GATE_ON);

    /* Enable TPM2's counter (MOD resets to 0xFFFF; select the module clock). */
    qtest_writel(qts, TPM2_BASE + TPM_SC, SC_CMOD_1);

    /*
     * Gate ON: the counter advances with virtual time.  0.5 ms steps at 24 MHz
     * are ~12000 ticks -- kept well under the 65536 period so nothing wraps and
     * "resumed > held" is unambiguous.
     */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2000);      /* 0.5 ms */
    uint32_t a = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2000);
    uint32_t b = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    g_assert_cmpuint(a, >, 0);
    g_assert_cmpuint(b, >, a);          /* still ticking */

    /*
     * Clear the TPM2 LPCG DIRECT: the block loses its clock.  The counter HOLDS
     * its last value -- CNT is clocked flip-flops, so removing the clock retains
     * the value; a gate is not a reset, so it does NOT zero (the fidelity 93 and
     * I converged on).  Freeze-at-0 here would be a reset the gate never issued.
     */
    qtest_writel(qts, CCM_TPM2_DIRECT, 0);
    uint32_t held = qtest_readl(qts, TPM2_BASE + TPM_CNT);
    g_assert_cmpuint(held, >, 0);       /* held, NOT zeroed */
    g_assert_cmpuint(held, >=, b);      /* at least the last running value */
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2000);
    g_assert_cmpuint(qtest_readl(qts, TPM2_BASE + TPM_CNT), ==, held);  /* frozen */

    /* Re-set the gate: the clock returns and the counter RESUMES from held, not 0. */
    qtest_writel(qts, CCM_TPM2_DIRECT, GATE_ON);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 2000);
    g_assert_cmpuint(qtest_readl(qts, TPM2_BASE + TPM_CNT), >, held);   /* resumed + advanced */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/lpcg/gate-stops-tpm", test_gate_stops_tpm);
    return g_test_run();
}
