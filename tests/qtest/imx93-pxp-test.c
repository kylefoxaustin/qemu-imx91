/*
 * QTest for the i.MX93 PXP (Pixel Pipeline) 2D engine.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a PXP copy the way the pxp_dma_v3 driver does for a g2d_copy, with no
 * kernel: a source pattern is staged in DRAM, the PS (source) and OUT (dest)
 * surface registers are programmed, the ENABLE bit kicks the engine, and the
 * OFM written back is checked byte-exact against the source while STAT reports
 * the completion interrupt. The register sequence matches the one captured from
 * the live driver in tests/pxp-imx93.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define PXP_BASE        0x4ae20000ULL

/* MXS register heads + the SET alias used to kick. */
#define PXP_CTRL_SET    0x04
#define PXP_STAT        0x10
#define PXP_OUT_CTRL    0x20
#define PXP_OUT_BUF     0x30
#define PXP_OUT_PITCH   0x50
#define PXP_OUT_LRC     0x60
#define PXP_OUT_PS_ULC  0x70
#define PXP_OUT_PS_LRC  0x80
#define PXP_PS_CTRL     0xb0
#define PXP_PS_BUF      0xc0
#define PXP_PS_PITCH    0xf0

#define CTRL_ENABLE     0x1
#define STAT_IRQ0       0x1
#define FMT_RGB888      0x4     /* 32-bit stored */

/* Guest DRAM (DDR @ 0x8000_0000). */
#define SRC_ADDR        0x80100000ULL
#define DST_ADDR        0x80200000ULL

#define W   64
#define H   32
#define BPP 4
#define PITCH  (W * BPP)
#define FM_LEN (PITCH * H)

static void pxp_writel(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, PXP_BASE + off, v);
}

static void test_copy(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    g_autofree uint8_t *src = g_malloc(FM_LEN);
    g_autofree uint8_t *dst = g_malloc(FM_LEN);
    uint32_t stat;
    int i;

    for (i = 0; i < FM_LEN; i++) {
        src[i] = (uint8_t)(i * 7 + 0x11);
    }
    qtest_memwrite(qts, SRC_ADDR, src, FM_LEN);
    memset(dst, 0xa5, FM_LEN);
    qtest_memwrite(qts, DST_ADDR, dst, FM_LEN);

    /* Program the source (PS) and destination (OUT) surfaces. */
    pxp_writel(qts, PXP_PS_CTRL, FMT_RGB888);
    pxp_writel(qts, PXP_PS_BUF, (uint32_t)SRC_ADDR);
    pxp_writel(qts, PXP_PS_PITCH, PITCH);
    pxp_writel(qts, PXP_OUT_CTRL, FMT_RGB888);
    pxp_writel(qts, PXP_OUT_BUF, (uint32_t)DST_ADDR);
    pxp_writel(qts, PXP_OUT_PITCH, PITCH);
    pxp_writel(qts, PXP_OUT_LRC, ((W - 1) << 16) | (H - 1));
    pxp_writel(qts, PXP_OUT_PS_ULC, 0);
    pxp_writel(qts, PXP_OUT_PS_LRC, ((W - 1) << 16) | (H - 1));

    /* Kick: the model runs the pass synchronously on this write. */
    pxp_writel(qts, PXP_CTRL_SET, CTRL_ENABLE);

    /* Completion interrupt is latched in STAT. */
    stat = qtest_readl(qts, PXP_BASE + PXP_STAT);
    g_assert_cmphex(stat & STAT_IRQ0, ==, STAT_IRQ0);

    /* OFM must be a byte-exact copy of the source. */
    qtest_memread(qts, DST_ADDR, dst, FM_LEN);
    for (i = 0; i < FM_LEN; i++) {
        if (dst[i] != src[i]) {
            g_test_message("OFM[%d] = %#x, want %#x", i, dst[i], src[i]);
        }
        g_assert_cmpuint(dst[i], ==, src[i]);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/aarch64/imx93-pxp/copy", test_copy);
    return g_test_run();
}
