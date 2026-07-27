/*
 * NXP i.MX 93 FlexSPI controller (serial NOR flash)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two command paths drive an attached SPI-NOR flash over a QEMU SSI bus:
 *
 *  - IP commands: the driver programs a sequence into the LUT (each seqid is
 *    four 32-bit words = eight 16-bit instructions: opcode[15:10], pads[9:8],
 *    operand[7:0]), sets IPCR0 (address) / IPCR1 (seqid + data size), and
 *    triggers via IPCMD. We interpret the sequence (CMD/ADDR/DUMMY/READ/WRITE/
 *    STOP), shifting bytes to the flash with chip-select asserted, fill RFDR on
 *    reads, drain TFDR on writes, and raise IPCMDDONE.
 *
 *  - AHB reads: the memory-mapped flash window issues a normal read (03h) to
 *    the flash for the accessed offset, so memory-mapped (XIP) reads return
 *    content.
 */

#include "qemu/osdep.h"
#include "hw/ssi/imx93_flexspi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define FSPI_MCR0       0x00
#define FSPI_AHBCR      0x0c
#define FSPI_AHBRXBUF0CR0 0x20   /* .. AHBRXBUF7CR0 at 0x3c, 4 bytes apart */
#define FSPI_FLSHA1CR1  0x70
#define FSPI_FLSHA2CR1  0x74
#define FSPI_FLSHB1CR1  0x78
#define FSPI_FLSHB2CR1  0x7c
#define FSPI_FLSHCR4    0x94
#define FSPI_DLLACR     0xc0
#define FSPI_DLLBCR     0xc4
#define FSPI_MCR2       0x08
#define FSPI_STS2       0xe8
#define FSPI_MCR0_SWRST (1u << 0)
#define FSPI_INTEN      0x10
#define FSPI_INTR       0x14
#define FSPI_INTR_IPCMDDONE (1u << 0)
#define FSPI_INTR_IPRXWA    (1u << 5)   /* IP RX FIFO watermark available */
#define FSPI_INTR_IPTXWE    (1u << 6)   /* IP TX FIFO watermark empty     */
#define FSPI_LUTKEY     0x18
#define FSPI_LUTKEY_VAL 0x5af05af0

/*
 * Reset values from IMX91RM.pdf rev 5, asserted by tests/imx91-reset-values.
 * All read-what-you-write config; the guest read ZERO from every one of them.
 */
#define FSPI_MCR0_RESET     0xffff80c2
/*
 * ⭐ MCR1 IS ALL-ONES AT RESET, AND IT IS NOT A DECORATION.
 *
 * MCR1 holds SEQWAIT and AHBBUSWAIT -- the sequencer and AHB bus TIMEOUTS -- and the
 * silicon comes up with both at their MAXIMUM (FFFF each).  We reset them to ZERO,
 * which on a timeout register does not mean "unset":
 *
 *     ZERO IS A LEGAL, MEANINGFUL VALUE.  IT MEANS TIME OUT IMMEDIATELY.
 *                                                    -- rt1180emulator's rule
 *
 * And the driver read-modify-writes MCR1 around its own settings, so it laundered
 * our zero back as if it had chosen a zero timeout.
 */
#define FSPI_MCR1           0x04
#define FSPI_MCR1_RESET     0xffffffff
#define FSPI_AHBCR_RESET    0x00000018
#define FSPI_LUTCR_RESET    0x00000002
/*
 * ⭐ AHBRXBUFnCR0's RESET VALUE IS NOT A CONSTANT -- IT CARRIES THE BUFFER'S OWN INDEX.
 *
 * The RM gives 8000_0020h, 8001_0020h, 8002_0020h ... 8007_0020h: bits 19:16 are
 * MSTRID, and each AHB RX buffer comes out of reset owning ITS OWN master ID.  I
 * seeded the same constant into all eight.
 *
 * Buffer 0 MATCHED -- which is exactly why it survived.  A pattern that is correct at
 * index 0 tells you NOTHING about index n.  (This tree already learned that once, from
 * the eDMA stride bug where channels 0, 8, 16 ... all matched and the 56 in between
 * did not.  I wrote the rule down -- WHEN A GATE FINDS A BUG, FIND ITS SIBLINGS -- and
 * then fixed AHBRXBUF0CR0 and walked away from its seven siblings.)
 */
