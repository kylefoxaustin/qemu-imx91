/*
 * Trivial SMBus register-file I2C slave (i.MX 93 board bring-up helper)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A minimal I2C target that implements the standard "write register pointer,
 * then read/write data" SMBus access pattern over a 256-byte read-what-you-
 * write register file. It is not a faithful model of any specific chip; it
 * exists so the board's I2C client drivers probe and register their
 * resources instead of deferring forever:
 *   - the PCA9451A PMIC (0x25): reg 0x00 (DEV_ID) must read with high nibble
 *     0x9, set via the "reg0" property; the regulator driver then registers
 *     its BUCK/LDO regulators (unblocking uSDHC, etc.);
 *   - the PCAL6524 GPIO expander (0x22): the pca953x driver registers a
 *     gpiochip, which resolves the FEC PHY reset-gpio.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_IMX93_I2C_REGDEV "imx93.i2c-regdev"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93I2CRegdevState, IMX93_I2C_REGDEV)

struct IMX93I2CRegdevState {
    I2CSlave parent_obj;

    uint8_t regs[256];
    uint8_t ptr;
    bool    have_ptr;
    uint8_t reg0;       /* reset value of register 0 (e.g. a device id) */
};

static int imx93_i2c_regdev_event(I2CSlave *i2c, enum i2c_event event)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);

    if (event == I2C_START_SEND) {
        s->have_ptr = false;    /* next byte is the register pointer */
    }
    return 0;
}

static int imx93_i2c_regdev_send(I2CSlave *i2c, uint8_t data)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);

    if (!s->have_ptr) {
        s->ptr = data;
        s->have_ptr = true;
    } else {
        s->regs[s->ptr++] = data;
    }
    return 0;
}

static uint8_t imx93_i2c_regdev_recv(I2CSlave *i2c)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(i2c);
    uint8_t val = s->regs[s->ptr];

    s->ptr++;
    return val;
}

static void imx93_i2c_regdev_reset(DeviceState *dev)
{
    IMX93I2CRegdevState *s = IMX93_I2C_REGDEV(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = s->reg0;
    s->ptr = 0;
    s->have_ptr = false;
}

static const Property imx93_i2c_regdev_props[] = {
    DEFINE_PROP_UINT8("reg0", IMX93I2CRegdevState, reg0, 0),
};

static const VMStateDescription vmstate_imx93_i2c_regdev = {
    .name = TYPE_IMX93_I2C_REGDEV,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, IMX93I2CRegdevState),
        VMSTATE_UINT8_ARRAY(regs, IMX93I2CRegdevState, 256),
        VMSTATE_UINT8(ptr, IMX93I2CRegdevState),
        VMSTATE_BOOL(have_ptr, IMX93I2CRegdevState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_i2c_regdev_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    dc->desc = "i.MX 93 trivial I2C register device";
    device_class_set_legacy_reset(dc, imx93_i2c_regdev_reset);
    dc->vmsd = &vmstate_imx93_i2c_regdev;
    device_class_set_props(dc, imx93_i2c_regdev_props);
    sc->event = imx93_i2c_regdev_event;
    sc->recv = imx93_i2c_regdev_recv;
    sc->send = imx93_i2c_regdev_send;
}

static const TypeInfo imx93_i2c_regdev_types[] = {
    {
        .name           = TYPE_IMX93_I2C_REGDEV,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(IMX93I2CRegdevState),
        .class_init     = imx93_i2c_regdev_class_init,
    },
};

DEFINE_TYPES(imx93_i2c_regdev_types)
