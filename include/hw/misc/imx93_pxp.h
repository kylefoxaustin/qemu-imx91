/*
 * NXP i.MX 93 PXP (Pixel Pipeline) 2D engine
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 93 PXP uses the Fetch-Engine / Store-Engine architecture (per-channel
 * INPUT_FETCH_* source and INPUT_STORE_* dest registers), with the legacy
 * MXS-style HW_PXP_CTRL at offset 0 retained for soft reset. The pxp_dma_v3
 * driver's pxp_soft_reset() spins in an UNBOUNDED loop waiting for HW_PXP_CTRL
 * CLKGATE to assert after software reset:
 *
 *     writel(SFTRST, CTRL_SET);
 *     while (!(readl(CTRL) & CLKGATE)) ;   // no timeout
 *
 * so that behaviour (asserting SFTRST also asserts CLKGATE) is modelled. All
 * registers are a read-what-you-write backing store; set the PXP_DBG environment
 * variable to trace every register access (used to capture the driver's exact
 * programming sequence while bringing up the blit datapath). The completion IRQ
 * (WAKEUPMIX PXP interrupt 0, GIC SPI 173) is wired but only raised once the
 * blit datapath drives it.
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
    qemu_irq irq;               /* WAKEUPMIX PXP interrupt 0 (completion) */
    uint32_t op_mode;           /* last-armed op: 0 copy, 1 fill, 2 fetch->store blit */
    bool blend_pending;         /* CH1 source armed -> next kick is a src-over blend */
    uint32_t ctrl;
    uint32_t stat;
    uint32_t regs[IMX93_PXP_NUM_REGS];
};

#endif /* IMX93_PXP_H */