#define FSPI_AHBRX_RESET    0x80000020    /* | (n << 16): MSTRID varies per buffer */

#define FSPI_MCR2_RESET     0x200081f7
#define FSPI_STS2_RESET     0x01000100    /* slave-delay selects; NOT the lock bits */

/*
 * DLL lock status, produced by the DLL -- asserted only once the guest ENABLES it.
 * DLLEN is bit 0 of DLLACR/DLLBCR (spi-nxp-fspi.c: FSPI_DLLACR_DLLEN = BIT(0)); the
 * driver writes DLLEN|SLVDLY there and then polls STS2 for the lock.  Keying this off
 * any other bit means the guest's enable never earns the lock -- the driver would burn
 * its full 5 ms poll and print "DLL lock failed, please fix it!" on every DLL config.
 */
#define FSPI_DLLCR_DLLEN    (1u << 0)
#define FSPI_STS2_ASLVLOCK  (1u << 0)
#define FSPI_STS2_AREFLOCK  (1u << 1)
#define FSPI_STS2_BSLVLOCK  (1u << 16)
#define FSPI_STS2_BREFLOCK  (1u << 17)
#define FSPI_FLSHCR1_RESET  0x00000063    /* FLSHA1/A2/B1/B2CR1 */
#define FSPI_FLSHCR4_RESET  0x000000c3
#define FSPI_DLLCR_RESET    0x00000100    /* DLLACR / DLLBCR */

/*
 * ⚠ LUTKEY RESETS TO THE KEY ITSELF (5AF05AF0h), AND THAT IS A TRAP.
 *
 * The unlock used to be `LUTCR == 2 && regs[LUTKEY] == KEY`.  Seed LUTKEY with its
 * real reset value and that check passes WITHOUT THE GUEST EVER WRITING THE KEY --
 * the model would unlock its LUT on a bare LUTCR write.
 *
 *     A MODEL THAT IS TOO FORGIVING DOES NOT FAIL SAFE.  IT SHIPS THE BUG
 *     DOWNSTREAM.  A guest that forgets the key would PASS HERE and be undefined
 *     on silicon.                                              -- mcxn947qemu
 *
 * So the reset value is now correct AND the unlock still requires the key to be
 * WRITTEN, which is what firmware actually does and what a real unlock sequence is.
 * Fixing a reset value must not quietly relax a guardrail.
 */
#define FSPI_LCKCR      0x1c
#define FSPI_STS0       0xe0
#define FSPI_STS0_IDLE  0x3         /* ARB_IDLE | SEQ_IDLE */
#define FSPI_IPCR0      0xa0
#define FSPI_IPCR1      0xa4
#define FSPI_IPCMD      0xb0
#define FSPI_IPCMD_TRG  (1u << 0)
#define FSPI_IPRXFCR    0xb8
#define FSPI_IPTXFCR    0xbc
#define FSPI_FIFO_CLR   (1u << 0)
#define FSPI_RFDR       0x100
#define FSPI_TFDR       0x180
#define FSPI_LUT        0x200
#define FSPI_LUT_END    0x300

/* LUT instruction opcodes. */
#define LUT_STOP        0x00
#define LUT_CMD         0x01
#define LUT_ADDR        0x02
#define LUT_MODE        0x04
#define LUT_NXP_WRITE   0x08
#define LUT_NXP_READ    0x09
#define LUT_DUMMY       0x0c

#define FLASH_CMD_READ  0x03

static void flexspi_update_irq(IMX93FlexSpiState *s)
{
    uint32_t active = s->regs[FSPI_INTR >> 2] & s->regs[FSPI_INTEN >> 2];

    qemu_set_irq(s->irq, !!active);
}

