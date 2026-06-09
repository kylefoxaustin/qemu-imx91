/*
 * NXP i.MX 9 DDR controller + DDR performance monitor (register compat)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The memory-controller@4e300000 ("nxp,imx9-memory-controller", simple-mfd)
 * contains the DDR PMU (ddr-pmu@4e300dc0, "fsl,imx93-ddr-pmu"), which the Linux
 * fsl_imx9_ddr_perf driver registers as a perf PMU so userspace can run
 * `perf stat -e imx9_ddr_pmu_.../...`.
 *
 * IMPORTANT: this is a register/perf-interface COMPATIBILITY model, not a
 * measurement. QEMU's DRAM is plain host memory: CPU loads/stores reach it
 * through the TCG softmmu TLB, never crossing a memory controller we could
 * count, and there is no cache model, so real DDR bandwidth (post-cache-miss
 * traffic) has no meaning here. The block is read-what-you-write: the driver
 * configures and clears the counters to 0, nothing increments them, so every
 * count reads back 0 - honestly "no traffic measured" rather than a fiction.
 * The overflow interrupt is wired but never asserted (counters never roll).
 */

#include "qemu/osdep.h"
#include "hw/misc/imx9_ddrc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

static uint64_t imx9_ddrc_read(void *opaque, hwaddr offset, unsigned size)
{
    Imx9DdrcState *s = opaque;

    if ((offset >> 2) >= IMX9_DDRC_NUM_REGS) {
        return 0;
    }
    return s->regs[offset >> 2];
}

static void imx9_ddrc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    Imx9DdrcState *s = opaque;

    if ((offset >> 2) < IMX9_DDRC_NUM_REGS) {
        s->regs[offset >> 2] = value;
    }
}

static const MemoryRegionOps imx9_ddrc_ops = {
    .read = imx9_ddrc_read,
    .write = imx9_ddrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx9_ddrc_reset(DeviceState *dev)
{
    Imx9DdrcState *s = IMX9_DDRC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void imx9_ddrc_realize(DeviceState *dev, Error **errp)
{
    Imx9DdrcState *s = IMX9_DDRC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx9_ddrc_ops, s,
                          TYPE_IMX9_DDRC, IMX9_DDRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx9_ddrc = {
    .name = TYPE_IMX9_DDRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Imx9DdrcState, IMX9_DDRC_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx9_ddrc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx9_ddrc_realize;
    dc->vmsd = &vmstate_imx9_ddrc;
    device_class_set_legacy_reset(dc, imx9_ddrc_reset);
    dc->desc = "i.MX9 DDR controller + DDR PMU (register compat)";
}

static const TypeInfo imx9_ddrc_types[] = {
    {
        .name          = TYPE_IMX9_DDRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Imx9DdrcState),
        .class_init    = imx9_ddrc_class_init,
    },
};

DEFINE_TYPES(imx9_ddrc_types)
