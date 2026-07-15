/*
 * Wolfson/Cirrus WM8962 audio codec (I2C, register-file model)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The WM8962 is the i.MX93 EVK's headphone/speaker/mic codec, on the SAI3
 * audio link. This is a read-what-you-write register-file model: enough for
 * the Linux wm8962 driver to probe and the fsl-asoc-card "wm8962-audio" card
 * to register. The only value that must be exact is the device-ID register
 * (SOFTWARE_RESET == 0x6243); the driver's FLL/DC-servo handshakes happen at
 * stream time, which this model does not drive (no audio backend).
 *
 * Unlike the trivial SMBus regdev, the WM8962 uses regmap-i2c with 16-bit
 * register addresses and 16-bit big-endian values, so the wire format is:
 *   write: [addr_hi][addr_lo] [val_hi][val_lo] ...   (auto-incrementing)
 *   read:  (write [addr_hi][addr_lo]) then repeated-start read [val_hi][val_lo]
 */

#include "qemu/osdep.h"
#include "hw/audio/wm8962.h"
#include "hw/i2c/i2c.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define WM8962_SOFTWARE_RESET   0x0f
/*
 * ⭐ THE CODEC IS THE BIT-CLOCK MASTER OF THE SAI3 CARD, SO THE SAMPLE RATE LIVES HERE.
 *
 * On this board the SAI is the bit-clock CONSUMER (TCR2.BCD_MSTR clear) -- the wm8962
 * generates BCLK/LRCLK -- so the rate the guest is playing is NOT recoverable from the
 * SAI's own divider registers.  It is programmed into R27 (ADDITIONAL_CONTROL_3) here,
 * SR field [2:0] plus INT_MODE [4], per fsl wm8962.c sr_vals[].  This model decodes it
 * and emits it on a Clock the SAI paces from.  Without this the SAI paced at a fixed
 * 48 kHz and a 16 kHz stream played 3x too fast.  (Credit 93emulator, who found and
 * fixed exactly this on the identical codec.)
 */
#define WM8962_ADDITIONAL_CONTROL_3  0x1b
#define WM8962_ADCTL3_SR_MASK        0x0007
#define WM8962_ADCTL3_INT_MODE       0x0010
/* Silicon reset default: SR=0 + INT_MODE => 48 kHz.  NOT 0 -- 0 would be 44.1 kHz. */
#define WM8962_ADCTL3_RESET          0x0010

static uint32_t wm8962_decode_rate(uint16_t adctl3)
{
    int sr = adctl3 & WM8962_ADCTL3_SR_MASK;
    int intm = !!(adctl3 & WM8962_ADCTL3_INT_MODE);

    switch (sr) {
    case 0: return intm ? 48000 : 44100;
    case 1: return 32000;
    case 2: return intm ? 24000 : 22050;
    case 3: return 16000;
    /*
     * SR=4 is {11025, 12000} and NEITHER is divisible by 8000, so INT_MODE cannot
     * disambiguate them (93's observation).  We DECLARE the ambiguity by picking the
     * common member rather than inventing a false certainty; the SAI3 test rates
     * (48000, 16000) are both unambiguous.
     */
    case 4: return 12000;
    case 5: return 8000;
    case 6: return intm ? 96000 : 88200;
    default: return 0;
    }
}
#define WM8962_DEVICE_ID        0x6243
#define WM8962_NUM_REGS         0x5294  /* WM8962_MAX_REGISTER + 1 */

OBJECT_DECLARE_SIMPLE_TYPE(Wm8962State, WM8962)

struct Wm8962State {
    I2CSlave parent_obj;

    uint16_t regs[WM8962_NUM_REGS];
    uint16_t ptr;           /* current register pointer */
    int      wphase;        /* bytes seen since I2C_START_SEND */
    int      rphase;        /* 0: next read byte is value high, 1: low */
    uint8_t  addr_hi;       /* first address byte (pending) */
    uint8_t  val_hi;        /* first value byte (pending) */

    /* Sample rate decoded from R27, published for the SAI to pace from. */
    Clock   *rate_out;
};

