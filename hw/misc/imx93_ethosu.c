/*
 * NXP i.MX 93 Arm Ethos-U65 microNPU (register-file bring-up model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the Ethos-U65 NPU APB registers the NXP M33 ethos firmware reads
 * during device init (ethosu_dev_init / ethosu_dev_soft_reset), so the firmware
 * accepts the part, resets it, and brings up the rpmsg-ethosu-channel. The
 * register contract was read out of the firmware's own disassembly:
 *   - NPU_REG_CONFIG[31:28] (product) must be 1 (Ethos-U65);
 *   - NPU_REG_STATUS bit 3 must read clear (reset complete / not faulted);
 *   - NPU_REG_PROT must reflect a secure + privileged master (bit0=priv,
 *     bit1=non-secure=0), which verify_access_state checks;
 *   - NPU_REG_ID is reported as capabilities (arch/version), not gated.
 * The command-stream processor (real inference) is intentionally not modelled.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_ethosu.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define ETHOSU_REG_ID       0x00
#define ETHOSU_REG_STATUS   0x04
#define ETHOSU_REG_RESET    0x0c
#define ETHOSU_REG_PROT     0x24
#define ETHOSU_REG_CONFIG   0x28

/*
 * ID: arch 1.0.6, product_major 1, version 0.0.0 - a plausible Ethos-U65
 * identity reported to the capabilities query (not gated by device init).
 */
#define ETHOSU_ID_VALUE     0x10061000
/* CONFIG: product=1 (U65, [31:28]), cmd_stream_version=1 ([7:4]), macs=8. */
#define ETHOSU_CONFIG_VALUE 0x10000018

static uint64_t imx93_ethosu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93EthosuState *s = opaque;

    switch (offset) {
    case ETHOSU_REG_ID:
        return ETHOSU_ID_VALUE;
    case ETHOSU_REG_STATUS:
        /* Idle: not running, no IRQ pending, bit 3 (reset/fault) clear. */
        return 0;
    case ETHOSU_REG_CONFIG:
        return ETHOSU_CONFIG_VALUE;
    default:
        if ((offset >> 2) >= IMX93_ETHOSU_REGS) {
            return 0;
        }
        return s->regs[offset >> 2];
    }
}

static void imx93_ethosu_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93EthosuState *s = opaque;

    switch (offset) {
    case ETHOSU_REG_ID:
    case ETHOSU_REG_STATUS:
    case ETHOSU_REG_CONFIG:
        return;     /* read-only identification/status */
    case ETHOSU_REG_RESET:
        /*
         * The firmware writes RESET with the requested access state in the low
         * bits ([0]=privileged, [1]=non-secure) and then reads PROT back to
         * confirm the NPU "switched" to it. Mirror those bits into PROT.
         */
        s->regs[ETHOSU_REG_PROT >> 2] = value & 0x3;
        s->regs[ETHOSU_REG_RESET >> 2] = value;
        return;
    default:
        if ((offset >> 2) < IMX93_ETHOSU_REGS) {
            s->regs[offset >> 2] = value;
        }
    }
}

static const MemoryRegionOps imx93_ethosu_ops = {
    .read = imx93_ethosu_read,
    .write = imx93_ethosu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_ethosu_reset(DeviceState *dev)
{
    IMX93EthosuState *s = IMX93_ETHOSU(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void imx93_ethosu_realize(DeviceState *dev, Error **errp)
{
    IMX93EthosuState *s = IMX93_ETHOSU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx93_ethosu_ops, s,
                          TYPE_IMX93_ETHOSU, IMX93_ETHOSU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_imx93_ethosu = {
    .name = TYPE_IMX93_ETHOSU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93EthosuState, IMX93_ETHOSU_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_ethosu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = imx93_ethosu_realize;
    dc->vmsd = &vmstate_imx93_ethosu;
    device_class_set_legacy_reset(dc, imx93_ethosu_reset);
    dc->desc = "i.MX93 Arm Ethos-U65 microNPU (bring-up model)";
}

static const TypeInfo imx93_ethosu_types[] = {
    {
        .name = TYPE_IMX93_ETHOSU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93EthosuState),
        .class_init = imx93_ethosu_class_init,
    },
};

DEFINE_TYPES(imx93_ethosu_types)
