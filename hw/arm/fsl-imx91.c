/*
 * NXP i.MX 91 SoC Implementation
 *
 * Derived from hw/arm/fsl-imx93.c. The i.MX 91 is a subset of the i.MX 93
 * (NXP AN14012/AN14561): single Cortex-A55, no Cortex-M33, no Ethos-U65 NPU,
 * no PXP 2D engine, no MIPI-DSI/CSI or LVDS (LCDIF parallel RGB + ISI parallel
 * camera only), 384 KiB OCRAM, LPDDR4-only. Like the i.MX 93 it has no System
 * Manager - Linux programs CCM/ANATOP/SRC directly.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Derived from hw/arm/fsl-imx93.c with the i.MX 93-only blocks removed: the
 * 2nd Cortex-A55 (single core), the Cortex-M33 + its MU peer, the Ethos-U65
 * NPU, the PXP 2D engine, and the MIPI-DSI/CSI + LVDS + ADV7535 display chain
 * (LCDIF drives a parallel-RGB panel and stands alone). Base addresses/IRQs are
 * inherited from the i.MX 93; verify against imx91.dtsi. See
 * 91_docs/IMX91_EMULATION_NOTES.md.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-clock.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fsl-imx91.h"
#include "hw/core/boards.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/unimp.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev-clock.h"
#include "qemu/main-loop.h"
#include "hw/audio/wm8962.h"
#include "hw/i2c/i2c.h"
#include "system/kvm.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/kvm_arm.h"
#include "qapi/error.h"
#include "qobject/qlist.h"

/*
 * Single source of truth for the SoC memory map. Each entry maps a region
 * ID (from enum FslImx91MemoryRegions) to its physical base address, size,
 * and a debug name. Bases are confirmed against imx91.dtsi.
 */
static const struct {
    hwaddr      addr;
    size_t      size;
    const char *name;
} fsl_imx91_memmap[FSL_IMX91_NUM_REGIONS] = {
    [FSL_IMX91_RAM] = { FSL_IMX91_RAM_START, FSL_IMX91_RAM_SIZE_MAX, "ram" },

    /* GICv3: distributor + redistributor (imx91.dtsi gic@48000000). */
    [FSL_IMX91_GIC_DIST] = { 0x48000000, 64 * KiB, "gic_dist" },
    [FSL_IMX91_GIC_REDIST] = { 0x48040000, 768 * KiB, "gic_redist" },

    /* On-chip RAM: 384 KiB on the i.MX 91 (the i.MX 93 has 640 KiB). */
    [FSL_IMX91_OCRAM] = { 0x20480000, 384 * KiB, "ocram" },

    /* LPUART console block. lpuart1/2 in AONMIX, lpuart3 in WAKEUPMIX. */
    [FSL_IMX91_LPUART1] = { 0x44380000, 64 * KiB, "lpuart1" },
    [FSL_IMX91_LPUART2] = { 0x44390000, 64 * KiB, "lpuart2" },
    [FSL_IMX91_LPUART3] = { 0x42570000, 64 * KiB, "lpuart3" },
    [FSL_IMX91_LPUART4] = { 0x42580000, 64 * KiB, "lpuart4" },
    [FSL_IMX91_LPUART5] = { 0x42590000, 64 * KiB, "lpuart5" },
    [FSL_IMX91_LPUART6] = { 0x425a0000, 64 * KiB, "lpuart6" },
    [FSL_IMX91_LPUART7] = { 0x42690000, 64 * KiB, "lpuart7" },
    [FSL_IMX91_LPUART8] = { 0x426a0000, 64 * KiB, "lpuart8" },

    /* Clock / reset / pinmux (direct register programming - no SM). */
    [FSL_IMX91_CCM] = { 0x44450000, 64 * KiB, "ccm" },
    [FSL_IMX91_ANATOP] = { 0x44480000, 8 * KiB, "anatop" },
    [FSL_IMX91_IOMUXC] = { 0x443c0000, 64 * KiB, "iomuxc" },
    [FSL_IMX91_SRC] = { 0x44460000, 64 * KiB, "src" },

    /* BLK_CTRL / syscfg aggregates per power domain. */
    [FSL_IMX91_BLK_CTRL_AONMIX] = { 0x44210000, 4 * KiB, "aonmix-blk-ctrl" },
    [FSL_IMX91_BLK_CTRL_WAKEUPMIX] = { 0x42420000, 4 * KiB, "wakeupmix-blk" },
    [FSL_IMX91_BLK_CTRL_DDRMIX] = { 0x4e010000, 64 * KiB, "ddrmix-blk-ctrl" },
    [FSL_IMX91_DDRC] = { 0x4e300000, IMX9_DDRC_SIZE, "ddrc" },

    /* Ethernet: FEC (real imx.enet) + eQOS dwmac (stub). */
    [FSL_IMX91_FEC] = { 0x42890000, 64 * KiB, "fec" },
    [FSL_IMX91_EQOS] = { 0x428a0000, 64 * KiB, "eqos" },

    /* eDMA controllers (edma1 AONMIX, edma2 WAKEUPMIX). */
    [FSL_IMX91_EDMA1] = { 0x44000000, 0x200000, "edma1" },
    [FSL_IMX91_EDMA2] = { 0x42000000, 0x210000, "edma2" },

    /* OCOTP / efuse syscon (FEC MAC-address nvmem cells live here). */
    [FSL_IMX91_OCOTP] = { 0x47510000, 64 * KiB, "ocotp" },

    /* Messaging Units (AONMIX MU1, WAKEUPMIX MU2, ELE/Sentinel S4 MU). */
    [FSL_IMX91_MU1] = { 0x44230000, 64 * KiB, "mu1" },
    [FSL_IMX91_MU2] = { 0x42440000, 64 * KiB, "mu2" },
    [FSL_IMX91_ELE_MU] = { 0x47520000, 64 * KiB, "ele_mu_s4" },

    /* System counter. */
    [FSL_IMX91_SYSCTR] = { 0x44290000, 192 * KiB, "sysctr" },

    /* Watchdogs (wdog1/2 AONMIX, wdog3/4/5 WAKEUPMIX). */
    [FSL_IMX91_WDOG1] = { 0x442d0000, 64 * KiB, "wdog1" },
    [FSL_IMX91_WDOG2] = { 0x442e0000, 64 * KiB, "wdog2" },
    [FSL_IMX91_WDOG3] = { 0x42490000, 64 * KiB, "wdog3" },
    [FSL_IMX91_WDOG4] = { 0x424a0000, 64 * KiB, "wdog4" },
    [FSL_IMX91_WDOG5] = { 0x424b0000, 64 * KiB, "wdog5" },

    /* Trusted Resource Domain Controller. */
    [FSL_IMX91_TRDC] = { 0x44270000, 64 * KiB, "trdc" },

    /* Battery-Backed Non-Secure Module (RTC + power key). */
    [FSL_IMX91_BBNSM] = { 0x44440000, 64 * KiB, "bbnsm" },

    /* Thermal Management Unit. */
    [FSL_IMX91_TMU] = { 0x44482000, 4 * KiB, "tmu" },

    /* ADC. */
    [FSL_IMX91_ADC1] = { 0x44530000, 64 * KiB, "adc1" },

    /* uSDHC controllers (eMMC / SD / SDIO). */
    [FSL_IMX91_USDHC1] = { 0x42850000, 64 * KiB, "usdhc1" },
    [FSL_IMX91_USDHC2] = { 0x42860000, 64 * KiB, "usdhc2" },
    [FSL_IMX91_USDHC3] = { 0x428b0000, 64 * KiB, "usdhc3" },

    /* Low-speed I/O controllers (stubbed). */
    [FSL_IMX91_TPM1] = { 0x44310000, 64 * KiB, "tpm1" },
    [FSL_IMX91_TPM2] = { 0x44320000, 64 * KiB, "tpm2" },
    [FSL_IMX91_TPM3] = { 0x424e0000, 64 * KiB, "tpm3" },
    [FSL_IMX91_TPM4] = { 0x424f0000, 64 * KiB, "tpm4" },
    [FSL_IMX91_TPM5] = { 0x42500000, 64 * KiB, "tpm5" },
    [FSL_IMX91_TPM6] = { 0x42510000, 64 * KiB, "tpm6" },
    [FSL_IMX91_I3C1] = { 0x44330000, 64 * KiB, "i3c1" },
    [FSL_IMX91_I3C2] = { 0x42520000, 64 * KiB, "i3c2" },
    [FSL_IMX91_LPI2C1] = { 0x44340000, 64 * KiB, "lpi2c1" },
    [FSL_IMX91_LPI2C2] = { 0x44350000, 64 * KiB, "lpi2c2" },
    [FSL_IMX91_LPI2C3] = { 0x42530000, 64 * KiB, "lpi2c3" },
    [FSL_IMX91_LPI2C4] = { 0x42540000, 64 * KiB, "lpi2c4" },
    [FSL_IMX91_LPI2C5] = { 0x426b0000, 64 * KiB, "lpi2c5" },
    [FSL_IMX91_LPI2C6] = { 0x426c0000, 64 * KiB, "lpi2c6" },
    [FSL_IMX91_LPI2C7] = { 0x426d0000, 64 * KiB, "lpi2c7" },
    [FSL_IMX91_LPI2C8] = { 0x426e0000, 64 * KiB, "lpi2c8" },
    [FSL_IMX91_LPSPI1] = { 0x44360000, 64 * KiB, "lpspi1" },
    [FSL_IMX91_LPSPI2] = { 0x44370000, 64 * KiB, "lpspi2" },
    [FSL_IMX91_LPSPI3] = { 0x42550000, 64 * KiB, "lpspi3" },
    [FSL_IMX91_LPSPI4] = { 0x42560000, 64 * KiB, "lpspi4" },
    [FSL_IMX91_LPSPI5] = { 0x426f0000, 64 * KiB, "lpspi5" },
    [FSL_IMX91_LPSPI6] = { 0x42700000, 64 * KiB, "lpspi6" },
    [FSL_IMX91_LPSPI7] = { 0x42710000, 64 * KiB, "lpspi7" },
    [FSL_IMX91_LPSPI8] = { 0x42720000, 64 * KiB, "lpspi8" },
    [FSL_IMX91_FLEXCAN1] = { 0x443a0000, 64 * KiB, "flexcan1" },
    [FSL_IMX91_FLEXCAN2] = { 0x425b0000, 64 * KiB, "flexcan2" },
    [FSL_IMX91_SAI1] = { 0x443b0000, 64 * KiB, "sai1" },
    [FSL_IMX91_SAI2] = { 0x42650000, 64 * KiB, "sai2" },
    [FSL_IMX91_SAI3] = { 0x42660000, 64 * KiB, "sai3" },
    [FSL_IMX91_MICFIL] = { 0x44520000, 64 * KiB, "micfil" },
    [FSL_IMX91_FLEXSPI1] = { 0x425e0000, 64 * KiB, "flexspi1" },
    [FSL_IMX91_XCVR] = { 0x42680000, 64 * KiB, "xcvr" },
    [FSL_IMX91_GPIO1] = { 0x47400000, 64 * KiB, "gpio1" },
    [FSL_IMX91_GPIO2] = { 0x43810000, 64 * KiB, "gpio2" },
    [FSL_IMX91_GPIO3] = { 0x43820000, 64 * KiB, "gpio3" },
    [FSL_IMX91_GPIO4] = { 0x43830000, 64 * KiB, "gpio4" },
    [FSL_IMX91_USBOTG1] = { 0x4c100000, 64 * KiB, "usbotg1" },
    [FSL_IMX91_USBOTG2] = { 0x4c200000, 64 * KiB, "usbotg2" },

    /* MEDIAMIX: block control + imaging cluster (lcdif/isi). */
    [FSL_IMX91_MEDIAMIX_PD] = { 0x44462400, 0x400, "mediamix-pd" },
    [FSL_IMX91_MEDIA_BLK_CTRL] = { 0x4ac10000, 4 * KiB, "media_blk_ctrl" },
    [FSL_IMX91_TSTMR1] = { 0x442c0000, 64 * KiB, "tstmr1" },
    [FSL_IMX91_TSTMR2] = { 0x42480000, 64 * KiB, "tstmr2" },
    [FSL_IMX91_SEMA42_1] = { 0x44260000, 64 * KiB, "sema42-1" },
    [FSL_IMX91_SEMA42_2] = { 0x42450000, 64 * KiB, "sema42-2" },
    [FSL_IMX91_FLEXIO1] = { 0x425c0000, 64 * KiB, "flexio1" },
    [FSL_IMX91_FLEXIO2] = { 0x425d0000, 64 * KiB, "flexio2" },
    [FSL_IMX91_LCDIF] = { 0x4ae30000, 64 * KiB, "lcdif" },
    [FSL_IMX91_ISI] = { 0x4ae40000, 64 * KiB, "isi" },
};

