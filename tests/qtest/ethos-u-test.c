/*
 * QTest for the Arm Ethos-U executor on the i.MX93 board.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a single int8 convolution through the NPU exactly as the firmware
 * would: the Vela-style command stream, the mlw-encoded weights, the scale/bias
 * stream and the IFM are placed in DRAM, the region bases and queue registers
 * are programmed, the engine is kicked, and the OFM written back is compared to
 * the golden. This exercises the whole in-QEMU path end to end (no BSP boot):
 * DMA fetch, command-stream parse, mlw decode, reorder inverse, scale unpack,
 * int8 conv, requant, OFM writeback and the completion IRQ/STATUS.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "ethos-u-test-data.h"

/* NPU APB block on the i.MX93. */
#define NPU_BASE        0x4a900000ULL
#define REG_STATUS      0x04
#define REG_CMD         0x08
#define REG_QBASE       0x10
#define REG_QBASE_HI    0x14
#define REG_QSIZE       0x20
#define REG_BASEP0      0x80

#define CMD_RUN         (1u << 0)
#define STATUS_IRQ      (1u << 1)
#define STATUS_END      (1u << 5)
#define STATUS_PARSE_ERR (1u << 4)

static void npu_writel(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, NPU_BASE + off, v);
}

static uint32_t npu_readl(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, NPU_BASE + off);
}

static void test_conv(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    uint8_t ofm[EU_OFM_LEN];
    uint32_t status = 0;
    int i;

    /* Stage the command stream, weights, scales and IFM in DRAM. */
    qtest_memwrite(qts, EU_CMS_BASE, eu_cms, sizeof(eu_cms));
    qtest_memwrite(qts, EU_WS_BASE + EU_WEIGHT_OFF, eu_weights,
                   sizeof(eu_weights));
    qtest_memwrite(qts, EU_WS_BASE + EU_SCALE_OFF, eu_scales,
                   sizeof(eu_scales));
    qtest_memwrite(qts, EU_ARENA_BASE + EU_IFM_OFF, eu_ifm, sizeof(eu_ifm));

    /* Region bases: BASEP0 = weights/scales region, BASEP1 = arena. */
    npu_writel(qts, REG_BASEP0 + 0, (uint32_t)EU_WS_BASE);
    npu_writel(qts, REG_BASEP0 + 4, (uint32_t)(EU_WS_BASE >> 32));
    npu_writel(qts, REG_BASEP0 + 8, (uint32_t)EU_ARENA_BASE);
    npu_writel(qts, REG_BASEP0 + 12, (uint32_t)(EU_ARENA_BASE >> 32));

    /* Queue + kick. */
    npu_writel(qts, REG_QBASE, (uint32_t)EU_CMS_BASE);
    npu_writel(qts, REG_QBASE_HI, (uint32_t)(EU_CMS_BASE >> 32));
    npu_writel(qts, REG_QSIZE, sizeof(eu_cms));
    npu_writel(qts, REG_CMD, CMD_RUN);

    /* The job runs on a worker thread; poll STATUS for completion. */
    for (i = 0; i < 10000; i++) {
        status = npu_readl(qts, REG_STATUS);
        if (status & STATUS_IRQ) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(status & STATUS_IRQ, ==, STATUS_IRQ);
    g_assert_cmphex(status & STATUS_END, ==, STATUS_END);
    g_assert_cmphex(status & STATUS_PARSE_ERR, ==, 0);

    qtest_memread(qts, EU_ARENA_BASE + EU_OFM_OFF, ofm, sizeof(ofm));
    for (i = 0; i < EU_OFM_LEN; i++) {
        g_assert_cmpint((int8_t)ofm[i], ==, (int8_t)eu_ofm_golden[i]);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ethos-u/conv", test_conv);
    return g_test_run();
}