static void flexspi_run_seq(IMX93FlexSpiState *s, int seqid)
{
    uint32_t addr = s->regs[FSPI_IPCR0 >> 2];
    uint32_t ipcr1 = s->regs[FSPI_IPCR1 >> 2];
    uint32_t datasz = ipcr1 & 0xffff;
    int i;

    qemu_set_irq(s->cs[0], 0);      /* assert chip-select */

    for (i = 0; i < 8; i++) {
        uint32_t word = s->regs[(FSPI_LUT + seqid * 16 + (i / 2) * 4) >> 2];
        uint16_t instr = (i & 1) ? (word >> 16) : (word & 0xffff);
        uint8_t opcode = (instr >> 10) & 0x3f;
        uint8_t operand = instr & 0xff;
        int n, b;

        switch (opcode) {
        case LUT_STOP:
            i = 8;
            break;
        case LUT_CMD:
            ssi_transfer(s->bus, operand);
            break;
        case LUT_ADDR:
            for (b = operand / 8 - 1; b >= 0; b--) {
                ssi_transfer(s->bus, (addr >> (b * 8)) & 0xff);
            }
            break;
        case LUT_DUMMY:
            for (n = 0; n < (operand + 7) / 8; n++) {
                ssi_transfer(s->bus, 0);
            }
            break;
        case LUT_MODE:
            ssi_transfer(s->bus, operand);
            break;
        case LUT_NXP_READ:
            for (n = 0; n < datasz; n++) {
                uint8_t rx = ssi_transfer(s->bus, 0);

                if (!fifo8_is_full(&s->rx)) {
                    fifo8_push(&s->rx, rx);
                }
            }
            break;
        case LUT_NXP_WRITE:
            for (n = 0; n < datasz; n++) {
                uint8_t tx = fifo8_is_empty(&s->tx) ? 0 : fifo8_pop(&s->tx);

                ssi_transfer(s->bus, tx);
            }
            break;
        default:
            break;
        }
    }

    qemu_set_irq(s->cs[0], 1);      /* deassert chip-select */

    s->regs[FSPI_INTR >> 2] |= FSPI_INTR_IPCMDDONE;
    flexspi_update_irq(s);
}