/*
 * For every peripheral region we don't yet model, install a stub that
 * traces accesses but doesn't fault. This lets U-Boot / Linux probe
 * registers safely while we iterate.
 */
/*
 * Map a dummy region (logs + reads-as-0) at a caller-chosen priority. Mirrors
 * create_unimplemented_device(), which is fixed at -1000, so we can sit a
 * catch-all *below* the named -1000 stubs.
 */
static void fsl_imx91_map_background(const char *name, hwaddr base,
                                     hwaddr size, int priority)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0, base, priority);
}

static void fsl_imx91_install_unimplemented(FslImx91State *s)
{
    static const int unimplemented_regions[] = {
        FSL_IMX91_IOMUXC, FSL_IMX91_SRC,
        FSL_IMX91_BLK_CTRL_AONMIX, FSL_IMX91_BLK_CTRL_WAKEUPMIX,
        FSL_IMX91_BLK_CTRL_DDRMIX,
        FSL_IMX91_TRDC,
        FSL_IMX91_I3C2,
        FSL_IMX91_FLEXIO2,
    };

    /*
     * Catch-all over the whole i.MX 9 peripheral aperture (0x4000_0000 ..
     * 0x5000_0000), one priority below the named -1000 stubs and the real
     * devices (priority 0). A non-stock device tree that enables a block we do
     * not model - or a hand-edited / custom DTB poking an arbitrary peripheral
     * address - then reads 0 and logs LOG_UNIMP instead of taking a data abort,
     * so the guest boots rather than dying on the first access. OCRAM
     * (0x2048_0000) and DRAM (0x8000_0000) sit outside this window and are real
     * memory, so they are unaffected.
     */
    fsl_imx91_map_background("imx91-periph-unimplemented",
                             0x40000000, 0x10000000, -2000);

    for (size_t i = 0; i < ARRAY_SIZE(unimplemented_regions); i++) {
        int r = unimplemented_regions[i];
        create_unimplemented_device(fsl_imx91_memmap[r].name,
                                    fsl_imx91_memmap[r].addr,
                                    fsl_imx91_memmap[r].size);
    }
}

/*
 * i.MX SiP SoC-info SMC (used by Linux/U-Boot to read the SoC id + revision).
 * The i.MX 91 has no Cortex-M33, so the RPROC SMCs the i.MX 93 served are gone.
 *
 * On real hardware TF-A's imx9_soc_info_handler() (plat/imx/common/ele_api.c)
 * returns the soc word + 128-bit UID the ELE firmware reports; Linux decodes
 * a1 in drivers/soc/imx/soc-imx9.c as:
 *   soc id  = (a1 & 0xffff) >> 8   (when a1 & 0xff == 0)
 *   rev maj = ((a1 >> 28) & 0xf) - 9,  rev min = (a1 >> 24) & 0xf
 * so 0xa0009100 reads back as "i.MX91" rev 1.0 (A0). The 93 port reported
 * 0x9300 here; the only SoC-specific field is the id, which must be 0x91. The
 * UID is a per-chip serial (synthesised - there is no fixed i.MX 91 value).
 */
