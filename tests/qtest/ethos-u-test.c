/*
 * QTest for the Arm Ethos-U executor on the i.MX93 board.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a series of int8 convolutions through the NPU exactly as the firmware
 * would: each case stages a Vela-style command stream, mlw-encoded weights, a
 * scale/bias stream and an IFM in DRAM, programs the region bases + queue, kicks
 * the engine and compares the OFM written back to the golden. The cases escalate
 * from the single-brick path to the multi-brick / depth-split path wider models
 * need, so a failure pinpoints which executor path is wrong. No BSP boot.
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

#define CMD_RUN          (1u << 0)
#define STATUS_IRQ       (1u << 1)
#define STATUS_END       (1u << 5)
#define STATUS_PARSE_ERR (1u << 4)

static void npu_writel(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, NPU_BASE + off, v);
}

static void run_case(QTestState *qts, const EuCase *c)
{
    g_autofree uint8_t *ofm = g_malloc(c->ofm_len);
    uint32_t status = 0;
    int i;

    for (i = 0; i < c->n_pokes; i++) {
        qtest_memwrite(qts, c->pokes[i].addr, c->pokes[i].data,
                       c->pokes[i].len);
    }
    qtest_memwrite(qts, CMS_BASE_ADDR, c->cms, c->cms_len);

    npu_writel(qts, REG_BASEP0 + 0, (uint32_t)c->basep0);
    npu_writel(qts, REG_BASEP0 + 4, (uint32_t)(c->basep0 >> 32));
    npu_writel(qts, REG_BASEP0 + 8, (uint32_t)c->basep1);
    npu_writel(qts, REG_BASEP0 + 12, (uint32_t)(c->basep1 >> 32));

    npu_writel(qts, REG_QBASE, (uint32_t)CMS_BASE_ADDR);
    npu_writel(qts, REG_QBASE_HI, (uint32_t)(CMS_BASE_ADDR >> 32));
    npu_writel(qts, REG_QSIZE, c->cms_len);
    npu_writel(qts, REG_CMD, CMD_RUN);

    for (i = 0; i < 10000; i++) {
        status = qtest_readl(qts, NPU_BASE + REG_STATUS);
        if (status & STATUS_IRQ) {
            break;
        }
        g_usleep(1000);
    }

    g_assert_cmphex(status & STATUS_IRQ, ==, STATUS_IRQ);
    g_assert_cmphex(status & STATUS_END, ==, STATUS_END);
    g_assert_cmphex(status & STATUS_PARSE_ERR, ==, 0);

    qtest_memread(qts, c->ofm_addr, ofm, c->ofm_len);
    for (i = 0; i < (int)c->ofm_len; i++) {
        if (ofm[i] != c->golden[i]) {
            g_test_message("%s: OFM[%d] = %d, want %d", c->name, i,
                           (int8_t)ofm[i], (int8_t)c->golden[i]);
        }
        g_assert_cmpint((int8_t)ofm[i], ==, (int8_t)c->golden[i]);
    }
}

static void test_conv(void)
{
    QTestState *qts = qtest_init("-machine imx93-11x11-evk -display none");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(eu_cases); i++) {
        run_case(qts, &eu_cases[i]);
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ethos-u/conv", test_conv);
    return g_test_run();
}
