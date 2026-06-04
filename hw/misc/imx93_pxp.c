/*
 * NXP i.MX 93 PXP (Pixel Pipeline) — minimal reset model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Implements only enough of HW_PXP_CTRL for the Linux pxp_dma_v3 driver's
 * unbounded soft-reset poll to complete; see imx93_pxp.h.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_pxp.h"
#include "migration/vmstate.h"

/* HW_PXP_CTRL and its MXS SET/CLR/TOG aliases. */
#define HW_PXP_CTRL         0x00
#define HW_PXP_CTRL_SET     0x04
#define HW_PXP_CTRL_CLR     0x08
#define HW_PXP_CTRL_TOG     0x0C

#define BM_PXP_CTRL_SFTRST  0x80000000
#define BM_PXP_CTRL_CLKGATE 0x40000000

static uint64_t imx93_pxp_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93PxpState *s = opaque;

    if (offset == HW_PXP_CTRL) {
        /*
         * Hardware asserts CLKGATE whenever SFTRST is asserted; the driver
         * polls for this after writing SFTRST to CTRL_SET.
         */
        uint32_t val = s->ctrl;
        if (val & BM_PXP_CTRL_SFTRST) {
            val |= BM_PXP_CTRL_CLKGATE;
        }
        return val;
    }
    return s->regs[offset / 4];
}

static void imx93_pxp_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93PxpState *s = opaque;

    switch (offset) {
    case HW_PXP_CTRL:
        s->ctrl = value;
        break;
    case HW_PXP_CTRL_SET:
        s->ctrl |= value;
        break;
    case HW_PXP_CTRL_CLR:
        s->ctrl &= ~(uint32_t)value;
        break;
    case HW_PXP_CTRL_TOG:
        s->ctrl ^= value;
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps imx93_pxp_ops = {
    .read = imx93_pxp_read,
    .write = imx93_pxp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_pxp_reset(DeviceState *dev)
{
    IMX93PxpState *s = IMX93_PXP(dev);

    s->ctrl = 0;
    memset(s->regs, 0, sizeof(s->regs));
}

static void imx93_pxp_init(Object *obj)
{
    IMX93PxpState *s = IMX93_PXP(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_pxp_ops, s,
                          TYPE_IMX93_PXP, IMX93_PXP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_imx93_pxp = {
    .name = TYPE_IMX93_PXP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IMX93PxpState),
        VMSTATE_UINT32_ARRAY(regs, IMX93PxpState, IMX93_PXP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_pxp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 PXP (reset-only model)";
    device_class_set_legacy_reset(dc, imx93_pxp_reset);
    dc->vmsd = &vmstate_imx93_pxp;
}

static const TypeInfo imx93_pxp_types[] = {
    {
        .name           = TYPE_IMX93_PXP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93PxpState),
        .instance_init  = imx93_pxp_init,
        .class_init     = imx93_pxp_class_init,
    },
};

DEFINE_TYPES(imx93_pxp_types)
