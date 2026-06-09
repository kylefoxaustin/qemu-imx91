/*
 * SPI-NAND flash (Gigadevice GD5F4GQ4RC, 4 Gbit) - SSI peripheral
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A SPI-NAND device on a QEMU SSI bus, enough for the Linux spi-nand stack to
 * detect the chip, read its status, and read/program/erase pages through a
 * FlexSPI (or any SSI) controller. The command set follows the SPINAND_*_OP
 * macros in include/linux/mtd/spinand.h: RESET (0xff), GET/SET_FEATURE
 * (0x0f/0x1f), READ_ID (0x9f), PAGE_READ-to-cache (0x13), READ_FROM_CACHE
 * (0x03/0x0b and the x2/x4/dual/quad-io variants), PROGRAM_LOAD (0x02/0x84 and
 * x4 0x32/0x34), PROGRAM_EXECUTE (0x10) and BLOCK_ERASE (0xd8). The FlexSPI
 * model drives one CS-delimited transaction per IP command, so CS deassert
 * resets the byte state machine (like m25p80).
 *
 * Storage is an optional block backend ('drive'); with none the chip reads as
 * erased (0xff) and program/erase are dropped, which is enough to enumerate and
 * read a blank device.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "system/block-backend.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qapi/error.h"

/* GD5F4GQ4RC: 4096-byte page + 256 OOB, 64 pages/block, 2048 blocks. */
#define NAND_PAGE       4096
#define NAND_OOB        256
#define NAND_PAGE_OOB   (NAND_PAGE + NAND_OOB)
#define NAND_PPB        64
#define NAND_BLOCKS     2048
#define NAND_PAGES      (NAND_PPB * NAND_BLOCKS)

#define MFR_GIGADEVICE  0xc8
#define DEV_ID0         0xa4
#define DEV_ID1         0x68

/* Feature register addresses. */
#define REG_BLOCK_LOCK  0xa0
#define REG_CFG         0xb0
#define REG_STATUS      0xc0
#define STATUS_BUSY     (1u << 0)

#define TYPE_SPI_NAND "gd5f4gq4"
OBJECT_DECLARE_SIMPLE_TYPE(SpiNandState, SPI_NAND)

struct SpiNandState {
    SSIPeripheral parent_obj;

    BlockBackend *blk;

    uint8_t cmd;                /* current command, 0 = awaiting command   */
    uint32_t pos;              /* bytes seen since the command byte        */
    uint32_t naddr;            /* address bytes this command consumes      */
    uint32_t ndummy;           /* dummy bytes after the address            */
    uint32_t addr;             /* accumulated address                      */
    uint32_t col;              /* column offset for cache read/write       */
    uint32_t row;              /* page address for read/program/erase      */
    bool wel;                  /* write-enable latch                       */

    uint8_t feature[256];      /* feature registers (status/cfg/lock)      */
    uint8_t cache[NAND_PAGE_OOB];
};

static void spi_nand_load_page(SpiNandState *s, uint32_t row)
{
    memset(s->cache, 0xff, sizeof(s->cache));
    if (s->blk && row < NAND_PAGES) {
        blk_pread(s->blk, (int64_t)row * NAND_PAGE_OOB, NAND_PAGE_OOB,
                  s->cache, 0);
    }
}

static void spi_nand_program_page(SpiNandState *s, uint32_t row)
{
    if (s->blk && (s->feature[REG_BLOCK_LOCK] == 0) && row < NAND_PAGES) {
        blk_pwrite(s->blk, (int64_t)row * NAND_PAGE_OOB, NAND_PAGE_OOB,
                   s->cache, 0);
    }
}

