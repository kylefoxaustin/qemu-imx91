/*
 * NXP i.MX 9 DDR controller + DDR performance monitor (register compat)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX9_DDRC_H
#define HW_MISC_IMX9_DDRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX9_DDRC "imx9-ddrc"
OBJECT_DECLARE_SIMPLE_TYPE(Imx9DdrcState, IMX9_DDRC)

#define IMX9_DDRC_SIZE      0x2000
#define IMX9_DDRC_NUM_REGS  (IMX9_DDRC_SIZE / 4)

struct Imx9DdrcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX9_DDRC_NUM_REGS];
};

#endif /* HW_MISC_IMX9_DDRC_H */