#define IMX_SIP_GET_SOC_INFO    0xc2000006
#define IMX91_SOC_INFO_A1       0xa0009100

static bool fsl_imx91_sip_handler(uint64_t fid, uint64_t a1, uint64_t a2,
                                  uint64_t a3, uint64_t ret[4])
{
    if (fid == IMX_SIP_GET_SOC_INFO) {
        ret[0] = 0;                          /* SMCCC_RET_SUCCESS */
        ret[1] = IMX91_SOC_INFO_A1;
        ret[2] = 0x00049f9100000000ULL;      /* uid[127:64] */
        ret[3] = 0x0000000000000001ULL;      /* uid[63:0] */
        return true;
    }
    return false;
}

/*
 * Resolve a CCM clock root BY NAME against the generated table.
 *
 * ⭐ A SLICE NUMBER COPIED BY HAND AND QUIETLY WRONG HANDS A CONSUMER SOME OTHER
 *    ROOT'S FREQUENCY -- a plausible number, which is the exact failure this whole
 *    clock-tree exercise exists to kill.  Callers treat -1 as a hard error.
 */
static int fsl_imx91_root_slice(const char *name)
{
    unsigned i;

    for (i = 0; i < IMX93_CCM_NUM_SLICES; i++) {
        if (imx93_ccm_root_name[i] && !strcmp(imx93_ccm_root_name[i], name)) {
            return i;
        }
    }
    return -1;
}

