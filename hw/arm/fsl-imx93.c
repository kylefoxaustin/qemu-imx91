/*
 * NXP i.MX 93 SoC Implementation - v0.0.1 scaffold
 *
 * Modeled on hw/arm/fsl-imx8mp.c (Bernhard Beschow) and the i.MX 95 port.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope:
 *   - 2x Cortex-A55 cluster instantiated
 *   - GICv3 wired to both cores including timer PPIs
 *   - DDR mapped at 0x8000_0000, OCRAM at 0x2048_0000
 *   - All non-CPU/GIC peripherals are create_unimplemented_device() stubs
 *     so accesses log instead of faulting
 *   - No LPUART model yet (next step in v0.0.2)
 *
 * Addresses are taken from imx93.dtsi and the i.MX 93 RM. Unlike i.MX 95,
 * the i.MX 93 has no System Manager, so CCM/ANATOP/IOMUXC/SRC are stubbed
 * here only as a starting point - they must be modeled functionally before
 * Linux clock/pinmux bring-up will succeed.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fsl-imx93.h"
#include "hw/core/boards.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/kvm.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/kvm_arm.h"
#include "qapi/error.h"
#include "qobject/qlist.h"

/*
 * Single source of truth for the SoC memory map. Each entry maps a region
 * ID (from enum FslImx93MemoryRegions) to its physical base address, size,
 * and a debug name. Bases are confirmed against imx93.dtsi.
 */
