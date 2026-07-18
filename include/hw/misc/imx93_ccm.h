/*
 * NXP i.MX 93 Clock Control Module (CCM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX 93 has no System Manager, so Linux programs the CCM directly
 * (compatible "fsl,imx93-ccm"). This models the i.MX 9 "clock root + LPCG" CCM
 * as a REAL CLOCK TREE, not a register file wearing its name: each root's
 * frequency is COMPUTED from the mux and divider the guest programmed,
 *
 *     root_hz = source(sel[slice], CONTROL.MUX) / (CONTROL.DIV + 1)
 *
 * and handed to consumers as a Clock. It used to produce no frequencies at all,
 * while the timers hardcoded 24 MHz and could not follow the tree even in
 * principle. The two agreed -- and they agreed because BOTH were fabricated.
 */

#ifndef IMX93_CCM_H
#define IMX93_CCM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/misc/imx93_ccm_roots.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_CCM "imx93.ccm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93CCMState, IMX93_CCM)

/* MMIO window (imx93.dtsi: clock-controller@44450000, reg size 0x10000). */
#define IMX93_CCM_REG_SIZE      (64 * KiB)
#define IMX93_CCM_NUM_REGS      (IMX93_CCM_REG_SIZE / 4)

/* Modelled consumers whose LPCG gate actually stops them (imx93_ccm_gated[]). */
#define IMX93_CCM_NUM_GATED     8

struct IMX93CCMState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[IMX93_CCM_NUM_REGS];

    /*
     * Inputs: the 24 MHz reference, and the four PLLs the ANATOP computes from
     * the registers the guest wrote.  Outputs: one clock per CLOCK_ROOT slice.
     *
     * A slice the RM documents but Linux never registers has NO SOURCE.  A root
     * that is OFF has no frequency.  A root whose source is undriven (clk_ext1)
     * has no frequency.  In every one of those cases the output is ZERO, and a
     * consumer of zero DOES NOT TICK -- it does not fall back to a default.
     *
     *     WHERE THE SOURCE IS ABSENT, THE MODEL MUST EXPOSE THAT, NOT ABSORB IT.
     *     A stopped clock gets diagnosed in a minute.  A plausible clock ships.
     */
    Clock *osc_in;
    Clock *pll_in[IMX93_PLL__COUNT];
    Clock *root_out[IMX93_CCM_NUM_SLICES];

    /*
     * Per-consumer GATED outputs: the consumer's root, gated by its own LPCG
     * DIRECT bit.  A block whose gate the guest cleared reads 0 Hz and stops --
     * without this the LPCG was storage the gating never reached.
     */
    Clock *gated_out[IMX93_CCM_NUM_GATED];
};

/* Slice index of a root, for the SoC's wiring (CONTROL lives at slice * 0x80). */
#define IMX93_CCM_SLICE(off)    ((off) / 0x80)

#endif /* IMX93_CCM_H */
