/*
 * NXP i.MX 93 ANATOP (Analog Top / PLL) module
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional model of the i.MX 93 ANATOP block (compatible "fsl,imx93-anatop"),
 * which hosts the fractional-N "GPPLL" PLLs that the Linux clk-imx93 /
 * clk-fracn-gppll driver programs directly (there is no System Manager on the
 * i.MX 93). The PLL outputs are COMPUTED from the registers the guest wrote,
 * using the same arithmetic as clk_fracn_gppll_recalc_rate(). A PLL that is not
 * powered up produces NOTHING -- and a consumer of nothing DOES NOT TICK.
 */

#ifndef IMX93_ANATOP_H
#define IMX93_ANATOP_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ANATOP "imx93.anatop"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93AnatopState, IMX93_ANATOP)

/* MMIO window (imx93.dtsi: clock-controller@44480000, reg size 0x2000). */
#define IMX93_ANATOP_REG_SIZE   (8 * KiB)
#define IMX93_ANATOP_NUM_REGS   (IMX93_ANATOP_REG_SIZE / 4)

/*
 * The fractional-N PLLs this block synthesises.
 *
 * SYS_PLL is deliberately absent. Its PFD outputs are FIXED on this SoC --
 * clk-imx93.c registers sys_pll_pfd0/1/2 as 1000 / 800 / 625 MHz constants --
 * so nothing derives a frequency from its registers. We do not model a rate
 * that no consumer reads.
 */
typedef enum {
    IMX93_PLL_ARM,
    IMX93_PLL_AUDIO,
    IMX93_PLL_VIDEO,
    IMX93_PLL_DRAM,
    IMX93_PLL__COUNT,
} IMX93AnatopPll;

struct IMX93AnatopState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_ANATOP_NUM_REGS];

    Clock *osc_in;                          /* the 24 MHz reference */
    Clock *pll_out[IMX93_PLL__COUNT];       /* computed, never asserted */
};

#endif /* IMX93_ANATOP_H */
