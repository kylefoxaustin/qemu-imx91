/*
 * NXP i.MX 93 SoC definitions
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * Modeled on hw/arm/fsl-imx8mp.h (Bernhard Beschow) and the i.MX 95 port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Unlike the i.MX 95, the i.MX 93 has NO System Manager: Linux programs the
 * CCM / ANATOP / IOMUXC / SRC blocks directly (no SCMI/SM indirection), so
 * those regions must eventually be modeled functionally rather than served
 * by an SM firmware stub. v0.0.1 installs logging stubs for them.
 *
 * All base addresses and IRQ numbers below are taken from the i.MX 93 Linux
 * device tree (arch/arm64/boot/dts/freescale/imx93.dtsi) and cross-checked
 * against the i.MX 93 Reference Manual (IMX93RM).
 */

#ifndef FSL_IMX93_H
#define FSL_IMX93_H

#include "target/arm/cpu.h"
#include "hw/char/imx_lpuart.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/imx93_ccm.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_FSL_IMX93 "fsl-imx93"
OBJECT_DECLARE_SIMPLE_TYPE(FslImx93State, FSL_IMX93)

/*
 * Main DDR window. The i.MX 93 maps DRAM starting at 0x8000_0000. The
 * 11x11 EVK ships with 2 GiB LPDDR4X; allow up to 4 GiB for the larger
 * board variants.
 */
#define FSL_IMX93_RAM_START         0x80000000ULL
#define FSL_IMX93_RAM_SIZE_MAX      (4ULL * GiB)

/*
 * i.MX 93 application processor complex:
 *   - 2x Cortex-A55  (the main APUs we emulate here)
 *   - 1x Cortex-M33  (real-time / low-power domain - not modeled in v0.0.1;
 *                     note: the M33 is NOT a System Manager as on i.MX 95)
 * In v0.0.1 we only instantiate the A55 cluster.
 */
enum FslImx93Configuration {
    FSL_IMX93_NUM_A55_CPUS  = 2,
    FSL_IMX93_NUM_LPUARTS   = 8,    /* LPUART1..LPUART8 */
    FSL_IMX93_NUM_IRQS      = 320,  /* GICv3 SPI budget for v0.0.1 */
};

/*
 * Number of LPUART instances modeled so far. Silicon has eight; v0.0.2
 * wires the console block (LPUART1-3), with LPUART1 as the EVK console.
 */
#define FSL_IMX93_NUM_MODELED_LPUARTS   3

struct FslImx93State {
    SysBusDevice    parent_obj;

    ARMCPU          cpu[FSL_IMX93_NUM_A55_CPUS];
    GICv3State      gic;
    IMXLPUARTState  lpuart[FSL_IMX93_NUM_MODELED_LPUARTS];
    IMX93CCMState   ccm;
    IMX93AnatopState anatop;
    MemoryRegion    ocram;
};

/*
 * Memory map region identifiers. The actual addresses live in the memmap
 * table in fsl-imx93.c.
 */
enum FslImx93MemoryRegions {
    FSL_IMX93_RAM,

    /* GICv3 (i.MX 93 has no ITS in the base SoC) */
    FSL_IMX93_GIC_DIST,
    FSL_IMX93_GIC_REDIST,

    /* On-chip RAM */
    FSL_IMX93_OCRAM,

    /* LPUART console block (AON + Wakeup domains) */
    FSL_IMX93_LPUART1,
    FSL_IMX93_LPUART2,
    FSL_IMX93_LPUART3,

    /* Clock / reset / pinmux infrastructure (stubbed as unimplemented) */
    FSL_IMX93_CCM,
    FSL_IMX93_ANATOP,
    FSL_IMX93_IOMUXC,
    FSL_IMX93_SRC,

    /* BLK_CTRL / syscfg aggregates per power domain (stubbed) */
    FSL_IMX93_BLK_CTRL_AONMIX,
    FSL_IMX93_BLK_CTRL_WAKEUPMIX,
    FSL_IMX93_BLK_CTRL_DDRMIX,

    /* Messaging Unit (AONMIX MU1) */
    FSL_IMX93_MU1,

    /* System counter */
    FSL_IMX93_SYSCTR,

    /* Watchdogs (WDOG3/WDOG4 in the A55 domain) */
    FSL_IMX93_WDOG3,
    FSL_IMX93_WDOG4,

    /* Trusted Resource Domain Controller (stubbed) */
    FSL_IMX93_TRDC,

    FSL_IMX93_NUM_REGIONS,
};

/*
 * IRQ assignments (GIC SPI numbers, from imx93.dtsi). These are the
 * interrupt numbers as the device tree presents them; the model adds
 * GIC_INTERNAL internally.
 */
enum FslImx93Irqs {
    FSL_IMX93_LPUART1_IRQ   = 19,
    FSL_IMX93_LPUART2_IRQ   = 20,
    FSL_IMX93_LPUART3_IRQ   = 68,
};

#endif /* FSL_IMX93_H */
