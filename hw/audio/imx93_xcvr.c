/*
 * NXP i.MX 93 Audio Transceiver (XCVR / SPDIF) - registration model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The control registers (at offset 0x800) use the i.MX SET/CLR/TOG alias
 * pattern: each register R is also writable at R+4 (set bits), R+8 (clear bits)
 * and R+0xC (toggle bits). The PHY/PLL sub-registers are reached through an
 * indirect "AI" interface (PHY_AI_CTRL/WDATA/RDATA): a toggle of the PLL/PHY
 * bit triggers an access the hardware acknowledges by setting the matching DONE
 * bit, which the driver polls. Modelling these lets the fsl_xcvr driver probe
 * and register its SPDIF card; the firmware-driven audio datapath is not run.
 */

#include "qemu/osdep.h"
#include "hw/audio/imx93_xcvr.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define XCVR_VERSION        0x00
#define XCVR_PHY_AI_CTRL    0x90
#define XCVR_PHY_AI_WDATA   0xa0
#define XCVR_PHY_AI_RDATA   0xa4

#define AI_CTRL_RWB         (1u << 31)
#define AI_TOG_PLL          (1u << 24)
#define AI_DONE_PLL         (1u << 25)
#define AI_TOG_PHY          (1u << 26)
#define AI_DONE_PHY         (1u << 27)

#define XCVR_VERSION_VALUE  0x00010000

#define RFDR_FIFO 0x0c00
#define TFDR_FIFO 0x0e00

/* Perform the indirect PHY/PLL access and acknowledge it via the DONE bits. */
static void xcvr_ai_complete(IMX93XcvrState *s)
{
    uint32_t ctrl = s->regs[XCVR_PHY_AI_CTRL >> 2];
    uint8_t addr = ctrl & 0xff;

    if (ctrl & AI_CTRL_RWB) {                       /* read */
        s->regs[XCVR_PHY_AI_RDATA >> 2] = s->ai_sub[addr];
    } else {                                        /* write */
        s->ai_sub[addr] = s->regs[XCVR_PHY_AI_WDATA >> 2];
    }
    /* DONE bit follows the TOG bit so the driver's poll completes. */
    ctrl &= ~(AI_DONE_PLL | AI_DONE_PHY);
    ctrl |= (ctrl & AI_TOG_PLL) ? AI_DONE_PLL : 0;
    ctrl |= (ctrl & AI_TOG_PHY) ? AI_DONE_PHY : 0;
    s->regs[XCVR_PHY_AI_CTRL >> 2] = ctrl;
}

static uint64_t xcvr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93XcvrState *s = opaque;
    uint32_t rel, sub;

    if (offset < IMX93_XCVR_REG_OFF) {
        uint32_t v = 0, b;

        for (b = 0; b < size && offset + b < IMX93_XCVR_RAM_SIZE; b++) {
            v |= (uint32_t)s->ram[offset + b] << (b * 8);
        }
        return v;
    }
    if (offset == RFDR_FIFO || offset == TFDR_FIFO) {
        return 0;
    }
    rel = offset - IMX93_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX93_XCVR_NUM_REGS) {
        return 0;
    }
    sub = rel & 0xf;
    /* SET/CLR/TOG aliases read back as the base register (except AI RDATA). */
    if (rel != XCVR_PHY_AI_RDATA && rel != XCVR_PHY_AI_WDATA &&
        (sub == 4 || sub == 8 || sub == 0xc)) {
        return s->regs[(rel & ~0xf) >> 2];
    }
    return s->regs[rel >> 2];
}

static void xcvr_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    IMX93XcvrState *s = opaque;
    uint32_t rel, sub, base;

    if (offset < IMX93_XCVR_REG_OFF) {
        uint32_t b;

        for (b = 0; b < size && offset + b < IMX93_XCVR_RAM_SIZE; b++) {
            s->ram[offset + b] = (value >> (b * 8)) & 0xff;
        }
        return;
    }
    if (offset == RFDR_FIFO || offset == TFDR_FIFO) {
        return;
    }
    rel = offset - IMX93_XCVR_REG_OFF;
    if ((rel >> 2) >= IMX93_XCVR_NUM_REGS) {
        return;
    }

    /* WDATA/RDATA are distinct registers, not SET/CLR aliases. */
    if (rel == XCVR_PHY_AI_WDATA || rel == XCVR_PHY_AI_RDATA) {
        s->regs[rel >> 2] = value;
        return;
    }

    sub = rel & 0xf;
    base = (rel & ~0xf) >> 2;
    switch (sub) {
    case 4:
        s->regs[base] |= value;     /* SET */
        break;
    case 8:
        s->regs[base] &= ~value;    /* CLR */
        break;
    case 0xc:
        s->regs[base] ^= value;     /* TOG */
        break;
    default:
        s->regs[rel >> 2] = value;
        base = rel >> 2;
        break;
    }

    /* An AI toggle triggers the indirect access. */
    if (base == (XCVR_PHY_AI_CTRL >> 2)) {
        xcvr_ai_complete(s);
    }
}

static const MemoryRegionOps xcvr_ops = {
    .read = xcvr_read,
    .write = xcvr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void xcvr_reset(DeviceState *dev)
{
    IMX93XcvrState *s = IMX93_XCVR(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ram, 0, sizeof(s->ram));
    memset(s->ai_sub, 0, sizeof(s->ai_sub));
    s->regs[XCVR_VERSION >> 2] = XCVR_VERSION_VALUE;
}

static void xcvr_realize(DeviceState *dev, Error **errp)
{
    IMX93XcvrState *s = IMX93_XCVR(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &xcvr_ops, s,
                          TYPE_IMX93_XCVR, IMX93_XCVR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_xcvr = {
    .name = TYPE_IMX93_XCVR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93XcvrState, IMX93_XCVR_NUM_REGS),
        VMSTATE_UINT8_ARRAY(ram, IMX93XcvrState, IMX93_XCVR_RAM_SIZE),
        VMSTATE_UINT32_ARRAY(ai_sub, IMX93XcvrState, 256),
        VMSTATE_END_OF_LIST()
    },
};

static void xcvr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = xcvr_realize;
    dc->vmsd = &vmstate_xcvr;
    device_class_set_legacy_reset(dc, xcvr_reset);
    dc->desc = "i.MX93 audio transceiver (SPDIF)";
}

static const TypeInfo xcvr_types[] = {
    {
        .name = TYPE_IMX93_XCVR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93XcvrState),
        .class_init = xcvr_class_init,
    },
};

DEFINE_TYPES(xcvr_types)
