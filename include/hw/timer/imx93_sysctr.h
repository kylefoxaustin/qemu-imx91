/*
 * NXP i.MX 93 System Counter (SYS_CTR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_IMX93_SYSCTR_H
#define HW_TIMER_IMX93_SYSCTR_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_IMX93_SYSCTR "imx93.sysctr"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SysctrState, IMX93_SYSCTR)

#define IMX93_SYSCTR_SIZE 0x30000

struct IMX93SysctrState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer cmp;

    uint32_t cmpcr;         /* compare control (enable / irq mask) */
    uint64_t cmpcv;         /* compare value (counter ticks) */

    /*
     * The counter's reference clock (the 24 MHz crystal, osc_24m).  The tick rate
     * AND the CNTFID0 register the guest divides by both come from HERE, so they
     * cannot disagree.  Before this, SYS_CTR_HZ and CNTFID0 were two hardcoded
     * 24 MHz constants that agreed only because both were fabricated -- change one
     * and a guest computing wall-clock as ticks/CNTFID0 gets the wrong time.
     */
    Clock *clk;
};

#endif /* HW_TIMER_IMX93_SYSCTR_H */
