/*
 * NXP i.MX 91 11x11 Evaluation Kit (LPDDR4) - QEMU machine
 *
 * Modeled on hw/arm/imx8mp-evk.c (Bernhard Beschow) and the i.MX 95 port.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Instantiates the i.MX 91 SoC, attaches LPDDR4, injects the virtio-mmio and
 * secure-enclave device-tree nodes the guest needs, wires up any -drive if=sd
 * cards and -machine canbus buses, and hands control to arm_load_kernel() so
 * -kernel boots to a console on LPUART1.
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "hw/arm/boot.h"
#include "hw/arm/fsl-imx91.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/sd/sd.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "net/can_emu.h"

#define TYPE_IMX91_EVK_MACHINE MACHINE_TYPE_NAME("imx91-11x11-evk")
OBJECT_DECLARE_SIMPLE_TYPE(Imx91EvkMachineState, IMX91_EVK_MACHINE)

struct Imx91EvkMachineState {
    MachineState parent_obj;

    /* Optional CAN buses, attached via -machine canbus0=...,canbus1=... */
    CanBusState *canbus[FSL_IMX91_NUM_FLEXCAN];
    /* FlexSPI flash device, via -machine flexspi-flash=gd5f4gq4 (NAND DTBs). */
    char *flexspi_flash;
};

/*
 * Inject device-tree nodes for the virtio-mmio transports the SoC instantiates
 * (see fsl-imx91.c). The kernel's CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES is off, so
 * it only binds these via DT. The stock DTB root carries interrupt-parent =
 * <&gic> and #address-cells/#size-cells = <2>, so a root-level node inherits
 * the GIC and uses 2-cell addresses. interrupts = <SPI N LEVEL_HIGH>.
 */
static void imx91_evk_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    for (int i = FSL_IMX91_NUM_VIRTIO_MMIO - 1; i >= 0; i--) {
        hwaddr base = FSL_IMX91_VIRTIO_MMIO_BASE +
                      i * FSL_IMX91_VIRTIO_MMIO_SIZE;
        int irq = FSL_IMX91_VIRTIO_MMIO_IRQ + i;
        g_autofree char *node = g_strdup_printf("/virtio_mmio@%" PRIx64, base);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "compatible", "virtio,mmio");
        qemu_fdt_setprop_cells(fdt, node, "reg",
                               0, base, 0, FSL_IMX91_VIRTIO_MMIO_SIZE);
        /* GIC_FDT_IRQ_TYPE_SPI = 0, IRQ_TYPE_LEVEL_HIGH = 4 */
        qemu_fdt_setprop_cells(fdt, node, "interrupts", 0, irq, 4);
        qemu_fdt_setprop(fdt, node, "dma-coherent", NULL, 0);
    }

    /*
     * The stock i.MX 91 EVK device tree's secure-enclave (fsl,imx93-se) node
     * omits the tamper-IRQ "interrupts" property that the i.MX 93's carries.
     * Without it the fsl-se driver fails platform_get_irq() (-ENXIO) and never
     * probes, so it never registers its se_data; that cascades to the
     * OCOTP-fsb-s400 nvmem provider (imx_get_se_data_info() returns NULL ->
     * -EPROBE_DEFER), leaving the ENET_QoS MAC and thermal-sensor nvmem
     * consumers stuck in deferred probe. Inject the two AONMIX secvio/tamper
     * SPIs (34, 35 - the same lines the i.MX 93 dtb wires) so the secure-
     * enclave driver registers and the fuse nvmem cells resolve.
     */
    {
        char **se = qemu_fdt_node_path(fdt, NULL, "fsl,imx93-se", &error_fatal);

        for (int i = 0; se && se[i]; i++) {
            qemu_fdt_setprop_cells(fdt, se[i], "interrupts",
                                   0, 34, 4, 0, 35, 4);
        }
        g_strfreev(se);
    }
}

