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
 * FIFO: words pushed to TDR0 (by the eDMA) are clocked out at the audio word
 * rate, maintaining the request/warning/error flags and FIFO-request interrupt
 * the driver relies on, requesting eDMA bursts as the FIFO drains, and handing
 * the played samples to the audio backend (so -audio captures the playback).
 */

#ifndef IMX93_SAI_H
#define IMX93_SAI_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/audio.h"

#define TYPE_IMX93_SAI "imx93.sai"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93SaiState, IMX93_SAI)

#define IMX93_SAI_SIZE   0x10000
/* Registers run from VERID (0x00) to MDIV (0x104); cover a little past that. */
#define IMX93_SAI_REGS   (0x108 / 4)

/* PARAM reports WPF=7 -> a 128-word transmit FIFO per data line. */
/* See the MICFIL header: the capability field is primary, the array is derived from it. */
#define IMX93_SAI_WPF        7                          /* PARAM[11:8] */
#define IMX93_SAI_FIFO_DEPTH (1u << IMX93_SAI_WPF)      /* = 128 words */
#define IMX93_SAI_SPF        5                          /* PARAM[19:16]: 32 slots/frame */
#define IMX93_SAI_DLN        4                          /* PARAM[3:0]:   4 datalines    */

/* Capture ring decoupling the FIFO drain from the audio backend's callback. */
#define IMX93_SAI_CAP_SIZE 16384

struct IMX93SaiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /*
     * The SAI's master clock -- the CCM SAI root, wired from the (now real)
     * anatop AUDIO_PLL -> CCM chain.  The bit clock, and therefore the sample
     * rate, is derived from THIS plus the guest's TCR2 divider and frame
     * geometry.  Before this was wired the model paced everything at a hardcoded
     * 48 kHz, so a 16 kHz stream played 3x too fast (436 Hz / 0.33 s instead of
     * 145 Hz / 1.0 s) -- a default that equalled the answer at 48 kHz only.
     */
    Clock *codec_rate;      /* sample rate published by the codec (wm8962 R27) */
    uint32_t tx_rate;           /* computed TX sample rate, 0 = not configured */
    uint32_t rx_rate;           /* computed RX sample rate */
    uint32_t voice_rate;        /* rate the backend voice is currently open at */
    int64_t  tx_word_ns;        /* ns per TX word (per-channel), from tx_rate    */
    int64_t  rx_word_ns;        /* ns per RX word, from rx_rate                  */
    qemu_irq dma_req_tx;        /* TX FIFO-needs-data request to the eDMA */
    qemu_irq dma_req_rx;        /* RX FIFO-has-data request to the eDMA */
    uint32_t regs[IMX93_SAI_REGS];

    /* Transmit FIFO (data line 0). */
    QEMUTimer *tx_timer;
    uint32_t tx_fifo[IMX93_SAI_FIFO_DEPTH];
    uint32_t tx_rptr;           /* read (transmit) pointer */
    uint32_t tx_wptr;           /* write (TDR0) pointer */
    uint32_t tx_count;          /* words currently in the FIFO */
    uint64_t tx_words;          /* total words clocked out (bookkeeping) */

    /*
     * Receive FIFO (data line 0) - capture. The receiver synthesises a sample
     * stream into this FIFO at the audio word rate; as it fills past the
     * watermark it requests an eDMA burst, which drains RDR0 into memory.
     */
    QEMUTimer *rx_timer;
    uint32_t rx_fifo[IMX93_SAI_FIFO_DEPTH];
    uint32_t rx_rptr;           /* read (RDR0 pop) pointer */
    uint32_t rx_wptr;           /* write (synthesis) pointer */
    uint32_t rx_count;          /* words currently in the FIFO */
    uint64_t rx_words;          /* total words captured (bookkeeping) */
    uint16_t rx_phase;          /* sawtooth-synthesis phase */

    /* Audio backend: clocked-out samples go to an -audiodev (e.g. wav). */
    AudioBackend *audio_be;
    SWVoiceOut *voice;
    bool      voice_active;
    uint8_t   cap[IMX93_SAI_CAP_SIZE];   /* played PCM awaiting the backend */
    uint32_t  cap_head;
    uint32_t  cap_count;
};

#endif /* IMX93_SAI_H */
