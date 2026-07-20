/*
 * NXP i.MX 93 Timer/PWM Module (TPM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_IMX93_TPM_H
#define HW_TIMER_IMX93_TPM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_IMX93_TPM "imx93.tpm"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93TpmState, IMX93_TPM)

#define IMX93_TPM_SIZE      0x10000
/*
 * ⭐ FOUR channels, not six.  PARAM.CHAN = 4 in IMX91RM.pdf rev 5 (§52.7.1.3), and
 *    pwm-imx-tpm reads that field and registers EXACTLY THAT MANY PWM channels:
 *
 *        val  = readl(base + PWM_IMX_TPM_PARAM);
 *        npwm = FIELD_GET(PWM_IMX_TPM_PARAM_CHAN, val);
 *
 * This model returned 6 -- a number somebody typed -- so Linux exposed pwm4 and
 * pwm5, which DO NOT EXIST ON THE SILICON.  They work here and fail on hardware,
 * which is the worst direction for a model to be wrong in.  An invented capability
 * is a promise the emulator makes on the chip's behalf.
 */
#define IMX93_TPM_CHANNELS  4

struct IMX93TpmState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    Clock *clk;             /* module clock from the CCM.  NO DEFAULT. */
    int64_t base_ns;        /* virtual time at which the counter last started */
    uint64_t clk_hz;        /* last-seen clock rate, to spot the gate 1<->0 edge */
    uint32_t held_cnt;      /* CNT frozen while the clock is gated (flip-flops hold) */
    uint32_t sc;            /* status/control (clock mode + prescaler) */
    uint32_t mod;           /* modulo (period) */
    uint32_t cnsc[IMX93_TPM_CHANNELS];
    uint32_t cnv[IMX93_TPM_CHANNELS];
};

#endif /* HW_TIMER_IMX93_TPM_H */