static void imx91_evk_init(MachineState *machine)
{
    Imx91EvkMachineState *m = IMX91_EVK_MACHINE(machine);
    static struct arm_boot_info boot_info;
    FslImx91State *s;
    int i;

    if (machine->ram_size > FSL_IMX91_RAM_SIZE_MAX) {
        error_report("RAM size " RAM_ADDR_FMT
                     " above max supported (0x%" PRIx64 ")",
                     machine->ram_size, (uint64_t)FSL_IMX91_RAM_SIZE_MAX);
        exit(1);
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = FSL_IMX91_RAM_START,
        .board_id     = -1,
        .ram_size     = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb   = imx91_evk_modify_dtb,
    };

    s = FSL_IMX91(object_new(TYPE_FSL_IMX91));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));

    /* Forward any user-attached CAN buses to the SoC's FlexCAN controllers. */
    for (i = 0; i < FSL_IMX91_NUM_FLEXCAN; i++) {
        if (m->canbus[i]) {
            g_autofree char *name = g_strdup_printf("canbus%d", i);
            object_property_set_link(OBJECT(s), name, OBJECT(m->canbus[i]),
                                     &error_abort);
        }
    }

    /* Forward the FlexSPI flash selection (NOR by default, or SPI-NAND). */
    if (m->flexspi_flash) {
        object_property_set_str(OBJECT(s), "flexspi-flash", m->flexspi_flash,
                                &error_abort);
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX91_RAM_START,
                                machine->ram);

    /* Attach an SD/MMC card to any uSDHC fed by a -drive if=sd,index=N. */
    for (i = 0; i < FSL_IMX91_NUM_USDHCS; i++) {
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

static const char *imx91_evk_get_default_cpu_type(const MachineState *ms)
{
    if (kvm_enabled()) {
        return ARM_CPU_TYPE_NAME("host");
    }
    return ARM_CPU_TYPE_NAME("cortex-a55");
}

/*
 * Constrain -cpu so a stray model is rejected with a clear message rather than
 * building the wrong core silently -- or, worse, core-dumping: an M-profile
 * type (e.g. -cpu cortex-m33) aborts in the SoC's A-profile wiring (no cntfrq
 * property).  Mirrors get_default_cpu_type: the sole valid type is cortex-a55
 * under TCG, or the host CPU under KVM.
 */
static GPtrArray *imx91_evk_get_valid_cpu_types(const MachineState *ms)
{
    GPtrArray *vct = g_ptr_array_new_with_free_func(g_free);

    if (kvm_enabled()) {
        g_ptr_array_add(vct, g_strdup(ARM_CPU_TYPE_NAME("host")));
    } else {
        g_ptr_array_add(vct, g_strdup(ARM_CPU_TYPE_NAME("cortex-a55")));
    }
    return vct;
}

static void imx91_11x11_evk_machine_init(MachineClass *mc)
{
    mc->desc                  = "NXP i.MX 91 11x11 EVK (LPDDR4)";
    mc->init                  = imx91_evk_init;
    /* The i.MX 91 is single-core (one Cortex-A55, no M33). */
    mc->default_cpus          = FSL_IMX91_NUM_A55_CPUS;
    mc->max_cpus              = FSL_IMX91_NUM_A55_CPUS;
    mc->default_ram_id        = "imx91-11x11-evk.ram";
    mc->default_ram_size      = 2 * GiB;   /* 11x11 EVK: 2 GiB LPDDR4 */
    mc->get_default_cpu_type  = imx91_evk_get_default_cpu_type;
    mc->get_valid_cpu_types   = imx91_evk_get_valid_cpu_types;
}

static void imx91_evk_set_flexspi_flash(Object *obj, const char *value,
                                        Error **errp)
{
    Imx91EvkMachineState *m = IMX91_EVK_MACHINE(obj);

    g_free(m->flexspi_flash);
    m->flexspi_flash = g_strdup(value);
}

static void imx91_evk_machine_instance_init(Object *obj)
{
    int i;

    /*
     * Per-FlexCAN CAN-bus links, settable from the command line, e.g.
     *   -object can-bus,id=canbus0 -machine canbus0=canbus0
     * The machine forwards each to the matching SoC FlexCAN controller.
     */
    for (i = 0; i < FSL_IMX91_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)&IMX91_EVK_MACHINE(obj)->canbus[i],
                                 object_property_allow_set_link, 0);
    }

    object_property_add_str(obj, "flexspi-flash", NULL,
                            imx91_evk_set_flexspi_flash);
}

static void imx91_11x11_evk_class_init(ObjectClass *oc, const void *data)
{
    imx91_11x11_evk_machine_init(MACHINE_CLASS(oc));
}

static const TypeInfo imx91_11x11_evk_machine_types[] = {
    {
        .name          = TYPE_IMX91_EVK_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(Imx91EvkMachineState),
        .instance_init = imx91_evk_machine_instance_init,
        .class_init    = imx91_11x11_evk_class_init,
        .interfaces    = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(imx91_11x11_evk_machine_types)