static void wm8962_publish_rate(Wm8962State *s)
{
    uint32_t fs = wm8962_decode_rate(s->regs[WM8962_ADDITIONAL_CONTROL_3]);

    clock_set_hz(s->rate_out, fs);
    clock_propagate(s->rate_out);
}

static int wm8962_event(I2CSlave *i2c, enum i2c_event event)
{
    Wm8962State *s = WM8962(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->wphase = 0;      /* address bytes come first */
        break;
    case I2C_START_RECV:
        s->rphase = 0;      /* read uses the pointer the preceding write set */
        break;
    default:
        break;
    }
    return 0;
}

static int wm8962_send(I2CSlave *i2c, uint8_t data)
{
    Wm8962State *s = WM8962(i2c);

    if (s->wphase == 0) {
        s->addr_hi = data;
    } else if (s->wphase == 1) {
        s->ptr = (s->addr_hi << 8) | data;
    } else if (((s->wphase - 2) & 1) == 0) {
        s->val_hi = data;
    } else {
        if (s->ptr < WM8962_NUM_REGS) {
            s->regs[s->ptr] = (s->val_hi << 8) | data;
            if (s->ptr == WM8962_ADDITIONAL_CONTROL_3) {
                wm8962_publish_rate(s);   /* the guest just set the sample rate */
            }
        }
        s->ptr++;
    }
    s->wphase++;
    return 0;
}

static uint8_t wm8962_recv(I2CSlave *i2c)
{
    Wm8962State *s = WM8962(i2c);
    uint16_t val = s->ptr < WM8962_NUM_REGS ? s->regs[s->ptr] : 0;

    if (s->rphase == 0) {
        s->rphase = 1;
        return val >> 8;
    }
    s->rphase = 0;
    s->ptr++;
    return val & 0xff;
}

static void wm8962_reset(DeviceState *dev)
{
    Wm8962State *s = WM8962(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[WM8962_SOFTWARE_RESET] = WM8962_DEVICE_ID;
    /*
     * R27 comes up at its silicon default (48 kHz), and the rate is PUBLISHED at reset.
     * This is load-bearing: at 48 kHz the driver's regmap_update_bits writes the reset
     * value, sees no change, and issues NO I2C write -- so if the rate were only
     * published on write, the SAI would never learn it.  93emulator's finding, on this
     * codec: a default that equals the answer is a green light with no witness behind it.
     */
    s->regs[WM8962_ADDITIONAL_CONTROL_3] = WM8962_ADCTL3_RESET;
    s->ptr = 0;
    s->wphase = 0;
    s->rphase = 0;
    if (s->rate_out) {
        wm8962_publish_rate(s);
    }
}

static const VMStateDescription vmstate_wm8962 = {
    .name = TYPE_WM8962,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Wm8962State),
        VMSTATE_UINT16_ARRAY(regs, Wm8962State, WM8962_NUM_REGS),
        VMSTATE_UINT16(ptr, Wm8962State),
        VMSTATE_INT32(wphase, Wm8962State),
        VMSTATE_INT32(rphase, Wm8962State),
        VMSTATE_UINT8(addr_hi, Wm8962State),
        VMSTATE_UINT8(val_hi, Wm8962State),
        VMSTATE_END_OF_LIST()
    },
};

static void wm8962_init(Object *obj)
{
    Wm8962State *s = WM8962(obj);

    s->rate_out = qdev_init_clock_out(DEVICE(obj), "rate");
}

static void wm8962_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "Wolfson WM8962 audio codec";
    dc->vmsd = &vmstate_wm8962;
    device_class_set_legacy_reset(dc, wm8962_reset);
    sc->event = wm8962_event;
    sc->recv = wm8962_recv;
    sc->send = wm8962_send;
}

static const TypeInfo wm8962_types[] = {
    {
        .name          = TYPE_WM8962,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Wm8962State),
        .instance_init = wm8962_init,
        .class_init    = wm8962_class_init,
    },
};

DEFINE_TYPES(wm8962_types)
