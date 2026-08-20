/*
 * QTest for i.MX91 LPCG clock gating reaching a consumer.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin.github@gmail.com>
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

/* MICFIL + its PDM LPCG gate (ccm + 0x9ac0).  pdm_root reads osc_24m at reset,
 * so the feed runs in a bare qtest without the audio PLL. */
#define MICFIL_BASE     0x44520000ULL
#define MICFIL_CTRL1    0x00
#define MICFIL_DATACH0  0x24
#define MICFIL_PDMIEN   (1u << 29)
#define CCM_PDM_DIRECT  0x44459ac0ULL

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

/*
 * The gate must stop the DATA, not just the pacing.  MICFIL synthesises samples
 * on the feed tick AND on demand when the FIFO is drained (so an out-running eDMA
 * never reads silence).  Gating only the tick would leave the on-demand path
 * feeding bytes through a cleared gate.  So: enable MICFIL (mclk = pdm_root =
 * osc_24m at reset, so it runs), clear the PDM LPCG, drain the held FIFO, and the
 * data path must go SILENT -- DATACH0 reads 0 and stays 0.  A model that gates
 * only the clock but keeps the on-demand synth keeps returning the sawtooth.
 */
static void test_gate_stops_micfil(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -display none");
    int i;

    /* Gate on at reset; enable the PDM interface -> the feed runs. */
    g_assert_cmphex(qtest_readl(qts, CCM_PDM_DIRECT) & GATE_ON, ==, GATE_ON);
    qtest_writel(qts, MICFIL_BASE + MICFIL_CTRL1, MICFIL_PDMIEN);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 10000);   /* ~0.1 ms of feed */

    /* Running: DATACH0 yields a varying stream (the sawtooth), not stuck. */
    uint32_t d0 = qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0);
    uint32_t d1 = qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0);
    g_assert_cmpuint(d0, !=, d1);        /* producing data */

    /* Clear the PDM LPCG: the feed freezes and the on-demand synth is suppressed. */
    qtest_writel(qts, CCM_PDM_DIRECT, 0);
    for (i = 0; i < 40; i++) {           /* drain any held FIFO (depth 32) */
        qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0);
    }
    /* Two consecutive 0s: the running sawtooth never repeats 0, so this is gated. */
    g_assert_cmpuint(qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0), ==, 0);

    /* Re-set the gate: the data path returns. */
    qtest_writel(qts, CCM_PDM_DIRECT, GATE_ON);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 10000);
    uint32_t e0 = qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0);
    uint32_t e1 = qtest_readl(qts, MICFIL_BASE + MICFIL_DATACH0);
    g_assert_cmpuint(e0, !=, e1);        /* producing data again */

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/lpcg/gate-stops-tpm", test_gate_stops_tpm);
    qtest_add_func("/imx91/lpcg/gate-stops-micfil", test_gate_stops_micfil);
    return g_test_run();
}
