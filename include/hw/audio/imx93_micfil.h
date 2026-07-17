/*
 * NXP i.MX 93 MICFIL (PDM microphone interface)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The MICFIL is the i.MX93 PDM microphone front-end: it decimates PDM mic
 * streams into a FIFO that eDMA drains. This model carries the register file
 * the fsl-micfil driver needs to probe and the ASoC "micfil" card to register
 * (a correct VERID/PARAM and a self-clearing software-reset bit), and - with no
 * physical PDM mic wired - synthesises a captured sample stream into a data
 * FIFO so a real `arecord` reads non-silent audio: once the module is enabled
 * (CTRL1.PDMIEN, CTRL1.MDIS clear) it clocks samples in at the audio rate and,
 * with CTRL1.DISEL=DMA, requests an eDMA drain of DATACH0 as the FIFO fills.
 */

#ifndef IMX93_MICFIL_H
#define IMX93_MICFIL_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_IMX93_MICFIL "imx93.micfil"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93MicfilState, IMX93_MICFIL)

#define IMX93_MICFIL_SIZE   0x10000
/* Registers run from CTRL1 (0x00) to VAD0_ZCD (0xa8). */
#define IMX93_MICFIL_REGS   (0xac / 4)
/* CTRL1.DISEL=IRQ / error / VAD share four interrupt lines. */
#define IMX93_MICFIL_IRQS   4
/* Deeper than the i.MX93 FIFO (32) so the fill can exceed the watermark. */
/*
 * 32, because that is what the silicon has -- and because the model, its own PARAM
 * register, and the driver's soc_data used to give THREE DIFFERENT ANSWERS for the
 * depth of one FIFO: the array below was 64, PARAM advertised 8 (FIFO_PTRWID=3), and
 * fsl_micfil_imx93.fifo_depth is 32.  A capability register that disagrees with the
 * implementation it describes is not an under-report.  It is a THIRD OPINION.
 */
/*
 * ⭐ THE CAPABILITY FIELD IS THE SOURCE OF TRUTH; THE IMPLEMENTATION IS DERIVED FROM IT.
 *
 * mcxn947qemu: "A CAPABILITY REGISTER THAT IS A *CONSTANT* CAN DRIFT FROM THE THING IT
 * DESCRIBES.  ONE *COMPUTED FROM* IT CANNOT."  They found their SAI's PARAM was right only
 * BY LUCK, with a comment claiming FIFO=32 above a value that encoded 8 -- the comment had
 * already drifted from the value it was describing, and nobody noticed for months.
 *
 * That lands on me: I "fixed" MICFIL by making three numbers agree BY HAND (the array, the
 * PARAM constant, and the RM).  Hand-agreement is exactly what drifts -- change the depth
 * and PARAM silently lies again, rebuilding the bug I had just fixed.
 *
 * So PARAM's FIFO_PTRWID is now PRIMARY and the array size is derived from it.  That is the
 * stronger direction: the register field is an EXPONENT (the driver does
 * `1 << FIFO_PTRWID`), so deriving the depth from it makes a NON-POWER-OF-TWO DEPTH
 * UNREPRESENTABLE rather than merely wrong.  You cannot write a depth this register could
 * not have described.
 */
#define IMX93_MICFIL_FIFO_PTRWID  5                                 /* PARAM[7:4] */
#define IMX93_MICFIL_FIFO_DEPTH   (1u << IMX93_MICFIL_FIFO_PTRWID)  /* = 32, as silicon */
#define IMX93_MICFIL_NPAIR        4                                 /* PARAM[3:0]: 8 mics */

struct IMX93MicfilState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    Clock *mclk;                /* PDM root clock; sets the capture sample rate */
    qemu_irq irq[IMX93_MICFIL_IRQS];
    qemu_irq dma_req;           /* FIFO-has-data request to the eDMA */
    uint32_t regs[IMX93_MICFIL_REGS];

    /* Synthesised capture FIFO drained via DATACH0. */
    QEMUTimer *rx_timer;
    uint32_t rx_fifo[IMX93_MICFIL_FIFO_DEPTH];
    uint32_t rx_rptr;
    uint32_t rx_wptr;
    uint32_t rx_count;          /* words currently in the FIFO */
    uint64_t rx_words;          /* total samples clocked in (drives waveform) */
};

#endif /* IMX93_MICFIL_H */
