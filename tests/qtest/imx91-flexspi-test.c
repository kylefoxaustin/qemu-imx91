/*
 * QTest for the i.MX91 FlexSPI controller + attached SPI-NOR flash.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a READ-ID (9Fh) sequence through the LUT / IP command path and checks
 * the JEDEC id of the board's is25wp064 flash comes back in RFDR, then reads
 * the AHB-mapped (XIP) window and checks an erased flash reads back 0xff.
 *
 * A second test selects the SPI-NAND (-machine ...,flexspi-flash=gd5f4gq4) and
 * checks READ-ID returns the Gigadevice manufacturer + device id, the way the
 * Linux spi-nand stack detects the chip.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define FSPI_BASE   0x425e0000
#define FSPI_AHB    0x28000000

#define FSPI_MCR0   0x00
#define FSPI_INTR   0x14
#define FSPI_LUTKEY 0x18
#define FSPI_LCKCR  0x1c
#define FSPI_IPCR0  0xa0
#define FSPI_IPCR1  0xa4
#define FSPI_IPCMD  0xb0
#define FSPI_IPRXFCR 0xb8
#define FSPI_DLLACR 0xc0
#define FSPI_DLLBCR 0xc4
#define FSPI_STS2   0xe8
#define FSPI_RFDR   0x100
#define FSPI_LUT    0x200

/* STS2 DLL lock bits, and DLLACR/DLLBCR fields the driver actually writes. */
#define STS2_ASLVLOCK   (1u << 0)
#define STS2_AREFLOCK   (1u << 1)
#define STS2_BSLVLOCK   (1u << 16)
#define STS2_BREFLOCK   (1u << 17)
#define STS2_A_LOCK     (STS2_ASLVLOCK | STS2_AREFLOCK)
#define STS2_B_LOCK     (STS2_BSLVLOCK | STS2_BREFLOCK)
#define DLLACR_DLLEN    (1u << 0)       /* spi-nxp-fspi.c: FSPI_DLLACR_DLLEN = BIT(0) */
#define DLLACR_SLVDLY(x) ((x) << 3)

#define LUTKEY_VAL  0x5af05af0
#define IS25WP064_JEDEC 0x17709d    /* 9d 70 17, packed little-endian */

static uint32_t rd(QTestState *q, uint32_t off)
{
    return qtest_readl(q, FSPI_BASE + off);
}

static void wr(QTestState *q, uint32_t off, uint32_t val)
{
    qtest_writel(q, FSPI_BASE + off, val);
}

static void test_read_id(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -m 4G "
                               "-display none -kernel /dev/null");
    uint16_t cmd = (0x01 << 10) | 0x9f;     /* LUT_CMD, opcode 9Fh */
    uint16_t rd_i = (0x09 << 10) | 0x00;    /* LUT_NXP_READ */
    uint32_t intr, id, ahb;

    wr(q, FSPI_MCR0, 0x1);                   /* software reset */
    wr(q, FSPI_LUTKEY, LUTKEY_VAL);
    wr(q, FSPI_LCKCR, 0x2);                   /* unlock LUT */
    wr(q, FSPI_LUT, ((uint32_t)rd_i << 16) | cmd);   /* seqid 0 */
    wr(q, FSPI_LCKCR, 0x1);                   /* lock LUT */

    wr(q, FSPI_IPCR0, 0);                     /* address 0 */
    wr(q, FSPI_IPCR1, (0 << 16) | 3);         /* seqid 0, 3 data bytes */
    wr(q, FSPI_IPRXFCR, 0x1);                 /* clear rx fifo */
    wr(q, FSPI_IPCMD, 0x1);                   /* trigger */

    intr = rd(q, FSPI_INTR);
    g_assert_cmphex(intr & 0x1, ==, 0x1);     /* IPCMDDONE */

    id = rd(q, FSPI_RFDR) & 0xffffff;
    g_assert_cmphex(id, ==, IS25WP064_JEDEC);

    /* AHB-mapped read of an erased flash returns 0xff bytes. */
    ahb = qtest_readl(q, FSPI_AHB);
    g_assert_cmphex(ahb, ==, 0xffffffff);

    qtest_quit(q);
}

