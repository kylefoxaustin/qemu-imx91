/*
 * NXP i.MX 93 FlexIO - configurable I/O block
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The FlexIO is a programmable shifter/timer fabric the i.MX93 EVK uses (with
 * the imx93-11x11-evk-flexio-i2c device tree) as an extra I2C master via the
 * nxp,imx-flexio MFD + i2c-flexio drivers. This model carries the register
 * file those drivers program: VERID/PARAM identification, the CTRL register
 * (with self-clearing software reset), and the shifter/timer configuration and
 * status registers. That is enough for the MFD to probe and the i2c-flexio
 * adapter to register, so a custom board/DTB can route an I2C bus through the
 * FlexIO. The shifter/timer engine is not driven to synthesise live I2C
 * transactions yet; set FLEXIO_DBG to trace the register sequence the driver
 * issues (the basis for a future functional datapath).
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_flexio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (per the imx-flexio MFD driver). */
#define FLEXIO_VERID            0x00
#define FLEXIO_PARAM            0x04
#define FLEXIO_CTRL             0x08
#define   FLEXIO_CTRL_SWRST     0x2     /* software reset (self-clearing) */
#define FLEXIO_SHIFTSTAT        0x10
#define FLEXIO_SHIFTERR         0x14
#define FLEXIO_TIMSTAT          0x18

/*
 * VERID: a plausible FlexIO version (major 2, minor 1). PARAM advertises the
 * resource counts the fabric provides: TRIGGER[31:24]=4, PIN[23:16]=32,
 * TIMER[15:8]=8, SHIFTER[7:0]=8. The i2c-flexio driver uses fixed shifter and
 * timer indices (0/1), so only the presence of the block matters here.
 */
#define FLEXIO_VERID_VALUE      0x02010000
#define FLEXIO_PARAM_VALUE      0x04200808

static void imx93_flexio_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (getenv("FLEXIO_DBG")) {
        fprintf(stderr, "[flexio] %s +0x%03x = 0x%08x\n", op,
                (unsigned)offset, value);
    }
}

static uint64_t imx93_flexio_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93FlexioState *s = opaque;
    uint32_t val;

    switch (offset) {
    case FLEXIO_VERID:
        val = FLEXIO_VERID_VALUE;
        break;
    case FLEXIO_PARAM:
        val = FLEXIO_PARAM_VALUE;
        break;
    default:
        val = (offset / 4) < IMX93_FLEXIO_NUM_REGS ? s->regs[offset / 4] : 0;
        break;
    }

    imx93_flexio_trace("rd", offset, val);
    return val;
}

static void imx93_flexio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93FlexioState *s = opaque;

    imx93_flexio_trace("wr", offset, (uint32_t)value);

    switch (offset) {
    case FLEXIO_VERID:
    case FLEXIO_PARAM:
        /* Read-only identification registers. */
        return;
    case FLEXIO_CTRL:
        /* Software reset is momentary: clear the fabric, never latch SWRST. */
        if (value & FLEXIO_CTRL_SWRST) {
            memset(s->regs, 0, sizeof(s->regs));
            value &= ~(uint64_t)FLEXIO_CTRL_SWRST;
        }
        break;
    default:
        break;
    }

    if ((offset / 4) < IMX93_FLEXIO_NUM_REGS) {
        s->regs[offset / 4] = value;
    }
}

static const MemoryRegionOps imx93_flexio_ops = {
    .read = imx93_flexio_read,
    .write = imx93_flexio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_flexio_reset(DeviceState *dev)
{
    IMX93FlexioState *s = IMX93_FLEXIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void imx93_flexio_init(Object *obj)
{
    IMX93FlexioState *s = IMX93_FLEXIO(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_flexio_ops, s,
                          TYPE_IMX93_FLEXIO, IMX93_FLEXIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_imx93_flexio = {
    .name = TYPE_IMX93_FLEXIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93FlexioState, IMX93_FLEXIO_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_flexio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 FlexIO";
    device_class_set_legacy_reset(dc, imx93_flexio_reset);
    dc->vmsd = &vmstate_imx93_flexio;
}

static const TypeInfo imx93_flexio_types[] = {
    {
        .name           = TYPE_IMX93_FLEXIO,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93FlexioState),
        .instance_init  = imx93_flexio_init,
        .class_init     = imx93_flexio_class_init,
    },
};

DEFINE_TYPES(imx93_flexio_types)
