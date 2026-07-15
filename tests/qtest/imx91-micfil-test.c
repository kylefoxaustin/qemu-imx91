/*
 * QTest for the i.MX91 MICFIL (PDM microphone) capture FIFO.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives the MICFIL capture path the way the fsl-micfil driver / eDMA datapath
 * does, with no kernel: programs a FIFO watermark, selects DMA, and enables the
 * module (CTRL1.PDMIEN). The model then clocks a synthesised sawtooth into the
 * data FIFO at the audio word rate. Advances the virtual clock and reads the
 * samples out of DATACH0 - they must be the ramp (non-silent, varying). The
 * eDMA drains DATACH0 the same way in a real capture.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define MICFIL_BASE     0x44520000ULL

#define MICFIL_CTRL1    0x00
#define MICFIL_FIFO_CTRL 0x10
#define MICFIL_VERID    0x84
#define MICFIL_PARAM    0x88
#define MICFIL_DATACH0  0x24

#define CTRL1_MDIS      (1u << 31)
#define CTRL1_PDMIEN    (1u << 29)
#define CTRL1_SRES      (1u << 27)
#define CTRL1_DISEL_DMA (1u << 24)
#define CTRL1_CHEN0     (1u << 0)

/* Must match MICFIL_WORD_NS in the model (48 kHz). */
#define WORD_NS         (1000000000LL / 48000)
#define WATERMARK       7

static uint32_t rd(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, MICFIL_BASE + off);
}

static void wr(QTestState *qts, uint64_t off, uint32_t v)
{
    qtest_writel(qts, MICFIL_BASE + off, v);
}

static void test_pdm_capture(void)
{
    QTestState *qts = qtest_init("-machine imx91-11x11-evk -display none");
    uint32_t prev = 0;
    int i, nonzero = 0, varied = 0;

    /* Identification must read back so the driver would probe. */
    g_assert_cmphex(rd(qts, MICFIL_VERID), ==, 0x020f0000);  /* RM: was fabricated 0x01000000 */
    g_assert_cmphex(rd(qts, MICFIL_PARAM), ==, 0x00000154);  /* RM: FIFO 32 + FIL_OUT_WIDTH; was 0x34 (FIFO 8) */

    /* Program a watermark, then enable channel 0, DMA, and the module. */
    wr(qts, MICFIL_FIFO_CTRL, WATERMARK);
    wr(qts, MICFIL_CTRL1, CTRL1_PDMIEN | CTRL1_DISEL_DMA | CTRL1_CHEN0);

    /* Clock in ~64 samples at the word rate. */
    qtest_clock_step(qts, 64 * WORD_NS + WORD_NS / 2);

    /* Read words out of DATACH0: the synthesised sawtooth. */
    for (i = 0; i < 32; i++) {
        uint32_t w = rd(qts, MICFIL_DATACH0);

        if (w) {
            nonzero++;
        }
        if (i && w != prev) {
            varied++;                    /* ramping, not stuck */
        }
        prev = w;
    }
    g_assert_cmpint(nonzero, >, 16);
    g_assert_cmpint(varied, >, 8);

    /* Software reset drains the FIFO; a read still rolls (synth-on-empty). */
    wr(qts, MICFIL_CTRL1, CTRL1_PDMIEN | CTRL1_DISEL_DMA | CTRL1_CHEN0 |
                          CTRL1_SRES);
    g_assert_cmphex(rd(qts, MICFIL_DATACH0), !=, 0);

    /* Disabling the module (MDIS) silences DATACH0. */
    wr(qts, MICFIL_CTRL1, CTRL1_MDIS);
    g_assert_cmphex(rd(qts, MICFIL_DATACH0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/imx91/micfil/pdm-capture", test_pdm_capture);
    return g_test_run();
}
