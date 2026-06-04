/*
 * NXP i.MX 93 PXP (Pixel Pipeline) — minimal reset model
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The PXP 2D engine is not functionally modeled, but its driver's
 * pxp_soft_reset() spins in an UNBOUNDED loop waiting for the MXS-style
 * HW_PXP_CTRL.CLKGATE bit to assert after software reset:
 *
 *     writel(SFTRST, CTRL_SET);
 *     while (!(readl(CTRL) & CLKGATE)) ;   // no timeout
 *
 * A plain return-0 stub hangs here forever. This model implements just the
 * HW_PXP_CTRL register with its SET/CLR/TOG aliases and the hardware
 * behaviour that asserting SFTRST also asserts CLKGATE, so the reset poll
 * completes. All other registers are read-what-you-write backing store.
 */

#ifndef IMX93_PXP_H
#define IMX93_PXP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_PXP "imx93.pxp"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93PxpState, IMX93_PXP)

#define IMX93_PXP_REG_SIZE      (64 * KiB)
#define IMX93_PXP_NUM_REGS      (IMX93_PXP_REG_SIZE / 4)

struct IMX93PxpState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t ctrl;
    uint32_t regs[IMX93_PXP_NUM_REGS];
};

#endif /* IMX93_PXP_H */