static void spi_nand_erase_block(SpiNandState *s, uint32_t row)
{
    uint32_t block = row / NAND_PPB;
    uint8_t erased[NAND_PAGE_OOB];
    uint32_t p;

    if (!s->blk || block >= NAND_BLOCKS) {
        return;
    }
    memset(erased, 0xff, sizeof(erased));
    for (p = 0; p < NAND_PPB; p++) {
        uint32_t page = block * NAND_PPB + p;
        blk_pwrite(s->blk, (int64_t)page * NAND_PAGE_OOB, NAND_PAGE_OOB,
                   erased, 0);
    }
}

/* Decode a command byte: how many address/dummy bytes follow it. */
static void spi_nand_begin_cmd(SpiNandState *s, uint8_t cmd)
{
    s->cmd = cmd;
    s->pos = 1;
    s->naddr = 0;
    s->ndummy = 0;
    s->addr = 0;

    switch (cmd) {
    case 0xff:                          /* RESET                            */
        s->feature[REG_STATUS] &= ~STATUS_BUSY;
        break;
    case 0x06:                          /* WRITE ENABLE                     */
        s->wel = true;
        break;
    case 0x04:                          /* WRITE DISABLE                    */
        s->wel = false;
        break;
    case 0x0f:                          /* GET FEATURE: 1 addr (reg)        */
    case 0x1f:                          /* SET FEATURE: 1 addr (reg)        */
        s->naddr = 1;
        break;
    case 0x9f:                          /* READ ID: no addr/dummy           */
        break;
    case 0x13:                          /* PAGE READ to cache: 3 addr (row) */
    case 0x10:                          /* PROGRAM EXECUTE: 3 addr (row)    */
    case 0xd8:                          /* BLOCK ERASE: 3 addr (row)        */
        s->naddr = 3;
        break;
    case 0x03:                          /* READ FROM CACHE (slow): 2 addr   */
        s->naddr = 2;
        break;
    case 0x0b:                          /* READ FROM CACHE (fast): 2 + 1dly */
    case 0x3b:                          /* x2                               */
    case 0x6b:                          /* x4                               */
    case 0xbb:                          /* dual-io                          */
    case 0xeb:                          /* quad-io                          */
        s->naddr = 2;
        s->ndummy = 1;
        break;
    case 0x02:                          /* PROGRAM LOAD: 2 addr (col)       */
    case 0x84:                          /* PROGRAM LOAD RANDOM              */
    case 0x32:                          /* PROGRAM LOAD x4                  */
    case 0x34:                          /* PROGRAM LOAD RANDOM x4           */
        s->naddr = 2;
        break;
    default:
        break;
    }
}

/* The address phase just finished: latch row/column and act where immediate. */
static void spi_nand_addr_done(SpiNandState *s)
{
    switch (s->cmd) {
    case 0x13:                          /* PAGE READ -> load cache          */
        s->row = s->addr & 0xffffff;
        spi_nand_load_page(s, s->row);
        break;
    case 0x10:                          /* PROGRAM EXECUTE                  */
        s->row = s->addr & 0xffffff;
        spi_nand_program_page(s, s->row);
        s->wel = false;
        break;
    case 0xd8:                          /* BLOCK ERASE                     */
        s->row = s->addr & 0xffffff;
        spi_nand_erase_block(s, s->row);
        s->wel = false;
        break;
    case 0x03: case 0x0b: case 0x3b:    /* READ FROM CACHE -> column       */
    case 0x6b: case 0xbb: case 0xeb:
    case 0x02: case 0x84: case 0x32: case 0x34:
        s->col = s->addr & 0xffff;
        break;
    default:
        break;
    }
}

