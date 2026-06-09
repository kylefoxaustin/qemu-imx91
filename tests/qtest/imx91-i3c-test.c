/*
 * QTest for the i.MX91 Silvaco I3C master + a legacy I2C target (wm8962).
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The imx91-...-i3c DTB puts the wm8962 codec on the I3C bus as a legacy I2C
 * device at 0x1a. Drive the controller the way svc-i3c-master does for a
 * register read - emit a START with an I2C address + write the 16-bit register
 * pointer, repeated-START for the read, pull the bytes from MRDATAB - and check
 * the wm8962 device-id register (0x0f) reads back 0x6243.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define I3C_BASE    0x44330000ULL

#define MCTRL       0x084
#define MSTATUS     0x088
#define MERRWARN    0x09c
#define MDATACTRL   0x0ac
#define MWDATAB     0x0b0
#define MWDATABE    0x0b4
#define MRDATAB     0x0c0

#define REQ_START   1
#define REQ_STOP    2
#define TYPE_I2C    (1u << 4)
#define DIR_READ    (1u << 8)
#define ADDR(a)     ((uint32_t)(a) << 9)
#define RDTERM(n)   ((uint32_t)(n) << 16)

#define MINT_MCTRLDONE  (1u << 9)
#define MERRWARN_NACK   (1u << 2)

#define WM8962_ADDR     0x1a
#define WM8962_DEVID    0x6243          /* SOFTWARE_RESET (reg 0x0f) */

static uint32_t rd(QTestState *q, uint64_t off)
{
    return qtest_readl(q, I3C_BASE + off);
}

static void wr(QTestState *q, uint64_t off, uint32_t v)
{
    qtest_writel(q, I3C_BASE + off, v);
}

static void test_wm8962_id(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -display none");
    uint32_t hi, lo;

    /* START + write address: set the 16-bit register pointer to 0x000f. */
    wr(q, MCTRL, REQ_START | TYPE_I2C | ADDR(WM8962_ADDR));
    g_assert_cmphex(rd(q, MSTATUS) & MINT_MCTRLDONE, ==, MINT_MCTRLDONE);
    g_assert_cmphex(rd(q, MERRWARN) & MERRWARN_NACK, ==, 0);   /* it ACKed */
    wr(q, MWDATAB, 0x00);
    wr(q, MWDATABE, 0x0f);

    /* Repeated START for the read of two value bytes. */
    wr(q, MCTRL, REQ_START | TYPE_I2C | DIR_READ | ADDR(WM8962_ADDR) |
                 RDTERM(2));
    g_assert_cmphex(rd(q, MERRWARN) & MERRWARN_NACK, ==, 0);
    hi = rd(q, MRDATAB) & 0xff;
    lo = rd(q, MRDATAB) & 0xff;
    wr(q, MCTRL, REQ_STOP);

    g_assert_cmphex((hi << 8) | lo, ==, WM8962_DEVID);

    /* An address with no device NACKs. */
    wr(q, MCTRL, REQ_START | TYPE_I2C | ADDR(0x55));
    g_assert_cmphex(rd(q, MERRWARN) & MERRWARN_NACK, ==, MERRWARN_NACK);
    wr(q, MCTRL, REQ_STOP);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/i3c/wm8962-id", test_wm8962_id);
    return g_test_run();
}
