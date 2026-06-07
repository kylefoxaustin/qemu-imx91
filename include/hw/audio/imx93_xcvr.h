/*
 * NXP i.MX 93 Audio Transceiver (XCVR / SPDIF) - registration model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the XCVR control registers enough for the fsl_xcvr driver to probe and
 * register its SPDIF sound card. Like the SAI/MICFIL models, no real audio data
 * is moved (the firmware-driven datapath is not executed).
 */

#ifndef HW_AUDIO_IMX93_XCVR_H
#define HW_AUDIO_IMX93_XCVR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_XCVR "imx93.xcvr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93XcvrState, IMX93_XCVR)

#define IMX93_XCVR_SIZE     0x10000
#define IMX93_XCVR_REG_OFF  0x800            /* control registers start here */
#define IMX93_XCVR_NUM_REGS (0x400 / 4)      /* control register window */
#define IMX93_XCVR_RAM_SIZE 0x800            /* firmware RAM below the regs */

struct IMX93XcvrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX93_XCVR_NUM_REGS];
    uint8_t ram[IMX93_XCVR_RAM_SIZE];
    uint32_t ai_sub[256];   /* PHY/PLL sub-registers via the AI interface */
};

#endif /* HW_AUDIO_IMX93_XCVR_H */
