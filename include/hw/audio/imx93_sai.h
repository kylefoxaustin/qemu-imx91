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
 * register (VERID/PARAM, self-clearing reset bits) plus a functional transmit
 * FIFO: words pushed to TDR0 are clocked out at the audio word rate once the
 * transmitter is enabled, maintaining the request/warning/error flags and the
 * FIFO-request interrupt the driver and (future) eDMA datapath rely on.
 */

#ifndef IMX93_SAI_H
#define IMX93_SAI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX93_SAI "imx93.sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SaiState, IMX93_SAI)

#define IMX93_SAI_SIZE   0x10000
/* Registers run from VERID (0x00) to MDIV (0x104); cover a little past that. */
#define IMX93_SAI_REGS   (0x108 / 4)

/* PARAM reports WPF=7 -> a 128-word transmit FIFO per data line. */
#define IMX93_SAI_FIFO_DEPTH 128

struct IMX93SaiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq dma_req;           /* TX FIFO-needs-data request to the eDMA */
    uint32_t regs[IMX93_SAI_REGS];

    /* Transmit FIFO (data line 0). */
    QEMUTimer *tx_timer;
    uint32_t tx_fifo[IMX93_SAI_FIFO_DEPTH];
    uint32_t tx_rptr;           /* read (transmit) pointer */
    uint32_t tx_wptr;           /* write (TDR0) pointer */
    uint32_t tx_count;          /* words currently in the FIFO */
    uint64_t tx_words;          /* total words clocked out (bookkeeping) */
};

#endif /* IMX93_SAI_H */
