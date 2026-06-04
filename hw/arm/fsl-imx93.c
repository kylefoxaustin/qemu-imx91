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

    /* Messaging Units (AONMIX MU1, WAKEUPMIX MU2, ELE/Sentinel S4 MU). */
    [FSL_IMX93_MU1]                  = { 0x44230000, 64 * KiB,   "mu1" },
    [FSL_IMX93_MU2]                  = { 0x42440000, 64 * KiB,   "mu2" },
    [FSL_IMX93_ELE_MU]               = { 0x47520000, 64 * KiB,   "ele_mu_s4" },

    /* System counter. */
    [FSL_IMX93_SYSCTR]               = { 0x44290000, 192 * KiB,  "sysctr" },

    /* Watchdogs. */
    [FSL_IMX93_WDOG3]                = { 0x442d0000, 64 * KiB,   "wdog3" },
    [FSL_IMX93_WDOG4]                = { 0x442e0000, 64 * KiB,   "wdog4" },

    /* Trusted Resource Domain Controller. */
    [FSL_IMX93_TRDC]                 = { 0x44270000, 64 * KiB,   "trdc" },
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
        FSL_IMX93_MU1, FSL_IMX93_MU2, FSL_IMX93_ELE_MU, FSL_IMX93_SYSCTR,
        FSL_IMX93_WDOG3, FSL_IMX93_WDOG4, FSL_IMX93_TRDC,
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
