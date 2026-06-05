/*
 * NXP i.MX 93 SAI (Synchronous Audio Interface)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The i.MX93 has three SAI instances (sai1/2/3), each an I2S transmit/receive
 * front-end whose FIFOs are drained/filled by eDMA3. This model carries the
 * register file the fsl-sai driver needs to probe and the ASoC card to
 * register: a correct VERID/PARAM (so version/FIFO-depth detection succeeds)
 * plus self-clearing software-/FIFO-reset bits. Actual sample movement is left
 * to the eDMA datapath; no audio backend is attached.
 */

#ifndef IMX93_SAI_H
#define IMX93_SAI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_IMX93_SAI "imx93.sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SaiState, IMX93_SAI)

#define IMX93_SAI_SIZE   0x10000
/* Registers run from VERID (0x00) to MDIV (0x104); cover a little past that. */
#define IMX93_SAI_REGS   (0x108 / 4)

struct IMX93SaiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[IMX93_SAI_REGS];
};

#endif /* IMX93_SAI_H */