static void fsl_imx91_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslImx91State *s = FSL_IMX91(dev);
    DeviceState *gicdev = DEVICE(&s->gic);
    const char *cpu_type = ms->cpu_type ?: ARM_CPU_TYPE_NAME("cortex-a55");
    /* The i.MX 91 is single-core: one Cortex-A55, no M33. */
    const unsigned n_a55 = FSL_IMX91_NUM_A55_CPUS;
    int i;

    if (ms->smp.cpus != n_a55) {
        error_setg(errp,
                   "%s: fixed topology is %u A55; run with -smp %u "
                   "(the default) - %d requested",
                   TYPE_FSL_IMX91, n_a55, n_a55, (int)ms->smp.cpus);
        return;
    }

    /* Instantiate the A55 cluster. */
    for (i = 0; i < n_a55; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i], cpu_type);
    }

    for (i = 0; i < n_a55; i++) {
        if (n_a55 > 1 &&
            object_property_find(OBJECT(&s->cpu[i]), "reset-cbar")) {
            object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                    fsl_imx91_memmap[FSL_IMX91_GIC_DIST].addr,
                                    &error_abort);
        }

        /*
         * MPIDR affinity must match the stock DT: imx91.dtsi places the two
         * A55s at cpu@0 (Aff1.Aff0 = 0.0) and cpu@100 (Aff1.Aff0 = 1.0), i.e.
         * each core in its own affinity-level-1 group. QEMU otherwise numbers
         * secondaries in Aff0 (0x0, 0x1), so a PSCI CPU_ON targeting 0x100
         * would find no matching CPU and fail with -EINVAL (the
         * "psci: failed to boot CPU1 (-22)" seen on first bring-up).
         */
        object_property_set_int(OBJECT(&s->cpu[i]), "mp-affinity",
                                (uint64_t)i << 8, &error_abort);

        /* i.MX 91 system counter runs at 24 MHz. */
        object_property_set_int(OBJECT(&s->cpu[i]), "cntfrq", FSL_IMX91_OSC_HZ,
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

        qdev_prop_set_uint32(gicdev, "num-cpu", n_a55);
        qdev_prop_set_uint32(gicdev, "num-irq",
                             FSL_IMX91_NUM_IRQS + GIC_INTERNAL);

        redist_region_count = qlist_new();
        qlist_append_int(redist_region_count, n_a55);
        qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);

        object_property_set_link(OBJECT(&s->gic), "sysmem",
                                 OBJECT(get_system_memory()), &error_fatal);
        if (!sysbus_realize(gicsbd, errp)) {
            return;
        }
        sysbus_mmio_map(gicsbd, 0, fsl_imx91_memmap[FSL_IMX91_GIC_DIST].addr);
        sysbus_mmio_map(gicsbd, 1, fsl_imx91_memmap[FSL_IMX91_GIC_REDIST].addr);

        /* Wire the per-CPU timer PPIs + IRQ/FIQ lines (same as 8MP). */
        for (i = 0; i < n_a55; i++) {
            DeviceState *cpudev = DEVICE(&s->cpu[i]);
            int intidbase = FSL_IMX91_NUM_IRQS + i * GIC_INTERNAL;
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
            sysbus_connect_irq(gicsbd, i + n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
            sysbus_connect_irq(gicsbd, i + 2 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
            sysbus_connect_irq(gicsbd, i + 3 * n_a55,
                qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
        }
    }

    /* Service the i.MX SiP SoC-info SMC (Linux/U-Boot read the SoC id+rev). */
    arm_register_sip_handler(fsl_imx91_sip_handler);

    /*
     * MU1: the A55-side messaging unit (mailbox@44230000). On the i.MX 93 this
     * is the A55<->M33 mailbox; the i.MX 91 has no M33, so it has no peer - the
     * imx-mailbox driver still binds it as a standalone MU controller.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->mu1);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_MU1].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX91_MU1_IRQ));
    }

    /* On-chip RAM. */
    memory_region_init_ram(&s->ocram, NULL, "imx93-ocram",
                           fsl_imx91_memmap[FSL_IMX91_OCRAM].size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(),
                                fsl_imx91_memmap[FSL_IMX91_OCRAM].addr,
                                &s->ocram);

    /* LPUARTs. LPUART1 is the 11x11 EVK console (stdout-path = &lpuart1). */
    {
        static const struct {
            int region;
            int irq;
        } lpuart_table[FSL_IMX91_NUM_MODELED_LPUARTS] = {
            { FSL_IMX91_LPUART1, FSL_IMX91_LPUART1_IRQ },
            { FSL_IMX91_LPUART2, FSL_IMX91_LPUART2_IRQ },
            { FSL_IMX91_LPUART3, FSL_IMX91_LPUART3_IRQ },
            { FSL_IMX91_LPUART4, FSL_IMX91_LPUART4_IRQ },
            { FSL_IMX91_LPUART5, FSL_IMX91_LPUART5_IRQ },
            { FSL_IMX91_LPUART6, FSL_IMX91_LPUART6_IRQ },
            { FSL_IMX91_LPUART7, FSL_IMX91_LPUART7_IRQ },
            { FSL_IMX91_LPUART8, FSL_IMX91_LPUART8_IRQ },
        };

        for (i = 0; i < FSL_IMX91_NUM_MODELED_LPUARTS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpuart[i]);

            qdev_prop_set_chr(DEVICE(&s->lpuart[i]), "chardev", serial_hd(i));
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx91_memmap[lpuart_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpuart_table[i].irq));
        }
    }

    /*
     * Clock infrastructure. The i.MX 91 has no System Manager, so these are
     * functionally modeled (not SCMI-stubbed): Linux programs them directly.
     */
    /*
     * The clock tree, wired for real:
     *
     *     osc_24m --> ANATOP --(arm/audio/video/dram PLL)--> CCM --> consumers
     *
     * The ANATOP COMPUTES each PLL from the registers the guest wrote; the CCM
     * COMPUTES each root as source(MUX) / (DIV + 1).  Nothing here asserts a
     * frequency and nothing falls back to one: a root with no source produces no
     * clock, and a consumer with no clock DOES NOT TICK.
     */
    clock_set_hz(s->osc_24m, FSL_IMX91_OSC_HZ);

    qdev_connect_clock_in(DEVICE(&s->anatop), "osc_in", s->osc_24m);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->anatop), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->anatop), 0,
                    fsl_imx91_memmap[FSL_IMX91_ANATOP].addr);

    qdev_connect_clock_in(DEVICE(&s->ccm), "osc_in", s->osc_24m);
    qdev_connect_clock_in(DEVICE(&s->ccm), "arm_pll_in",
                          qdev_get_clock_out(DEVICE(&s->anatop), "arm_pll"));
    qdev_connect_clock_in(DEVICE(&s->ccm), "audio_pll_in",
                          qdev_get_clock_out(DEVICE(&s->anatop), "audio_pll"));
    qdev_connect_clock_in(DEVICE(&s->ccm), "video_pll_in",
                          qdev_get_clock_out(DEVICE(&s->anatop), "video_pll"));
    qdev_connect_clock_in(DEVICE(&s->ccm), "dram_pll_in",
                          qdev_get_clock_out(DEVICE(&s->anatop), "dram_pll"));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ccm), 0,
                    fsl_imx91_memmap[FSL_IMX91_CCM].addr);

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
                    fsl_imx91_memmap[FSL_IMX91_ELE_MU].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_ELE_TX_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ele), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_ELE_RX_IRQ));

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
        } usdhc_table[FSL_IMX91_NUM_USDHCS] = {
            { FSL_IMX91_USDHC1, FSL_IMX91_USDHC1_IRQ },
            { FSL_IMX91_USDHC2, FSL_IMX91_USDHC2_IRQ },
            { FSL_IMX91_USDHC3, FSL_IMX91_USDHC3_IRQ },
        };

        for (i = 0; i < FSL_IMX91_NUM_USDHCS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usdhc[i]);

            /*
             * VEND_SPEC comes out of reset at 3000_7809h (IMX91RM), not 0 -- bits
             * 14:11 are the uSDHC's soft clock enables, and sdhci-esdhc-imx.c
             * read-modify-writes this register.  See sdhci_reset().
             */
            qdev_prop_set_uint32(DEVICE(sbd), "vendor-spec-reset", 0x30007809);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx91_memmap[usdhc_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usdhc_table[i].irq));
        }
    }

    /* FEC ethernet (real imx.enet); PHY at the 11x11 EVK's MDIO address 2. */
    object_property_set_uint(OBJECT(&s->fec), "phy-num",
                             FSL_IMX91_FEC_PHY_NUM, &error_abort);
    object_property_set_uint(OBJECT(&s->fec), "tx-ring-num", 3, &error_abort);
    /* ECR's reserved top nibble is 7 on this SoC, not the i.MX6/7's f. */
    object_property_set_uint(OBJECT(&s->fec), "ecr-reset", 0x70000000, &error_abort);
    qemu_configure_nic_device(DEVICE(&s->fec), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fec), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fec), 0,
                    fsl_imx91_memmap[FSL_IMX91_FEC].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_FEC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fec), 1,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_FEC_TIMER_IRQ));

    /* eQOS (dwmac4) ethernet - the board's second NIC. */
    qemu_configure_nic_device(DEVICE(&s->eqos), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eqos), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->eqos), 0,
                    fsl_imx91_memmap[FSL_IMX91_EQOS].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eqos), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_EQOS_IRQ));

    /*
     * LPI2C2: real controller with the board's PMIC (pca9451a @ 0x25) and
     * PCAL6524 GPIO expander (@ 0x22) attached. The expander provides the FEC
     * PHY reset-gpio; the PMIC's regulators unblock uSDHC and friends. Other
     * LPI2C instances stay logging stubs.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c2);
        I2CSlave *pmic;

        qdev_prop_set_string(DEVICE(&s->lpi2c2), "bus-name", "lpi2c2");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_LPI2C2].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX91_LPI2C2_IRQ));

        pmic = i2c_slave_new(TYPE_IMX93_I2C_REGDEV, FSL_IMX91_PCA9451_ADDR);
        qdev_prop_set_uint8(DEVICE(pmic), "reg0", FSL_IMX91_PCA9451_DEVID);
        qdev_prop_set_bit(DEVICE(pmic), "pca9450", true);
        i2c_slave_realize_and_unref(pmic, s->lpi2c2.bus, &error_abort);

        I2CSlave *expander = i2c_slave_new(TYPE_IMX93_I2C_REGDEV,
                                           FSL_IMX91_PCAL6524_ADDR);
        qdev_prop_set_bit(DEVICE(expander), "pcal6524", true);
        i2c_slave_realize_and_unref(expander, s->lpi2c2.bus, &error_abort);

        /*
         * ADP5585 I/O expander (io-expander@34): its adp5585 MFD driver checks
         * the ID register then registers the GPIO whose hogs gate the board's
         * audio-power and CAN2-standby regulators. Without it those regulators
         * (and their consumers) stay in deferred probe. (No LVDS backlight on
         * the i.MX 91, but the audio/CAN rails still depend on this expander.)
         */
        I2CSlave *adp = i2c_slave_new(TYPE_IMX93_I2C_REGDEV,
                                      FSL_IMX91_ADP5585_ADDR);
        qdev_prop_set_uint8(DEVICE(adp), "reg0", FSL_IMX91_ADP5585_ID);
        i2c_slave_realize_and_unref(adp, s->lpi2c2.bus, &error_abort);
    }

    /*
     * GPIO banks (gpio1..gpio4). Each exposes two GIC lines; the gpio-vf610
     * driver uses the first. Real models so the gpiochip + irqchip register.
     */
    {
        static const struct {
            int region, irq, irq_hi;
        } gpio_table[FSL_IMX91_NUM_GPIOS] = {
            { FSL_IMX91_GPIO1, FSL_IMX91_GPIO1_IRQ, FSL_IMX91_GPIO1_IRQ_HI },
            { FSL_IMX91_GPIO2, FSL_IMX91_GPIO2_IRQ, FSL_IMX91_GPIO2_IRQ_HI },
            { FSL_IMX91_GPIO3, FSL_IMX91_GPIO3_IRQ, FSL_IMX91_GPIO3_IRQ_HI },
            { FSL_IMX91_GPIO4, FSL_IMX91_GPIO4_IRQ, FSL_IMX91_GPIO4_IRQ_HI },
        };

        for (i = 0; i < FSL_IMX91_NUM_GPIOS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->gpio[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx91_memmap[gpio_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, gpio_table[i].irq));
            sysbus_connect_irq(sbd, 1,
                qdev_get_gpio_in(gicdev, gpio_table[i].irq_hi));
        }
    }

    /*
     * Display: LCDIFv3 driving a parallel-RGB panel (the i.MX 91 has no MIPI
     * DSI, no LVDS, no HDMI bridge - LCDIF parallel RGB only). The MEDIAMIX
     * block control (GPR) carries the LCDIF output mux; the SRC "mediamix"
     * slice reports the media power domain as on so the imx93-pd genpd
     * power-on poll completes.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mediamix), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mediamix), 0,
                    fsl_imx91_memmap[FSL_IMX91_MEDIAMIX_PD].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->media_blk_ctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->media_blk_ctrl), 0,
                    fsl_imx91_memmap[FSL_IMX91_MEDIA_BLK_CTRL].addr);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->lcdif), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->lcdif), 0,
                    fsl_imx91_memmap[FSL_IMX91_LCDIF].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->lcdif), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_LCDIF_IRQ));

    /* ISI: capture channel; synthesises frames for the imx8-isi V4L2 driver. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->isi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isi), 0,
                    fsl_imx91_memmap[FSL_IMX91_ISI].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->isi), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_ISI_IRQ));

    /*
     * eDMA1: the i.MX LPI2C driver moves any transfer >= 8 bytes (e.g. the
     * 64-byte HDMI EDID block read) through eDMA, so a working DMA engine is
     * required for the display I2C bus to read EDID. Channel N interrupts on
     * GIC SPI (95 + N).
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->edma1);

        object_property_set_uint(OBJECT(&s->edma1), "num-channels",
                                 FSL_IMX91_EDMA1_CHANNELS, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_EDMA1].addr);
        for (i = 0; i < FSL_IMX91_EDMA1_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX91_EDMA1_IRQ_BASE + i));
        }
    }

    /*
     * eDMA2 (WAKEUPMIX) is the "edma4" variant: 64 channels at a 0x8000 page
     * stride, with channel interrupts paired so channel N raises GIC SPI
     * (128 + N/2). It serves the WAKEUPMIX peripherals - notably sai2/sai3, so
     * the SAI3 + wm8962 audio card can request its DMA channels.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->edma2);

        object_property_set_uint(OBJECT(&s->edma2), "num-channels",
                                 FSL_IMX91_EDMA2_CHANNELS, &error_abort);
        /* EDMA4_2 is a different IP version: the RM gives its CSR a different reset. */
        object_property_set_uint(OBJECT(&s->edma2), "mp-csr-reset",
                                 0x00400000, &error_abort);
        object_property_set_uint(OBJECT(&s->edma2), "chan-stride",
                                 FSL_IMX91_EDMA2_CHAN_STRIDE, &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_EDMA2].addr);
        for (i = 0; i < FSL_IMX91_EDMA2_CHANNELS; i++) {
            sysbus_connect_irq(sbd, i,
                qdev_get_gpio_in(gicdev, FSL_IMX91_EDMA2_IRQ_BASE + i / 2));
        }
    }

    /*
     * LPI2C1: carries the WM8962 audio codec (the SAI3 speaker/headphone/mic
     * card). The i.MX 91 has no DSI-to-HDMI bridge, so there is no ADV7535 /
     * DDC here - the display is LCDIF parallel RGB to a fixed panel.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c1);

        qdev_prop_set_string(DEVICE(&s->lpi2c1), "bus-name", "lpi2c1");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_LPI2C1].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX91_LPI2C1_IRQ));

        /*
         * WM8962 audio codec @ 0x1a (the SAI3 speaker/headphone/mic card).  The codec
         * is the bit-clock master, so it -- not the SAI -- knows the sample rate: it
         * decodes R27 and publishes it on a "rate" clock, which we wire into SAI3's
         * "codec-rate" input.  SAI3 (&s->sai[2]) is created but not yet realized here,
         * so the clock-in connect is legal (it asserts !realized).
         */
        DeviceState *codec = DEVICE(i2c_slave_new(TYPE_WM8962,
                                                  FSL_IMX91_WM8962_ADDR));
        i2c_slave_realize_and_unref(I2C_SLAVE(codec), s->lpi2c1.bus, &error_abort);
        qdev_connect_clock_in(DEVICE(&s->sai[2]), "codec-rate",
                              qdev_get_clock_out(codec, "rate"));
    }

    /*
     * LPI2C8: the camera control bus on the mt9m114 device-tree variant. It
     * carries the MT9M114 sensor (0x48) plus a PCA9538 I/O expander (0x70)
     * that gates the sensor's power rails. With these the camera pipeline
     * (mt9m114 -> parallel-CSI -> ISI) binds and the V4L2 graph links.
     */
    {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c8);
        I2CSlave *pca;

        qdev_prop_set_string(DEVICE(&s->lpi2c8), "bus-name", "lpi2c8");
        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[FSL_IMX91_LPI2C8].addr);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX91_LPI2C8_IRQ));

        pca = i2c_slave_new(TYPE_IMX93_I2C_REGDEV, FSL_IMX91_PCA9538_ADDR);
        i2c_slave_realize_and_unref(pca, s->lpi2c8.bus, &error_abort);

        i2c_slave_realize_and_unref(
            i2c_slave_new(TYPE_MT9M114, FSL_IMX91_MT9M114_ADDR),
            s->lpi2c8.bus, &error_abort);
    }

    /*
     * LPI2C3-7: functional but unpopulated on the EVK. Bring them up as real
     * controllers (rather than logging stubs) so the i2c-N adapters register
     * and the machine can host I2C peripherals the stock EVK never defined -
     * e.g. -device <i2c-dev>,bus=/machine/soc/lpi2c5/i2c-bus.0. This is the
     * expandability goal: a board that grows beyond its reference design.
     */
    {
        static const struct {
            int region, irq;
        } lpi2c_exp_tbl[5] = {
            { FSL_IMX91_LPI2C3, FSL_IMX91_LPI2C3_IRQ },
            { FSL_IMX91_LPI2C4, FSL_IMX91_LPI2C4_IRQ },
            { FSL_IMX91_LPI2C5, FSL_IMX91_LPI2C5_IRQ },
            { FSL_IMX91_LPI2C6, FSL_IMX91_LPI2C6_IRQ },
            { FSL_IMX91_LPI2C7, FSL_IMX91_LPI2C7_IRQ },
        };

        for (i = 0; i < ARRAY_SIZE(s->lpi2c_exp); i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c_exp[i]);
            g_autofree char *bus_name = g_strdup_printf("lpi2c%d", i + 3);

            /* Name the bus so peripherals attach via -device bus=lpi2cN. */
            qdev_prop_set_string(DEVICE(&s->lpi2c_exp[i]), "bus-name",
                                 bus_name);
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                            fsl_imx91_memmap[lpi2c_exp_tbl[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, lpi2c_exp_tbl[i].irq));
        }
    }

    /*
     * FlexIO1: a configurable shifter/timer fabric. The EVK's flexio-i2c device
     * tree routes an extra I2C master through it (nxp,imx-flexio MFD +
     * i2c-flexio); modelling its register file lets that adapter register, so a
     * custom board can grow an I2C bus the reference design runs over LPI2C.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexio1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexio1),
                    0, fsl_imx91_memmap[FSL_IMX91_FLEXIO1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexio1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_FLEXIO1_IRQ));

    /*
     * virtio-mmio transports (not real i.MX91 hardware). These give the guest
     * a place to attach virtio devices - notably a virtio-keyboard so the
     * emulated HDMI/LCDIF console gets real keyboard input. Matching DTB nodes
     * are injected by the board (imx93-evk.c). SPIs 230.. are unused.
     */
    for (i = 0; i < FSL_IMX91_NUM_VIRTIO_MMIO; i++) {
        DeviceState *vmmio = qdev_new("virtio-mmio");
        SysBusDevice *sbd = SYS_BUS_DEVICE(vmmio);

        /*
         * Modern (virtio 1.0) transport; the legacy default fails feature
         * negotiation with the modern guest virtio_mmio driver.
         */
        qdev_prop_set_bit(vmmio, "force-legacy", false);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0,
            FSL_IMX91_VIRTIO_MMIO_BASE + i * FSL_IMX91_VIRTIO_MMIO_SIZE);
        sysbus_connect_irq(sbd, 0,
            qdev_get_gpio_in(gicdev, FSL_IMX91_VIRTIO_MMIO_IRQ + i));
    }

    /*
     * FlexCAN1/2. Real controllers (hw/net/can/flexcan.c) on QEMU's CAN bus
     * subsystem; each is wired to a user-supplied CAN bus via the board's
     * canbus0/canbus1 links (NULL = register-only, frames dropped). The stock
     * NXP EVK DT enables flexcan2; the Linux flexcan driver binds and brings up
     * the canN netdev.
     */
    {
        static const struct {
            int region, irq;
        } flexcan_table[FSL_IMX91_NUM_FLEXCAN] = {
            { FSL_IMX91_FLEXCAN1, FSL_IMX91_FLEXCAN1_IRQ },
            { FSL_IMX91_FLEXCAN2, FSL_IMX91_FLEXCAN2_IRQ },
        };

        for (i = 0; i < FSL_IMX91_NUM_FLEXCAN; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->flexcan[i]);

            if (s->canbus[i]) {
                object_property_set_link(OBJECT(&s->flexcan[i]), "canbus",
                                         OBJECT(s->canbus[i]), &error_abort);
            }
            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0,
                fsl_imx91_memmap[flexcan_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, flexcan_table[i].irq));
        }
    }

    /*
     * USB OTG1/2 - ChipIdea controllers (EHCI host core). The USBNC "usbmisc"
     * glue at +0x200 stays a stub, as on i.MX7. Attach USB devices with e.g.
     * "-device usb-kbd"; note the stock EVK DT sets dr_mode=otg with a Type-C
     * role switch, so host mode depends on the (unmodelled) Type-C controller.
     */
    {
        static const struct {
            int region, irq;
        } usb_table[FSL_IMX91_NUM_USBS] = {
            { FSL_IMX91_USBOTG1, FSL_IMX91_USB1_IRQ },
            { FSL_IMX91_USBOTG2, FSL_IMX91_USB2_IRQ },
        };

        for (i = 0; i < FSL_IMX91_NUM_USBS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usb[i]);
            hwaddr base = fsl_imx91_memmap[usb_table[i].region].addr;
            g_autofree char *misc = g_strdup_printf("usbmisc%d", i + 1);

            /*
             * The ChipIdea identification block, from IMX91RM.pdf rev 5.  These
             * read as ZERO in the shared model, and ci_hdrc BRANCHES ON THEM:
             * ID.VERSION == 0 makes the driver conclude it is talking to a
             * ChipIdea 1.x controller.  This silicon is a 2.5 (ID = E4A1FA05h).
             * A zero reset value is not the absence of a claim -- it is a claim.
             *
             * HWDEVICE is deliberately left at zero: we do not emulate device
             * mode, and DCCPARAMS already reports host-only, so advertising 8
             * endpoints here would leave two registers contradicting each other
             * about the same fact.  We under-report, CONSISTENTLY.
             */
            object_property_set_uint(OBJECT(sbd), "id", 0xe4a1fa05, &error_abort);
            object_property_set_uint(OBJECT(sbd), "hwgeneral", 0x00000015, &error_abort);
            object_property_set_uint(OBJECT(sbd), "hwhost", 0x10020001, &error_abort);
            object_property_set_uint(OBJECT(sbd), "hwtxbuf", 0x80080b08, &error_abort);
            object_property_set_uint(OBJECT(sbd), "hwrxbuf", 0x00000808, &error_abort);
            object_property_set_uint(OBJECT(sbd), "sbuscfg", 0x00000002, &error_abort);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, base);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, usb_table[i].irq));
            create_unimplemented_device(misc, base + 0x200, 0x200);
        }
    }

    /*
     * Audio front-ends. SAI1/3 and MICFIL are the EVK's active codecs; their
     * FIFOs are serviced by eDMA3 (sai1 + micfil on edma1, sai3 on edma2). The
     * models carry the register file the fsl-sai/fsl-micfil drivers probe so
     * the ASoC cards register; sample movement rides the eDMA datapath.
     */
    {
        static const struct {
            int region, irq;
        } sai_table[FSL_IMX91_NUM_SAIS] = {
            { FSL_IMX91_SAI1, FSL_IMX91_SAI1_IRQ },
            { FSL_IMX91_SAI2, FSL_IMX91_SAI2_IRQ },
            { FSL_IMX91_SAI3, FSL_IMX91_SAI3_IRQ },
        };
        static const int micfil_irqs[IMX93_MICFIL_IRQS] = {
            FSL_IMX91_MICFIL_IRQ0, FSL_IMX91_MICFIL_IRQ1,
            FSL_IMX91_MICFIL_IRQ2, FSL_IMX91_MICFIL_IRQ3,
        };

        for (i = 0; i < FSL_IMX91_NUM_SAIS; i++) {
            SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sai[i]);

            if (!sysbus_realize(sbd, errp)) {
                return;
            }
            sysbus_mmio_map(sbd, 0, fsl_imx91_memmap[sai_table[i].region].addr);
            sysbus_connect_irq(sbd, 0,
                qdev_get_gpio_in(gicdev, sai_table[i].irq));
        }

        /*
         * eDMA requests are routed by DMA-request source id: each peripheral's
         * dma-req drives the eDMA 'dma-req' input indexed by the source id from
         * the EVK DTB dmas= props, and the eDMA matches it against the armed
         * channel's CH_MUX[7:0] (eDMA2). So SAI3 TX, SAI3 RX, and the XCVR can
         * each advance their OWN cyclic channel concurrently - the 4-way audio
         * case (play + capture + SPDIF) no longer collides on a single line.
         * SAI3 TX/RX are serviced by eDMA2 (wm8962 playback/capture).
         */
        qdev_connect_gpio_out_named(DEVICE(&s->sai[2]), "dma-req-tx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x3c));
        qdev_connect_gpio_out_named(DEVICE(&s->sai[2]), "dma-req-rx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x3d));
        /*
         * SAI1 (AONMIX) is the cpu DAI for the bt-sco card and, on the
         * imx91-...-mqs DTB, for the MQS PWM "codec" (MQS rides SAI1 TX). Its
         * TX/RX requests are serviced by eDMA1. eDMA1 (AONMIX) routes via an
         * integrated request mux rather than CH_MUX, so its CH_MUX reads 0 and
         * the eDMA's first-armed-cyclic fallback delivers these.
         */
        qdev_connect_gpio_out_named(DEVICE(&s->sai[0]), "dma-req-tx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma1), "dma-req", 0x15));
        qdev_connect_gpio_out_named(DEVICE(&s->sai[0]), "dma-req-rx", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma1), "dma-req", 0x16));

        /*
         * LPUART RX is paged through a cyclic eDMA channel by the imx-lpuart
         * driver, so each LPUART's RX DMA-request line must drive its eDMA at
         * the RX source id from the EVK DTB dmas= props - otherwise DMA-mode RX
         * never advances and received bytes never reach userspace. (TX is
         * mem->device, which the eDMA runs whole at channel start, so it needs
         * no request line.) LPUART1/2 are on eDMA1 (AONMIX), LPUART3-8 on
         * eDMA2.
         */
        {
            static const struct {
                int edma;       /* 1 = AONMIX eDMA1, 2 = WAKEUPMIX eDMA2 */
                int rx_src;
            } lpuart_dma[FSL_IMX91_NUM_MODELED_LPUARTS] = {
                { 1, 0x11 }, { 1, 0x13 },               /* LPUART1, LPUART2 */
                { 2, 0x12 }, { 2, 0x14 }, { 2, 0x16 },  /* LPUART3, 4, 5 */
                { 2, 0x18 }, { 2, 0x58 }, { 2, 0x5a },  /* LPUART6, 7, 8 */
            };
            for (i = 0; i < FSL_IMX91_NUM_MODELED_LPUARTS; i++) {
                DeviceState *edma = lpuart_dma[i].edma == 1 ?
                    DEVICE(&s->edma1) : DEVICE(&s->edma2);
                qdev_connect_gpio_out_named(DEVICE(&s->lpuart[i]),
                    "dma-req-rx", 0,
                    qdev_get_gpio_in_named(edma, "dma-req",
                                           lpuart_dma[i].rx_src));
            }
        }

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->micfil), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->micfil), 0,
                        fsl_imx91_memmap[FSL_IMX91_MICFIL].addr);
        for (i = 0; i < IMX93_MICFIL_IRQS; i++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->micfil), i,
                               qdev_get_gpio_in(gicdev, micfil_irqs[i]));
        }
        /*
         * MICFIL capture: the FIFO-has-data request drives a cyclic eDMA1
         * channel that drains DATACH0 -> memory, the same datapath that fills
         * the SAI transmit FIFO for playback (eDMA services whichever cyclic
         * channel is armed, so a MICFIL-only arecord routes here).
         */
        qdev_connect_gpio_out_named(DEVICE(&s->micfil), "dma-req", 0,
            qdev_get_gpio_in_named(DEVICE(&s->edma1), "dma-req", 0x1d));
    }

    /*
     * I3C1 (AONMIX). The imx91-...-i3c DTB moves the wm8962 codec onto the I3C
     * bus as a legacy I2C target, so attach a wm8962 at 0x1a to the I3C
     * controller's built-in I2C bus. Other DTBs omit the node, so it sits idle.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i3c1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i3c1),
                    0, fsl_imx91_memmap[FSL_IMX91_I3C1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i3c1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_I3C1_IRQ));
    i2c_slave_create_simple(s->i3c1.bus->i2c_bus, TYPE_WM8962,
                            FSL_IMX91_WM8962_ADDR);

    /*
     * DDR controller + DDR PMU. A register/perf-interface compat model only:
     * QEMU DRAM is plain host memory with no controller in the path and no
     * cache model, so the perf counters cannot measure real DDR bandwidth and
     * read back 0. It lets the fsl_imx9_ddr_perf driver probe and `perf` open
     * the imx9_ddr events instead of falling through to the catch-all.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ddrc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ddrc),
                    0, fsl_imx91_memmap[FSL_IMX91_DDRC].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ddrc), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_DDRC_PMU_IRQ));

    /* BBNSM: real-time clock + power key. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->bbnsm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bbnsm), 0,
                    fsl_imx91_memmap[FSL_IMX91_BBNSM].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bbnsm), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_BBNSM_IRQ));

    /* WDOG1-5: watchdog timers. */
    for (i = 0; i < 5; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdog[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdog[i]), 0,
                        fsl_imx91_memmap[FSL_IMX91_WDOG1 + i].addr);
    }

    /* TMU: thermal monitor (temperature is polled; alarm IRQ not modelled). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tmu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tmu), 0,
                    fsl_imx91_memmap[FSL_IMX91_TMU].addr);

    /* SAR-ADC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc1), 0,
                    fsl_imx91_memmap[FSL_IMX91_ADC1].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc1), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_ADC1_IRQ));

    /* OCOTP: fuse shadow (MAC addresses, SoC unique ID). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ocotp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ocotp), 0,
                    fsl_imx91_memmap[FSL_IMX91_OCOTP].addr);

    /*
     * System counter (clocksource + compare clockevent), clocked by the 24 MHz
     * crystal.  The counter's tick rate and its CNTFID0 frequency register both
     * derive from this one clock, so a guest computing wall-clock as ticks/CNTFID0
     * cannot get a rate the counter does not actually run at.  Wired before realize
     * (qdev_connect_clock_in asserts !realized).
     */
    qdev_connect_clock_in(DEVICE(&s->sysctr), "clk", s->osc_24m);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctr), 0,
                    fsl_imx91_memmap[FSL_IMX91_SYSCTR].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sysctr), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_SYSCTR_IRQ));

    /*
     * TPM1-6, each fed by the CCM root its LPCG hangs off (clk-imx93.c ccgr_array).
     * Note TPM1 and TPM3 are clocked from the BUS roots, NOT from a TPM root --
     * exactly the sort of fact a hardcoded 24 MHz constant made invisible.
     */
    for (i = 0; i < 6; i++) {
        static const char *const tpm_root[6] = {
            "bus_aon_root",     /* TPM1 */
            "tpm2_root",        /* TPM2 */
            "bus_wakeup_root",  /* TPM3 */
            "tpm4_root",        /* TPM4 */
            "tpm5_root",        /* TPM5 */
            "tpm6_root",        /* TPM6 */
        };
        int slice = fsl_imx91_root_slice(tpm_root[i]);

        if (slice < 0) {
            error_setg(errp, "imx91: no CCM clock root named '%s' for TPM%d",
                       tpm_root[i], i + 1);
            return;
        }
        qdev_connect_clock_in(DEVICE(&s->tpm[i]), "clk",
                              s->ccm.root_out[slice]);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tpm[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tpm[i]), 0,
                        fsl_imx91_memmap[FSL_IMX91_TPM1 + i].addr);
    }

    /* MU2 messaging unit (no peer wired; disabled on the EVK). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mu2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mu2), 0,
                    fsl_imx91_memmap[FSL_IMX91_MU2].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mu2), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_MU2_IRQ));

    /* FlexSPI NOR-flash controller + an attached SPI-NOR flash. */
    {
        SysBusDevice *fsbd = SYS_BUS_DEVICE(&s->flexspi);
        DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);
        DeviceState *flash;
        qemu_irq cs_line;

        if (!sysbus_realize(fsbd, errp)) {
            return;
        }
        sysbus_mmio_map(fsbd, 0, fsl_imx91_memmap[FSL_IMX91_FLEXSPI1].addr);
        sysbus_mmio_map(fsbd, 1, FSL_IMX91_FLEXSPI_AHB_ADDR);
        sysbus_connect_irq(fsbd, 0,
                           qdev_get_gpio_in(gicdev, FSL_IMX91_FLEXSPI1_IRQ));

        flash = qdev_new(s->flexspi_flash ?: "is25wp064");
        if (dinfo) {
            qdev_prop_set_drive(flash, "drive",
                                blk_by_legacy_dinfo(dinfo));
        }
        qdev_realize_and_unref(flash, BUS(s->flexspi.bus), &error_abort);
        cs_line = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
        qdev_connect_gpio_out_named(DEVICE(&s->flexspi), "cs", 0, cs_line);
    }

    /* XCVR SPDIF audio transceiver. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xcvr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->xcvr), 0,
                    fsl_imx91_memmap[FSL_IMX91_XCVR].addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->xcvr), 0,
                       qdev_get_gpio_in(gicdev, FSL_IMX91_XCVR_IRQ));
    /*
     * SPDIF playback: the XCVR TX FIFO is drained by a cyclic eDMA2 channel
     * (the xcvr dtb wires dmas to edma2), the same datapath that fills the SAI
     * transmit FIFO. It shares eDMA2's request line with SAI3 - the eDMA
     * services whichever cyclic channel is armed.
     */
    qdev_connect_gpio_out_named(DEVICE(&s->xcvr), "dma-req", 0,
        qdev_get_gpio_in_named(DEVICE(&s->edma2), "dma-req", 0x42));

    /* TSTMR1/2 timestamp timers + SEMA42 hardware semaphores (Group A). */
    {
        const int tstmr_r[2] = { FSL_IMX91_TSTMR1, FSL_IMX91_TSTMR2 };
        const int sema_r[2] = { FSL_IMX91_SEMA42_1, FSL_IMX91_SEMA42_2 };

        for (i = 0; i < 2; i++) {
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->tstmr[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->tstmr[i]), 0,
                            fsl_imx91_memmap[tstmr_r[i]].addr);
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->sema42[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->sema42[i]), 0,
                            fsl_imx91_memmap[sema_r[i]].addr);
        }
    }

    /* LPSPI1-8: SPI masters (each exposes an SSI bus for slaves). */
    {
        static const int lpspi_irq[8] = { 16, 17, 65, 66, 191, 192, 193, 194 };

        for (i = 0; i < 8; i++) {
            g_autofree char *name = g_strdup_printf("lpspi%d", i + 1);

            /*
             * Name each SSI bus so peripherals attach at runtime:
             * -device <ssi-dev>,bus=lpspiN.
             */
            qdev_prop_set_string(DEVICE(&s->lpspi[i]), "bus-name", name);
            if (!sysbus_realize(SYS_BUS_DEVICE(&s->lpspi[i]), errp)) {
                return;
            }
            sysbus_mmio_map(SYS_BUS_DEVICE(&s->lpspi[i]), 0,
                            fsl_imx91_memmap[FSL_IMX91_LPSPI1 + i].addr);
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->lpspi[i]), 0,
                               qdev_get_gpio_in(gicdev, lpspi_irq[i]));
        }
    }

    fsl_imx91_install_unimplemented(s);
}

