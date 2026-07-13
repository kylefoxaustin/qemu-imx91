/*
 * NXP i.MX 91 Temperature Monitor (u_temp_anamix / "fsl,imx91-tmu")
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_IMX91_TMU_H
#define HW_MISC_IMX91_TMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX91_TMU "imx91.tmu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX91TmuState, IMX91_TMU)

#define IMX91_TMU_REG_SIZE  (4 * KiB)
#define IMX91_TMU_NUM_REGS  (IMX91_TMU_REG_SIZE / 4)

struct IMX91TmuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX91_TMU_NUM_REGS];

    /* Reported junction temperature, in millidegrees C. */
    int32_t temperature;
};

#endif /* HW_MISC_IMX91_TMU_H */