static uint64_t flexspi_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexSpiState *s = opaque;

    if (offset >= FSPI_RFDR && offset < FSPI_RFDR + 0x80) {
        uint32_t word = 0;
        int b;

        for (b = 0; b < 4; b++) {       /* RFDR packs 4 bytes, LSB first */
            if (!fifo8_is_empty(&s->rx)) {
                word |= (uint32_t)fifo8_pop(&s->rx) << (b * 8);
            }
        }
        return word;
    }
    switch (offset) {
    case FSPI_STS0:
        return FSPI_STS0_IDLE;      /* always idle (commands complete inline) */
    case FSPI_INTR:
        /*
         * Commands complete inline, so the IP FIFOs are never the bottleneck:
         * always report TX-watermark-empty and (when RX holds data) RX-
         * watermark-available, else the driver's per-chunk poll of these bits
         * times out and WARNs / mis-sequences a read (e.g. SPI-NAND config).
         */
        return s->regs[FSPI_INTR >> 2] | FSPI_INTR_IPTXWE |
               (fifo8_is_empty(&s->rx) ? 0 : FSPI_INTR_IPRXWA);
    default:
        if ((offset >> 2) >= IMX93_FLEXSPI_NUM_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

/*
 * ⭐ THE DLL LOCK BITS ARE STATUS PRODUCED BY A MECHANISM, SO THEY ARE COMPUTED, NOT
 *    SEEDED -- AND THE GUEST HAS BEEN COMPLAINING ABOUT THEM IN PLAIN ENGLISH.
 *
 * spi-nxp-fspi.c resets the DLL, enables it, then polls STS2 for REF/SLV lock:
 *
 *     ret = fspi_readl_poll_tout(f, iobase + FSPI_STS2, FSPI_STS2_AB_LOCK, ...);
 *     if (ret)
 *             dev_warn(f->dev, "DLL lock failed, please fix it!\n");
 *
 * We answered 0 forever, so the DLL never locked: the driver burned the full poll
 * timeout on every DLL configuration and then printed a warning ASKING US TO FIX IT.
 *
 * Note what the RM's STS2 reset value is NOT: 0100_0100h has bits 8 and 24 (the
 * slave-delay selects) and NONE of the lock bits.  The chip comes up UNLOCKED, and it
 * is right to -- a DLL that reports "locked" before you have switched it on is the
 * fabricated-ready bug.  So the reset value is seeded and the lock is EARNED.
 */
static void flexspi_dll_update(IMX93FlexSpiState *s)
{
    uint32_t sts2 = s->regs[FSPI_STS2 >> 2];

    sts2 &= ~(FSPI_STS2_ASLVLOCK | FSPI_STS2_AREFLOCK |
              FSPI_STS2_BSLVLOCK | FSPI_STS2_BREFLOCK);

    if (s->regs[FSPI_DLLACR >> 2] & FSPI_DLLCR_DLLEN) {
        sts2 |= FSPI_STS2_ASLVLOCK | FSPI_STS2_AREFLOCK;
    }
    if (s->regs[FSPI_DLLBCR >> 2] & FSPI_DLLCR_DLLEN) {
        sts2 |= FSPI_STS2_BSLVLOCK | FSPI_STS2_BREFLOCK;
    }
    s->regs[FSPI_STS2 >> 2] = sts2;
}

static void flexspi_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    IMX93FlexSpiState *s = opaque;

    if (offset >= FSPI_TFDR && offset < FSPI_TFDR + 0x80) {
        int b;

        for (b = 0; b < 4; b++) {       /* TFDR unpacks 4 bytes, LSB first */
            if (!fifo8_is_full(&s->tx)) {
                fifo8_push(&s->tx, (value >> (b * 8)) & 0xff);
            }
        }
        return;
    }
    if (offset >= FSPI_LUT && offset < FSPI_LUT_END) {
        if (s->lut_unlocked) {
            s->regs[offset >> 2] = value;
        }
        return;
    }

    switch (offset) {
    case FSPI_MCR0:
        if (value & FSPI_MCR0_SWRST) {
            fifo8_reset(&s->rx);
            fifo8_reset(&s->tx);
            value &= ~FSPI_MCR0_SWRST;
        }
        s->regs[FSPI_MCR0 >> 2] = value;
        break;
    case FSPI_LUTKEY:
        s->lutkey_written = (value == FSPI_LUTKEY_VAL);
        s->regs[FSPI_LUTKEY >> 2] = value;
        break;
    case FSPI_LCKCR:
        /*
         * Unlock requires the key to have been WRITTEN, then LCKCR=2 (unlock) /
         * 1 (lock).  Not merely "LUTKEY reads as the key" -- it resets to the key,
         * so that test would unlock on a bare LUTCR write and let a guest that
         * forgot the key pass here and be undefined on silicon.
         */
        if (value == 2 && s->lutkey_written) {
            s->lut_unlocked = true;
        } else if (value == 1) {
            s->lut_unlocked = false;
            s->lutkey_written = false;
        }
        break;
    case FSPI_INTR:
        s->regs[FSPI_INTR >> 2] &= ~value;      /* write-1-to-clear */
        flexspi_update_irq(s);
        break;
    case FSPI_IPRXFCR:
        if (value & FSPI_FIFO_CLR) {
            fifo8_reset(&s->rx);
        }
        break;
    case FSPI_IPTXFCR:
        if (value & FSPI_FIFO_CLR) {
            fifo8_reset(&s->tx);
        }
        break;
    case FSPI_IPCMD:
        if (value & FSPI_IPCMD_TRG) {
            int seqid = (s->regs[FSPI_IPCR1 >> 2] >> 16) & 0xff;

            flexspi_run_seq(s, seqid);
        }
        break;
    case FSPI_DLLACR:
    case FSPI_DLLBCR:
        s->regs[offset >> 2] = value;
        flexspi_dll_update(s);      /* the lock is EARNED, not seeded */
        break;

    case FSPI_STS2:
        break;                      /* read-only status */

    default:
        if ((offset >> 2) < IMX93_FLEXSPI_NUM_REGS) {
            s->regs[offset >> 2] = value;
        }
        break;
    }
}

static const MemoryRegionOps flexspi_ops = {
    .read = flexspi_read,
    .write = flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* AHB-mapped flash window: a memory-mapped read issues a 03h read. */
static uint64_t flexspi_ahb_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexSpiState *s = opaque;
    uint64_t val = 0;
    int b;

    qemu_set_irq(s->cs[0], 0);
    ssi_transfer(s->bus, FLASH_CMD_READ);
    for (b = 2; b >= 0; b--) {
        ssi_transfer(s->bus, (offset >> (b * 8)) & 0xff);
    }
    for (b = 0; b < size; b++) {
        val |= (uint64_t)ssi_transfer(s->bus, 0) << (b * 8);
    }
    qemu_set_irq(s->cs[0], 1);
    return val;
}

static void flexspi_ahb_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    /* XIP window is read-only; programming goes through the IP path. */
}