#define GD5F_READID 0x68a4c8           /* c8 (Gigadevice) a4 68, packed LE */

static void test_nand_read_id(void)
{
    QTestState *q = qtest_init(
        "-machine imx91-11x11-evk,flexspi-flash=gd5f4gq4 "
        "-m 4G -display none -kernel /dev/null");
    uint16_t cmd = (0x01 << 10) | 0x9f;     /* LUT_CMD, opcode 9Fh */
    uint16_t rd_i = (0x09 << 10) | 0x00;    /* LUT_NXP_READ */
    uint32_t id;

    wr(q, FSPI_MCR0, 0x1);                   /* software reset */
    wr(q, FSPI_LUTKEY, LUTKEY_VAL);
    wr(q, FSPI_LCKCR, 0x2);                   /* unlock LUT */
    wr(q, FSPI_LUT, ((uint32_t)rd_i << 16) | cmd);   /* seqid 0 */
    wr(q, FSPI_LCKCR, 0x1);                   /* lock LUT */

    wr(q, FSPI_IPCR0, 0);                     /* no address (OPCODE method) */
    wr(q, FSPI_IPCR1, (0 << 16) | 3);         /* seqid 0, 3 id bytes */
    wr(q, FSPI_IPRXFCR, 0x1);                 /* clear rx fifo */
    wr(q, FSPI_IPCMD, 0x1);                   /* trigger */

    g_assert_cmphex(rd(q, FSPI_INTR) & 0x1, ==, 0x1);   /* IPCMDDONE */
    id = rd(q, FSPI_RFDR) & 0xffffff;
    g_assert_cmphex(id, ==, GD5F_READID);

    qtest_quit(q);
}

/*
 * The DLL lock is EARNED, not seeded: spi-nxp-fspi.c enables the DLL (DLLACR/DLLBCR
 * bit 0 = DLLEN, with SLVDLY) then polls STS2 for the REF/SLV lock bits, warning
 * "DLL lock failed, please fix it!" and burning a 5 ms timeout if they never come.
 * So: at reset no lock bit is set; writing DLLEN to DLLACR earns the A-side lock;
 * writing it to DLLBCR earns the B-side lock.  If the model keys the lock off any
 * bit other than the one the driver writes (bit 0), the guest's enable never trips
 * it and the lock bits stay 0 -- which is exactly what this asserts against.
 */
static void test_dll_lock(void)
{
    QTestState *q = qtest_init("-machine imx91-11x11-evk -m 4G "
                               "-display none -kernel /dev/null");

    /* Reset: STS2 has the slave-delay selects but NONE of the lock bits. */
    g_assert_cmphex(rd(q, FSPI_STS2) & (STS2_A_LOCK | STS2_B_LOCK), ==, 0);

    /* Enable the A-side DLL the way the driver does: DLLEN | SLVDLY(0xF). */
    wr(q, FSPI_DLLACR, DLLACR_DLLEN | DLLACR_SLVDLY(0xF));
    g_assert_cmphex(rd(q, FSPI_STS2) & STS2_A_LOCK, ==, STS2_A_LOCK);
    /* B-side has not been enabled yet, so it must still read unlocked. */
    g_assert_cmphex(rd(q, FSPI_STS2) & STS2_B_LOCK, ==, 0);

    /* Enable the B-side DLL: now both banks report locked (AB_LOCK). */
    wr(q, FSPI_DLLBCR, DLLACR_DLLEN | DLLACR_SLVDLY(0xF));
    g_assert_cmphex(rd(q, FSPI_STS2) & (STS2_A_LOCK | STS2_B_LOCK),
                    ==, STS2_A_LOCK | STS2_B_LOCK);

    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("imx93/flexspi/read-id", test_read_id);
    qtest_add_func("imx91/flexspi/nand-read-id", test_nand_read_id);
    qtest_add_func("imx91/flexspi/dll-lock", test_dll_lock);
    return g_test_run();
}
