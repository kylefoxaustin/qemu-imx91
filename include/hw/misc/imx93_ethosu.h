/*
 * NXP i.MX 93 Arm Ethos-U65 microNPU (register-file bring-up model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX93 Ethos-U65 NPU is driven by firmware on the Cortex-M33, not by
 * Linux directly. This models the NPU APB register block (base 0x4a900000)
 * enough for the NXP ethos M33 firmware's device init to accept the part and
 * complete a soft reset, so it brings up the "rpmsg-ethosu-channel" and Linux's
 * ethosu driver connects. The command-stream compute engine is NOT modelled -
 * no real inference is executed.
 */

#ifndef IMX93_ETHOSU_H
#define IMX93_ETHOSU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_ETHOSU "imx93.ethosu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93EthosuState, IMX93_ETHOSU)

#define IMX93_ETHOSU_SIZE   0x10000
#define IMX93_ETHOSU_REGS   (0x100 / 4)     /* APB control registers */

struct IMX93EthosuState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX93_ETHOSU_REGS];
};

#endif /* IMX93_ETHOSU_H */