static const MemoryRegionOps flexspi_ahb_ops = {
    .read = flexspi_ahb_read,
    .write = flexspi_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * memcpy_fromio() bursts the AHB window with 8-byte loads, so accept up to
     * 8: a narrower cap makes QEMU reject the access as invalid (MEMTX_ERROR =
     * external abort) before the handler even runs - which is what broke the
     * SPI-NAND page read.
     */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void flexspi_reset(DeviceState *dev)
{
    IMX93FlexSpiState *s = IMX93_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));

    s->regs[FSPI_MCR0 >> 2]   = FSPI_MCR0_RESET;
    s->regs[FSPI_MCR1 >> 2]   = FSPI_MCR1_RESET;
    s->regs[FSPI_AHBCR >> 2]  = FSPI_AHBCR_RESET;
    s->regs[FSPI_LUTKEY >> 2] = FSPI_LUTKEY_VAL;
    s->regs[FSPI_LCKCR >> 2]  = FSPI_LUTCR_RESET;   /* the RM calls 0x1c LUTCR */
    for (int i = 0; i < 8; i++) {
        s->regs[(FSPI_AHBRXBUF0CR0 + i * 4) >> 2] = FSPI_AHBRX_RESET | (i << 16);
    }
    s->regs[FSPI_FLSHA1CR1 >> 2] = FSPI_FLSHCR1_RESET;
    s->regs[FSPI_FLSHA2CR1 >> 2] = FSPI_FLSHCR1_RESET;
    s->regs[FSPI_FLSHB1CR1 >> 2] = FSPI_FLSHCR1_RESET;
    s->regs[FSPI_FLSHB2CR1 >> 2] = FSPI_FLSHCR1_RESET;
    s->regs[FSPI_FLSHCR4 >> 2]   = FSPI_FLSHCR4_RESET;
    s->regs[FSPI_DLLACR >> 2]    = FSPI_DLLCR_RESET;
    s->regs[FSPI_DLLBCR >> 2]    = FSPI_DLLCR_RESET;
    s->regs[FSPI_MCR2 >> 2]      = FSPI_MCR2_RESET;
    s->regs[FSPI_STS2 >> 2]      = FSPI_STS2_RESET;

    s->lut_unlocked = false;
    s->lutkey_written = false;
    fifo8_reset(&s->rx);
    fifo8_reset(&s->tx);
}

static void flexspi_realize(DeviceState *dev, Error **errp)
{
    IMX93FlexSpiState *s = IMX93_FLEXSPI(dev);

    s->bus = ssi_create_bus(dev, "spi");
    qdev_init_gpio_out_named(dev, s->cs, "cs", IMX93_FLEXSPI_NUM_CS);
    fifo8_create(&s->rx, 512);
    fifo8_create(&s->tx, 512);

    memory_region_init_io(&s->iomem, OBJECT(dev), &flexspi_ops, s,
                          TYPE_IMX93_FLEXSPI, IMX93_FLEXSPI_REG_SIZE);
    memory_region_init_io(&s->ahb, OBJECT(dev), &flexspi_ahb_ops, s,
                          "flexspi-ahb", IMX93_FLEXSPI_AHB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ahb);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_flexspi = {
    .name = TYPE_IMX93_FLEXSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93FlexSpiState, IMX93_FLEXSPI_NUM_REGS),
        VMSTATE_BOOL(lut_unlocked, IMX93FlexSpiState),
        VMSTATE_FIFO8(rx, IMX93FlexSpiState),
        VMSTATE_FIFO8(tx, IMX93FlexSpiState),
        VMSTATE_END_OF_LIST()
    },
};

static void flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flexspi_realize;
    dc->vmsd = &vmstate_flexspi;
    device_class_set_legacy_reset(dc, flexspi_reset);
    dc->desc = "i.MX93 FlexSPI controller";
}

static const TypeInfo flexspi_types[] = {
    {
        .name = TYPE_IMX93_FLEXSPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93FlexSpiState),
        .class_init = flexspi_class_init,
    },
};

DEFINE_TYPES(flexspi_types)
