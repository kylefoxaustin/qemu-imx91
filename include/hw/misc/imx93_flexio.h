/*
 * NXP i.MX 93 FlexIO - configurable I/O (used as an I2C master)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMX93_FLEXIO_H
#define IMX93_FLEXIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_FLEXIO "imx93.flexio"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93FlexioState, IMX93_FLEXIO)

#define IMX93_FLEXIO_SIZE       (64 * KiB)
/* Registers run from VERID (0x00) up to TIMCMP (0x500+); cover a bit past. */
#define IMX93_FLEXIO_NUM_REGS   (0x600 / 4)

struct IMX93FlexioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX93_FLEXIO_NUM_REGS];
};

#endif /* IMX93_FLEXIO_H */
