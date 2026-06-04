/*
 * NXP i.MX 93 11x11 Evaluation Kit (LPDDR4X) - QEMU machine
 *
 * Modeled on hw/arm/imx8mp-evk.c (Bernhard Beschow) and the i.MX 95 port.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * v0.0.1 scope: instantiate the SoC, attach DDR, hand control to
 * arm_load_kernel() so -kernel works. No DTB modification, no SD card,
 * no console yet (LPUART model arrives in v0.0.2).
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/fsl-imx93.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

static void imx93_evk_init(MachineState *machine)
{
    static struct arm_boot_info boot_info;
    FslImx93State *s;

    if (machine->ram_size > FSL_IMX93_RAM_SIZE_MAX) {
        error_report("RAM size " RAM_ADDR_FMT
                     " above max supported (0x%" PRIx64 ")",
                     machine->ram_size, (uint64_t)FSL_IMX93_RAM_SIZE_MAX);
        exit(1);
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = FSL_IMX93_RAM_START,
        .board_id     = -1,
        .ram_size     = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
    };

    s = FSL_IMX93(object_new(TYPE_FSL_IMX93));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX93_RAM_START,
                                machine->ram);

    /* Attach an SD/MMC card to any uSDHC fed by a -drive if=sd,index=N. */
    for (int i = 0; i < FSL_IMX93_NUM_USDHCS; i++) {
        DriveInfo *di = drive_get(IF_SD, i, 0);
        BlockBackend *blk;
        DeviceState *carddev;
        BusState *bus;

        if (!di) {
            continue;
        }
        blk = blk_by_legacy_dinfo(di);
        bus = qdev_get_child_bus(DEVICE(&s->usdhc[i]), "sd-bus");
        carddev = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(carddev, "drive", blk, &error_fatal);
        qdev_realize_and_unref(carddev, bus, &error_fatal);
    }

    if (!qtest_enabled()) {
        arm_load_kernel(&s->cpu[0], machine, &boot_info);
    }
}

static const char *imx93_evk_get_default_cpu_type(const MachineState *ms)
{
    if (kvm_enabled()) {
        return ARM_CPU_TYPE_NAME("host");
    }
    return ARM_CPU_TYPE_NAME("cortex-a55");
}

static void imx93_11x11_evk_machine_init(MachineClass *mc)
{
    mc->desc                  = "NXP i.MX 93 11x11 EVK (LPDDR4X)";
    mc->init                  = imx93_evk_init;
    mc->default_cpus          = FSL_IMX93_NUM_A55_CPUS;
    mc->max_cpus              = FSL_IMX93_NUM_A55_CPUS;
    mc->default_ram_id        = "imx93-11x11-evk.ram";
    mc->default_ram_size      = 2 * GiB;   /* 11x11 EVK ships with 2 GiB LPDDR4X */
    mc->get_default_cpu_type  = imx93_evk_get_default_cpu_type;
}

DEFINE_MACHINE_AARCH64("imx93-11x11-evk", imx93_11x11_evk_machine_init)