static const struct {
    hwaddr      addr;
    size_t      size;
    const char *name;
} fsl_imx93_memmap[FSL_IMX93_NUM_REGIONS] = {
    [FSL_IMX93_RAM]                  = { FSL_IMX93_RAM_START, FSL_IMX93_RAM_SIZE_MAX, "ram" },

    /* GICv3: distributor + redistributor (imx93.dtsi gic@48000000). */
    [FSL_IMX93_GIC_DIST]             = { 0x48000000, 64 * KiB,   "gic_dist" },
    [FSL_IMX93_GIC_REDIST]           = { 0x48040000, 768 * KiB,  "gic_redist" },

    /* On-chip RAM. TODO: confirm exact OCRAM size from RM (512 KiB used). */
    [FSL_IMX93_OCRAM]                = { 0x20480000, 512 * KiB,  "ocram" },

    /* LPUART console block. lpuart1/2 in AONMIX, lpuart3 in WAKEUPMIX. */
    [FSL_IMX93_LPUART1]              = { 0x44380000, 64 * KiB,   "lpuart1" },
    [FSL_IMX93_LPUART2]              = { 0x44390000, 64 * KiB,   "lpuart2" },
    [FSL_IMX93_LPUART3]              = { 0x42570000, 64 * KiB,   "lpuart3" },
    [FSL_IMX93_LPUART4]              = { 0x42580000, 64 * KiB,   "lpuart4" },
    [FSL_IMX93_LPUART5]              = { 0x42590000, 64 * KiB,   "lpuart5" },
    [FSL_IMX93_LPUART6]              = { 0x425a0000, 64 * KiB,   "lpuart6" },
    [FSL_IMX93_LPUART7]              = { 0x42690000, 64 * KiB,   "lpuart7" },
    [FSL_IMX93_LPUART8]              = { 0x426a0000, 64 * KiB,   "lpuart8" },

    /* Clock / reset / pinmux (direct register programming - no SM). */
    [FSL_IMX93_CCM]                  = { 0x44450000, 64 * KiB,   "ccm" },
    [FSL_IMX93_ANATOP]               = { 0x44480000, 8 * KiB,    "anatop" },
    [FSL_IMX93_IOMUXC]               = { 0x443c0000, 64 * KiB,   "iomuxc" },
    [FSL_IMX93_SRC]                  = { 0x44460000, 64 * KiB,   "src" },

    /* BLK_CTRL / syscfg aggregates per power domain. */
    [FSL_IMX93_BLK_CTRL_AONMIX]      = { 0x44210000, 4 * KiB,    "blk_ctrl_aonmix" },
    [FSL_IMX93_BLK_CTRL_WAKEUPMIX]   = { 0x42420000, 4 * KiB,    "blk_ctrl_wakeupmix" },
    [FSL_IMX93_BLK_CTRL_DDRMIX]      = { 0x4e010000, 64 * KiB,   "blk_ctrl_ddrmix" },

    /* Ethernet: FEC (real imx.enet) + eQOS dwmac (stub). */
    [FSL_IMX93_FEC]                  = { 0x42890000, 64 * KiB,   "fec" },
    [FSL_IMX93_EQOS]                 = { 0x428a0000, 64 * KiB,   "eqos" },

    /* eDMA controllers (edma1 AONMIX, edma2 WAKEUPMIX). */
    [FSL_IMX93_EDMA1]                = { 0x44000000, 0x200000,   "edma1" },
    [FSL_IMX93_EDMA2]                = { 0x42000000, 0x210000,   "edma2" },

    /* Cortex-M33 remoteproc resource table region (M33 SRAM). */
    [FSL_IMX93_RSC_TABLE]            = { 0x2021e000, 4 * KiB,    "m33_rsc_table" },

    /* OCOTP / efuse syscon (FEC MAC-address nvmem cells live here). */
    [FSL_IMX93_OCOTP]                = { 0x47510000, 64 * KiB,   "ocotp" },

    /* Messaging Units (AONMIX MU1, WAKEUPMIX MU2, ELE/Sentinel S4 MU). */
    [FSL_IMX93_MU1]                  = { 0x44230000, 64 * KiB,   "mu1" },
    [FSL_IMX93_MU2]                  = { 0x42440000, 64 * KiB,   "mu2" },
    [FSL_IMX93_ELE_MU]               = { 0x47520000, 64 * KiB,   "ele_mu_s4" },

    /* System counter. */
    [FSL_IMX93_SYSCTR]               = { 0x44290000, 192 * KiB,  "sysctr" },

    /* Watchdogs (wdog1/2 AONMIX, wdog3/4/5 WAKEUPMIX). */
    [FSL_IMX93_WDOG1]                = { 0x442d0000, 64 * KiB,   "wdog1" },
    [FSL_IMX93_WDOG2]                = { 0x442e0000, 64 * KiB,   "wdog2" },
    [FSL_IMX93_WDOG3]                = { 0x42490000, 64 * KiB,   "wdog3" },
    [FSL_IMX93_WDOG4]                = { 0x424a0000, 64 * KiB,   "wdog4" },
    [FSL_IMX93_WDOG5]                = { 0x424b0000, 64 * KiB,   "wdog5" },

    /* Trusted Resource Domain Controller. */
    [FSL_IMX93_TRDC]                 = { 0x44270000, 64 * KiB,   "trdc" },

    /* Battery-Backed Non-Secure Module (RTC + power key). */
    [FSL_IMX93_BBNSM]                = { 0x44440000, 64 * KiB,   "bbnsm" },

    /* Thermal Management Unit. */
    [FSL_IMX93_TMU]                  = { 0x44482000, 4 * KiB,    "tmu" },

    /* ADC. */
    [FSL_IMX93_ADC1]                 = { 0x44530000, 64 * KiB,   "adc1" },

    /* uSDHC controllers (eMMC / SD / SDIO). */
    [FSL_IMX93_USDHC1]               = { 0x42850000, 64 * KiB,   "usdhc1" },
    [FSL_IMX93_USDHC2]               = { 0x42860000, 64 * KiB,   "usdhc2" },
    [FSL_IMX93_USDHC3]               = { 0x428b0000, 64 * KiB,   "usdhc3" },

    /* Low-speed I/O controllers (stubbed). */
    [FSL_IMX93_TPM1]                 = { 0x44310000, 64 * KiB,   "tpm1" },
    [FSL_IMX93_TPM2]                 = { 0x44320000, 64 * KiB,   "tpm2" },
    [FSL_IMX93_TPM3]                 = { 0x424e0000, 64 * KiB,   "tpm3" },
    [FSL_IMX93_TPM4]                 = { 0x424f0000, 64 * KiB,   "tpm4" },
    [FSL_IMX93_TPM5]                 = { 0x42500000, 64 * KiB,   "tpm5" },
    [FSL_IMX93_TPM6]                 = { 0x42510000, 64 * KiB,   "tpm6" },
    [FSL_IMX93_I3C1]                 = { 0x44330000, 64 * KiB,   "i3c1" },
    [FSL_IMX93_I3C2]                 = { 0x42520000, 64 * KiB,   "i3c2" },
    [FSL_IMX93_LPI2C1]               = { 0x44340000, 64 * KiB,   "lpi2c1" },
    [FSL_IMX93_LPI2C2]               = { 0x44350000, 64 * KiB,   "lpi2c2" },
    [FSL_IMX93_LPI2C3]               = { 0x42530000, 64 * KiB,   "lpi2c3" },
    [FSL_IMX93_LPI2C4]               = { 0x42540000, 64 * KiB,   "lpi2c4" },
    [FSL_IMX93_LPI2C5]               = { 0x426b0000, 64 * KiB,   "lpi2c5" },
    [FSL_IMX93_LPI2C6]               = { 0x426c0000, 64 * KiB,   "lpi2c6" },
    [FSL_IMX93_LPI2C7]               = { 0x426d0000, 64 * KiB,   "lpi2c7" },
    [FSL_IMX93_LPI2C8]               = { 0x426e0000, 64 * KiB,   "lpi2c8" },
    [FSL_IMX93_LPSPI1]               = { 0x44360000, 64 * KiB,   "lpspi1" },
    [FSL_IMX93_LPSPI2]               = { 0x44370000, 64 * KiB,   "lpspi2" },
    [FSL_IMX93_LPSPI3]               = { 0x42550000, 64 * KiB,   "lpspi3" },
    [FSL_IMX93_LPSPI4]               = { 0x42560000, 64 * KiB,   "lpspi4" },
    [FSL_IMX93_LPSPI5]               = { 0x426f0000, 64 * KiB,   "lpspi5" },
    [FSL_IMX93_LPSPI6]               = { 0x42700000, 64 * KiB,   "lpspi6" },
    [FSL_IMX93_LPSPI7]               = { 0x42710000, 64 * KiB,   "lpspi7" },
    [FSL_IMX93_LPSPI8]               = { 0x42720000, 64 * KiB,   "lpspi8" },
    [FSL_IMX93_FLEXCAN1]             = { 0x443a0000, 64 * KiB,   "flexcan1" },
    [FSL_IMX93_FLEXCAN2]             = { 0x425b0000, 64 * KiB,   "flexcan2" },
    [FSL_IMX93_SAI1]                 = { 0x443b0000, 64 * KiB,   "sai1" },
    [FSL_IMX93_SAI2]                 = { 0x42650000, 64 * KiB,   "sai2" },
    [FSL_IMX93_SAI3]                 = { 0x42660000, 64 * KiB,   "sai3" },
    [FSL_IMX93_MICFIL]               = { 0x44520000, 64 * KiB,   "micfil" },
    [FSL_IMX93_FLEXSPI1]             = { 0x425e0000, 64 * KiB,   "flexspi1" },
    [FSL_IMX93_XCVR]                 = { 0x42680000, 64 * KiB,   "xcvr" },
    [FSL_IMX93_GPIO1]                = { 0x47400000, 64 * KiB,   "gpio1" },
    [FSL_IMX93_GPIO2]                = { 0x43810000, 64 * KiB,   "gpio2" },
    [FSL_IMX93_GPIO3]                = { 0x43820000, 64 * KiB,   "gpio3" },
    [FSL_IMX93_GPIO4]                = { 0x43830000, 64 * KiB,   "gpio4" },
    [FSL_IMX93_USBOTG1]              = { 0x4c100000, 64 * KiB,   "usbotg1" },
    [FSL_IMX93_USBOTG2]              = { 0x4c200000, 64 * KiB,   "usbotg2" },

    /* MEDIAMIX: block control + imaging cluster (csi/dsi/pxp/lcdif/isi). */
    [FSL_IMX93_MEDIA_BLK_CTRL]       = { 0x4ac10000, 4 * KiB,    "media_blk_ctrl" },
    [FSL_IMX93_MIPI_CSI]             = { 0x4ae00000, 64 * KiB,   "mipi_csi" },
    [FSL_IMX93_DSI]                  = { 0x4ae10000, 64 * KiB,   "dsi" },
    [FSL_IMX93_PXP]                  = { 0x4ae20000, 64 * KiB,   "pxp" },
    [FSL_IMX93_LCDIF]                = { 0x4ae30000, 64 * KiB,   "lcdif" },
    [FSL_IMX93_ISI]                  = { 0x4ae40000, 64 * KiB,   "isi" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
static void fsl_imx93_install_unimplemented(FslImx93State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX93_IOMUXC, FSL_IMX93_SRC,
        FSL_IMX93_BLK_CTRL_AONMIX, FSL_IMX93_BLK_CTRL_WAKEUPMIX,
        FSL_IMX93_BLK_CTRL_DDRMIX,
        FSL_IMX93_EQOS,
        FSL_IMX93_EDMA1, FSL_IMX93_EDMA2, FSL_IMX93_RSC_TABLE, FSL_IMX93_OCOTP,
        FSL_IMX93_MU1, FSL_IMX93_MU2, FSL_IMX93_SYSCTR,
        FSL_IMX93_WDOG1, FSL_IMX93_WDOG2, FSL_IMX93_WDOG3,
        FSL_IMX93_WDOG4, FSL_IMX93_WDOG5,
        FSL_IMX93_TRDC, FSL_IMX93_BBNSM, FSL_IMX93_TMU, FSL_IMX93_ADC1,
        FSL_IMX93_MEDIA_BLK_CTRL, FSL_IMX93_MIPI_CSI, FSL_IMX93_DSI,
        FSL_IMX93_LCDIF, FSL_IMX93_ISI,
        FSL_IMX93_TPM1, FSL_IMX93_TPM2, FSL_IMX93_TPM3,
        FSL_IMX93_TPM4, FSL_IMX93_TPM5, FSL_IMX93_TPM6,
        FSL_IMX93_I3C1, FSL_IMX93_I3C2,
        FSL_IMX93_LPI2C1, FSL_IMX93_LPI2C2, FSL_IMX93_LPI2C3, FSL_IMX93_LPI2C4,
        FSL_IMX93_LPI2C5, FSL_IMX93_LPI2C6, FSL_IMX93_LPI2C7, FSL_IMX93_LPI2C8,
        FSL_IMX93_LPSPI1, FSL_IMX93_LPSPI2, FSL_IMX93_LPSPI3, FSL_IMX93_LPSPI4,
        FSL_IMX93_LPSPI5, FSL_IMX93_LPSPI6, FSL_IMX93_LPSPI7, FSL_IMX93_LPSPI8,
        FSL_IMX93_FLEXCAN1, FSL_IMX93_FLEXCAN2,
        FSL_IMX93_SAI1, FSL_IMX93_SAI2, FSL_IMX93_SAI3,
        FSL_IMX93_MICFIL, FSL_IMX93_FLEXSPI1, FSL_IMX93_XCVR,
        FSL_IMX93_GPIO1, FSL_IMX93_GPIO2, FSL_IMX93_GPIO3, FSL_IMX93_GPIO4,
        FSL_IMX93_USBOTG1, FSL_IMX93_USBOTG2,
    };

    for (size_t i = 0; i < ARRAY_SIZE(unimplemented_regions); i++) {
        int r = unimplemented_regions[i];
        create_unimplemented_device(fsl_imx93_memmap[r].name,
                                    fsl_imx93_memmap[r].addr,
                                    fsl_imx93_memmap[r].size);
    }
}

static void fsl_imx93_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx93State *s = FSL_IMX93(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    int i;

    if (ms->smp.cpus > FSL_IMX93_NUM_A55_CPUS) {
        error_setg(errp, "%s: only %d A55 CPUs are supported (%d requested)",
                   TYPE_FSL_IMX93, FSL_IMX93_NUM_A55_CPUS, (int)ms->smp.cpus);
        return;
    }

    /* Instantiate the A55 cluster. */
    for (i = 0; i < ms->smp.cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i], cpu_type);
    }

    for (i = 0; i < ms->smp.cpus; i++) {
        if (ms->smp.cpus > 1 &&
            object_property_find(OBJECT(&s->cpu[i]), "reset-cbar")) {
            object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                    fsl_imx93_memmap[FSL_IMX93_GIC_DIST].addr,
                                    &error_abort);
        }

        /*
         * MPIDR affinity must match the stock DT: imx93.dtsi places the two
         * A55s at cpu@0 (Aff1.Aff0 = 0.0) and cpu@100 (Aff1.Aff0 = 1.0), i.e.
         * each core in its own affinity-level-1 group. QEMU otherwise numbers
         * secondaries in Aff0 (0x0, 0x1), so a PSCI CPU_ON targeting 0x100
         * would find no matching CPU and fail with -EINVAL (the
         * "psci: failed to boot CPU1 (-22)" seen on first bring-up).
         */
        object_property_set_int(OBJECT(&s->cpu[i]), "mp-affinity",
                                (uint64_t)i << 8, &error_abort);

        /* i.MX 93 system counter runs at 24 MHz. */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq", 24000000,
                                &error_abort);

        if (object_property_find(OBJECT(&s->cpu[i]), "has_el2")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el2",
                                     !kvm_enabled(), &error_abort);
        }
        if (object_property_find(OBJECT(&s->cpu[i]), "has_el3")) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "has_el3",
                                     !kvm_enabled(), &error_abort);
        }

        if (i) {
            /* Secondary CPUs come up via PSCI / SRC. */
            object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                                     true, &error_abort);
        }

        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    /* GICv3 */
    {
        SysBusDevice *gicsbd = SYS_BUS_DEVICE(&s->gic);
        QList *redist_region_count;

        qdev_prop_set_uint32(gicdev, "num-cpu", ms->smp.cpus);
        qdev_prop_set_uint32(gicdev, "num-irq",
                             FSL_IMX93_NUM_IRQS + GIC_INTERNAL);

        redist_region_count = qlist_new();
        qlist_append_int(redist_region_count, ms->smp.cpus);
        qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

        object_property_set_link(OBJECT(&s->gic), "sysmem",
                                 OBJECT(get_system_memory()), &error_fatal);
        if (!sysbus_realize(gicsbd, errp)) {
            return;
        }
        sysbus_mmio_map(gicsbd, 0, fsl_imx93_memmap[FSL_IMX93_GIC_DIST].addr);
        sysbus_mmio_map(gicsbd, 1, fsl_imx93_memmap[FSL_IMX93_GIC_REDIST].addr);

        /* Wire the per-CPU timer PPIs and IRQ/FIQ lines (same pattern as 8MP). */
        for (i = 0; i < ms->smp.cpus; i++) {
            DeviceState *cpudev = DEVICE(&s->cpu[i]);
            int intidbase = FSL_IMX93_NUM_IRQS + i * GIC_INTERNAL;
            qemu_irq irq;

            static const int timer_irqs[] = {
                [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
                [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
                [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
                [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
            };
            for (size_t j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
                irq = qdev_get_gpio_in(gicdev, intidbase + timer_irqs[j]);
                qdev_connect_gpio_out(cpudev, j, irq);
            }

            sysbus_connect_irq(gicsbd, i,
                qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
            sysbus_connect_irq(gicsbd, i + ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gicsbd, i + 2 * ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gicsbd, i + 3 * ms->smp.cpus,
                qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        }
    }

    /* On-chip RAM. */
    memory_region_init_ram(&s->ocram, NULL, "imx93-ocram",
                           fsl_imx93_memmap[FSL_IMX93_OCRAM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx93_memmap[FSL_IMX93_OCRAM].addr,
                                &s->ocram);

    /* LPUARTs. LPUART1 is the 11x11 EVK console (stdout-path = &lpuart1). */
    {
        static const struct {
            int region;
            int irq;
        } lpuart_table[FSL_IMX93_NUM_MODELED_LPUARTS] = {
            { FSL_IMX93_LPUART1, FSL_IMX93_LPUART1_IRQ },
            { FSL_IMX93_LPUART2, FSL_IMX93_LPUART2_IRQ },
            { FSL_IMX93_LPUART3, FSL_IMX93_LPUART3_IRQ },
            { FSL_IMX93_LPUART4, FSL_IMX93_LPUART4_IRQ },
            { FSL_IMX93_LPUART5, FSL_IMX93_LPUART5_IRQ },
            { FSL_IMX93_LPUART6, FSL_IMX93_LPUART6_IRQ },
            { FSL_IMX93_LPUART7, FSL_IMX93_LPUART7_IRQ },
            { FSL_IMX93_LPUART8, FSL_IMX93_LPUART8_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_MODELED_LPUARTS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpuart[i]);

            qdev_prop_set_chr(DEVICE(&s->lpuart[i]), "chardev", serial_hd(i));
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[lpuart_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpuart_table[i].irq));
        }
    }

    /*
     * Clock infrastructure. The i.MX 93 has no System Manager, so these are
     * functionally modeled (not SCMI-stubbed): Linux programs them directly.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ccm), 0,
                    fsl_imx93_memmap[FSL_IMX93_CCM].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->anatop), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->anatop), 0,
                    fsl_imx93_memmap[FSL_IMX93_ANATOP].addr);

    /* PXP: reset-only model so the driver's unbounded soft-reset poll ends. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pxp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pxp), 0,
                    fsl_imx93_memmap[FSL_IMX93_PXP].addr);

    /*
     * ELE (EdgeLock Enclave) s4muap MU + success responder. Lets the fsl-se
     * driver probe (se_soc_info no longer times out), which in turn lets the
     * OCOTP driver register the FEC/eQOS MAC nvmem cells. "tx"/"rx" IRQs are
     * SPI 31/30.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ele), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ele), 0,
                    fsl_imx93_memmap[FSL_IMX93_ELE_MU].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ELE_TX_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_ELE_RX_IRQ));

    /*
     * uSDHC controllers. Real imx-usdhc model (carries the
     * SDHCI_QUIRK_SDCLK_AUTO_GATE fix) so the sdhci-esdhc-imx driver's
     * commands complete and, critically, device_shutdown() does not wedge
     * the way it would against a logging stub - which is what lets a guest
     * poweroff reach PSCI SYSTEM_OFF.
     */
    {
        static const struct {
            int region;
            int irq;
        } usdhc_table[FSL_IMX93_NUM_USDHCS] = {
            { FSL_IMX93_USDHC1, FSL_IMX93_USDHC1_IRQ },
            { FSL_IMX93_USDHC2, FSL_IMX93_USDHC2_IRQ },
            { FSL_IMX93_USDHC3, FSL_IMX93_USDHC3_IRQ },
        };

        for (i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usdhc[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx93_memmap[usdhc_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usdhc_table[i].irq));
        }
    }

    /* FEC ethernet (the eQOS dwmac has no upstream model yet; it stays a
     * stub). The internal PHY answers at the 11x11 EVK's MDIO address 2. */
    object_property_set_uint(OBJECT(&s->fec), "phy-num",
                             FSL_IMX93_FEC_PHY_NUM, &error_abort);
    object_property_set_uint(OBJECT(&s->fec), "tx-ring-num", 3, &error_abort);
    qemu_configure_nic_device(DEVICE(&s->fec), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fec), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fec), 0,
                    fsl_imx93_memmap[FSL_IMX93_FEC].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_FEC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX93_FEC_TIMER_IRQ));

    /* All peripherals not yet modeled get logging stubs. */
    fsl_imx93_install_unimplemented(s);
}

static void fsl_imx93_init(Object *obj)
{
    FslImx93State *s = FSL_IMX93(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);
    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMX93_CCM);
    object_initialize_child(obj, "anatop", &s->anatop, TYPE_IMX93_ANATOP);
    object_initialize_child(obj, "pxp", &s->pxp, TYPE_IMX93_PXP);
    object_initialize_child(obj, "ele", &s->ele, TYPE_IMX93_ELE);

    for (i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
    }

    object_initialize_child(obj, "fec", &s->fec, TYPE_IMX_ENET);

    for (i = 0; i < FSL_IMX93_NUM_MODELED_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }
}

static void fsl_imx93_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fsl_imx93_realize;
    /* This is an SoC, not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx93_types[] = {
    {
        .name           = TYPE_FSL_IMX93,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(FslImx93State),
        .instance_init  = fsl_imx93_init,
        .class_init     = fsl_imx93_class_init,
    },
};

DEFINE_TYPES(fsl_imx93_types)
