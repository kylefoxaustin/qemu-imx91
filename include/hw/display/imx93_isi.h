/*
 * NXP i.MX 93 ISI (Image Sensing Interface) - capture channel
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMX93_ISI_H
#define IMX93_ISI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ISI "imx93.isi"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93IsiState, IMX93_ISI)

#define IMX93_ISI_REG_SIZE      (64 * KiB)
#define IMX93_ISI_NUM_REGS      (IMX93_ISI_REG_SIZE / 4)

struct IMX93IsiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;               /* WAKEUPMIX ISI interrupt (GIC SPI 172) */
    QEMUTimer *frame_timer;
    uint32_t frame;             /* frame counter; parity selects BUF1/BUF2 */
    uint32_t regs[IMX93_ISI_NUM_REGS];
};

#endif /* IMX93_ISI_H */