static uint32_t spi_nand_transfer(SSIPeripheral *dev, uint32_t val)
{
    SpiNandState *s = SPI_NAND(dev);
    uint8_t out = 0xff;
    uint32_t apos;

    if (s->pos == 0) {
        spi_nand_begin_cmd(s, val & 0xff);
        return 0xff;
    }

    apos = s->pos - 1;                  /* bytes consumed after the command */

    if (apos < s->naddr) {              /* address phase                    */
        s->addr = (s->addr << 8) | (val & 0xff);
        s->pos++;
        if (apos + 1 == s->naddr) {
            spi_nand_addr_done(s);
        }
        return 0xff;
    }
    if (apos < s->naddr + s->ndummy) {  /* dummy phase                      */
        s->pos++;
        return 0xff;
    }

    /* data phase */
    switch (s->cmd) {
    case 0x9f:                          /* READ ID bytes                    */
        switch (apos) {
        case 0:
            out = MFR_GIGADEVICE;
            break;
        case 1:
            out = DEV_ID0;
            break;
        case 2:
            out = DEV_ID1;
            break;
        default:
            out = 0;
            break;
        }
        break;
    case 0x0f:                          /* GET FEATURE value                */
        out = s->feature[s->addr & 0xff];
        break;
    case 0x1f:                          /* SET FEATURE value                */
        s->feature[s->addr & 0xff] = val & 0xff;
        break;
    case 0x03: case 0x0b: case 0x3b:    /* READ FROM CACHE                  */
    case 0x6b: case 0xbb: case 0xeb:
        out = s->col < NAND_PAGE_OOB ? s->cache[s->col] : 0xff;
        s->col++;
        break;
    case 0x02: case 0x84: case 0x32: case 0x34:   /* PROGRAM LOAD           */
        if (s->col < NAND_PAGE_OOB) {
            s->cache[s->col] = val & 0xff;
        }
        s->col++;
        break;
    default:
        break;
    }

    s->pos++;
    return out;
}

static int spi_nand_set_cs(SSIPeripheral *dev, bool select)
{
    SpiNandState *s = SPI_NAND(dev);

    if (select) {                       /* CS deasserted: end transaction   */
        s->pos = 0;
        s->cmd = 0;
    }
    return 0;
}

static void spi_nand_realize(SSIPeripheral *dev, Error **errp)
{
    SpiNandState *s = SPI_NAND(dev);

    if (s->blk) {
        uint64_t need = (uint64_t)NAND_PAGES * NAND_PAGE_OOB;

        if (blk_getlength(s->blk) < (int64_t)need) {
            error_setg(errp, "spi-nand backing image too small "
                       "(need %" PRIu64 " bytes)", need);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
    }
    memset(s->feature, 0, sizeof(s->feature));
    memset(s->cache, 0xff, sizeof(s->cache));
    s->pos = 0;
    s->cmd = 0;
}

static const VMStateDescription vmstate_spi_nand = {
    .name = "spi-nand",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, SpiNandState),
        VMSTATE_UINT8(cmd, SpiNandState),
        VMSTATE_UINT32(pos, SpiNandState),
        VMSTATE_UINT32(naddr, SpiNandState),
        VMSTATE_UINT32(ndummy, SpiNandState),
        VMSTATE_UINT32(addr, SpiNandState),
        VMSTATE_UINT32(col, SpiNandState),
        VMSTATE_UINT32(row, SpiNandState),
        VMSTATE_BOOL(wel, SpiNandState),
        VMSTATE_UINT8_ARRAY(feature, SpiNandState, 256),
        VMSTATE_UINT8_ARRAY(cache, SpiNandState, NAND_PAGE_OOB),
        VMSTATE_END_OF_LIST()
    },
};

static const Property spi_nand_props[] = {
    DEFINE_PROP_DRIVE("drive", SpiNandState, blk),
};

static void spi_nand_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = spi_nand_realize;
    k->transfer = spi_nand_transfer;
    k->set_cs = spi_nand_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_spi_nand;
    dc->desc = "Gigadevice GD5F4GQ4 SPI-NAND flash";
    device_class_set_props(dc, spi_nand_props);
}

static const TypeInfo spi_nand_types[] = {
    {
        .name          = TYPE_SPI_NAND,
        .parent        = TYPE_SSI_PERIPHERAL,
        .instance_size = sizeof(SpiNandState),
        .class_init    = spi_nand_class_init,
    },
};

DEFINE_TYPES(spi_nand_types)
