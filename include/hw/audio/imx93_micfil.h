/*
 * NXP i.MX 93 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The MICFIL is the i.MX93 PDM microphone front-end: it decimates PDM mic
 * streams into a FIFO that eDMA3 drains. This model carries the register file
 * the fsl-micfil driver needs to probe and the ASoC "micfil" card to register:
 * a correct VERID/PARAM and a self-clearing software-reset bit. No PDM data is
 * synthesised; capture is left to the eDMA datapath.
 */

#ifndef IMX93_MICFIL_H
#define IMX93_MICFIL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_MICFIL "imx93.micfil"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93MicfilState, IMX93_MICFIL)

#define IMX93_MICFIL_SIZE   0x10000
/* Registers run from CTRL1 (0x00) to VAD0_ZCD (0xa8). */
#define IMX93_MICFIL_REGS   (0xac / 4)
/* CTRL1.DISEL=IRQ / error / VAD share four interrupt lines. */
#define IMX93_MICFIL_IRQS   4

struct IMX93MicfilState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq[IMX93_MICFIL_IRQS];
    uint32_t regs[IMX93_MICFIL_REGS];
};

#endif /* IMX93_MICFIL_H */