static void fsl_imx91_init(Object *obj)
{
    FslImx91State *s = FSL_IMX91(obj);
    int i;

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);
    object_initialize_child(obj, "mu1", &s->mu1, TYPE_IMX_MU);
    object_initialize_child(obj, "bbnsm", &s->bbnsm, TYPE_IMX93_BBNSM);
    for (i = 0; i < 5; i++) {
        g_autofree char *name = g_strdup_printf("wdog%d", i + 1);
        object_initialize_child(obj, name, &s->wdog[i], TYPE_IMX93_WDOG);
    }
    /*
     * The i.MX 91's thermal sensor is NOT the i.MX 93's TMU.  Its device tree says
     * "fsl,imx91-tmu" (drivers/thermal/imx91_thermal.c), which speaks the
     * u_temp_anamix block: CTRL0/STAT0/DATA0/CTRL1 with SET/CLR/TOG aliases.  This
     * machine used to map the 93's TMR/TMSR/TIER register file here, so every
     * enable the driver issued through CTRL1_SET vanished and the guest could not
     * read a temperature at all.
     */
    object_initialize_child(obj, "tmu", &s->tmu, TYPE_IMX91_TMU);
    object_initialize_child(obj, "adc1", &s->adc1, TYPE_IMX93_ADC);
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("lpspi%d", i + 1);
        object_initialize_child(obj, name, &s->lpspi[i], TYPE_IMX93_LPSPI);
    }
    object_initialize_child(obj, "ocotp", &s->ocotp, TYPE_IMX93_OCOTP);
    object_initialize_child(obj, "sysctr", &s->sysctr, TYPE_IMX93_SYSCTR);
    for (i = 0; i < 6; i++) {
        g_autofree char *name = g_strdup_printf("tpm%d", i + 1);
        object_initialize_child(obj, name, &s->tpm[i], TYPE_IMX93_TPM);
    }
    object_initialize_child(obj, "mu2", &s->mu2, TYPE_IMX_MU);
    object_initialize_child(obj, "flexspi", &s->flexspi, TYPE_IMX93_FLEXSPI);
    object_initialize_child(obj, "xcvr", &s->xcvr, TYPE_IMX93_XCVR);
    for (i = 0; i < 2; i++) {
        g_autofree char *tn = g_strdup_printf("tstmr%d", i + 1);
        g_autofree char *sn = g_strdup_printf("sema42-%d", i + 1);
        object_initialize_child(obj, tn, &s->tstmr[i], TYPE_IMX93_TSTMR);
        object_initialize_child(obj, sn, &s->sema42[i], TYPE_IMX93_SEMA42);
    }
    /* The board's 24 MHz crystal: the one frequency this SoC is entitled to assert. */
    s->osc_24m = qdev_init_clock_out(DEVICE(obj), "osc_24m");

    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMX93_CCM);
    object_initialize_child(obj, "anatop", &s->anatop, TYPE_IMX93_ANATOP);
    object_initialize_child(obj, "ele", &s->ele, TYPE_IMX93_ELE);

    for (i = 0; i < FSL_IMX91_NUM_USDHCS; i++) {
        g_autofree char *name = g_strdup_printf("usdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
    }

    object_initialize_child(obj, "fec", &s->fec, TYPE_IMX_ENET);
    object_initialize_child(obj, "eqos", &s->eqos, TYPE_IMX93_DWMAC);
    object_initialize_child(obj, "edma1", &s->edma1, TYPE_IMX93_EDMA);
    object_initialize_child(obj, "edma2", &s->edma2, TYPE_IMX93_EDMA);
    object_initialize_child(obj, "lpi2c1", &s->lpi2c1, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "lpi2c2", &s->lpi2c2, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "lpi2c8", &s->lpi2c8, TYPE_IMX_LPI2C);
    object_initialize_child(obj, "flexio1", &s->flexio1, TYPE_IMX93_FLEXIO);
    for (i = 0; i < ARRAY_SIZE(s->lpi2c_exp); i++) {
        g_autofree char *name = g_strdup_printf("lpi2c%d", i + 3);
        object_initialize_child(obj, name, &s->lpi2c_exp[i], TYPE_IMX_LPI2C);
    }
    object_initialize_child(obj, "mediamix", &s->mediamix,
                            TYPE_IMX93_SRC_SLICE);
    object_initialize_child(obj, "media-blk-ctrl", &s->media_blk_ctrl,
                            TYPE_IMX93_MEDIA_BLK_CTRL);
    object_initialize_child(obj, "lcdif", &s->lcdif, TYPE_IMX93_LCDIF);
    object_initialize_child(obj, "isi", &s->isi, TYPE_IMX93_ISI);

    for (i = 0; i < FSL_IMX91_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("flexcan%d", i + 1);
        object_initialize_child(obj, name, &s->flexcan[i], TYPE_FLEXCAN);
    }

    for (i = 0; i < FSL_IMX91_NUM_USBS; i++) {
        g_autofree char *name = g_strdup_printf("usb%d", i + 1);
        object_initialize_child(obj, name, &s->usb[i], TYPE_CHIPIDEA);
    }

    for (i = 0; i < FSL_IMX91_NUM_SAIS; i++) {
        g_autofree char *name = g_strdup_printf("sai%d", i + 1);
        object_initialize_child(obj, name, &s->sai[i], TYPE_IMX93_SAI);
    }

    object_initialize_child(obj, "micfil", &s->micfil, TYPE_IMX93_MICFIL);
    object_initialize_child(obj, "i3c1", &s->i3c1, TYPE_SVC_I3C);
    object_initialize_child(obj, "ddrc", &s->ddrc, TYPE_IMX9_DDRC);

    for (i = 0; i < FSL_IMX91_NUM_GPIOS; i++) {
        g_autofree char *name = g_strdup_printf("gpio%d", i + 1);
        object_initialize_child(obj, name, &s->gpio[i], TYPE_IMX93_GPIO);
    }

    for (i = 0; i < FSL_IMX91_NUM_MODELED_LPUARTS; i++) {
        g_autofree char *name = g_strdup_printf("lpuart%d", i + 1);
        object_initialize_child(obj, name, &s->lpuart[i], TYPE_IMX_LPUART);
    }
}

static const Property fsl_imx91_properties[] = {
    DEFINE_PROP_LINK("canbus0", FslImx91State, canbus[0], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus1", FslImx91State, canbus[1], TYPE_CAN_BUS,
                     CanBusState *),
    /*
     * SSI device attached to the FlexSPI bus. Defaults to the EVK's serial NOR
     * (is25wp064); set to "gd5f4gq4" to model the SPI-NAND the flexspi-nand DTB
     * expects (-machine imx91-11x11-evk,flexspi-flash=gd5f4gq4).
     */
    DEFINE_PROP_STRING("flexspi-flash", FslImx91State, flexspi_flash),
};

static void fsl_imx91_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = fsl_imx91_realize;
    device_class_set_props(dc, fsl_imx91_properties);
    /* This is an SoC, not user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx91_types[] = {
    {
        .name           = TYPE_FSL_IMX91,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(FslImx91State),
        .instance_init  = fsl_imx91_init,
        .class_init     = fsl_imx91_class_init,
    },
};

DEFINE_TYPES(fsl_imx91_types)
