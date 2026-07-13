/*
 * NXP i.MX 91 Temperature Monitor (u_temp_anamix, "fsl,imx91-tmu")
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ⭐ THE i.MX 91 HAS A DIFFERENT TEMPERATURE SENSOR TO THE i.MX 93, AND WE WERE
 *    MODELLING THE 93's.
 *
 * This machine mapped hw/misc/imx93_tmu.c -- a TMR/TMSR/TIER/TRITSR register file --
 * at 4448_2000h.  But the i.MX 91's device tree says `compatible = "fsl,imx91-tmu"`,
 * which binds drivers/thermal/imx91_thermal.c, and THAT driver speaks an entirely
 * different block (the RM calls it u_temp_anamix):
 *
 *     CTRL0  @0x000 (+SET/CLR/TOG)      STAT0 @0x010  DRDY0_IF = bit 16
 *     DATA0  @0x020  -- read as 16 bits, in units of 1/64 degree C
 *     CTRL1  @0x200 (+SET/CLR/TOG)      EN=31  START=30  STOP=29
 *     PERIOD_CTRL @0x270   REF_DIV @0x280   PUD_ST_CTRL @0x2B0
 *     TRIM1 @0x2E0  TRIM2 @0x2F0
 *
 * The driver ENABLES and STARTS the sensor exclusively through the SET/CLR aliases:
 *
 *     writel(CTRL1_EN,    base + CTRL1_SET);
 *     writel(CTRL1_START, base + CTRL1_SET);
 *
 * so on the old model every one of those writes vanished, DRDY never asserted, and
 * imx91_tmu_get_temp()'s 40 ms poll timed out.  The result, measured in the guest:
 *
 *     # cat /sys/class/thermal/thermal_zone0/temp
 *     cat: read error: No data available
 *
 * THE i.MX 91's ONLY TEMPERATURE SENSOR DID NOT WORK.  The thermal zone registered,
 * the driver bound, and nothing ever read a degree.  It was found by sweeping every
 * block in the chip for SET/CLR/TOG aliases the model was silently swallowing --
 * which is the same bug as the CCM's CONTROL_SET and the ANATOP's DIV_SET, in its
 * third and worst home.
 *
 *     A MODEL INHERITED FROM A NEIGHBOURING CHIP IS A MODEL OF THAT CHIP.
 *
 * ⚠ DRDY IS NOT A FABRICATED READY BIT.  It asserts only when the sensor is ENABLED
 * and a conversion has been requested (START, or a periodic measurement mode).  A
 * guest that reads DATA0 without enabling the sensor gets NO DATA, which is what
 * silicon does -- and is the opposite of the always-ready bits this fleet has spent
 * the week digging out of its own models.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx91_tmu.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

#define TMU_CTRL0           0x000   /* +0x04/08/0c: SET / CLR / TOG */
#define TMU_STAT0           0x010   /* +0x14/18/1c */
#define TMU_DATA0           0x020   /* +0x24/28/2c */
#define TMU_THR_CTRL01      0x030
#define TMU_THR_CTRL23      0x040
#define TMU_CTRL1           0x200   /* +0x204/208/20c */
#define TMU_PERIOD_CTRL     0x270
#define TMU_REF_DIV         0x280
#define TMU_PUD_ST_CTRL     0x2b0
#define TMU_TRIM1           0x2e0
#define TMU_TRIM2           0x2f0

/* CTRL1 */
#define CTRL1_EN            (1u << 31)
#define CTRL1_START         (1u << 30)
#define CTRL1_STOP          (1u << 29)
#define CTRL1_MEAS_MODE     (3u << 24)      /* 0 = single one-shot, 2 = periodic */

/* STAT0 */
#define STAT0_DRDY0_IF      (1u << 16)

/* Reset values, from IMX91RM.pdf rev 5, asserted by tests/imx91-reset-values. */
#define CTRL0_RESET         0x00003000
#define STAT0_RESET         0x80000000
#define REF_DIV_RESET       0x80000000

/* Registers that carry SET/CLR/TOG aliases at +4 / +8 / +C. */
static bool tmu_is_alias(hwaddr offset, hwaddr *base)
{
    switch (offset & ~0xfull) {
    case TMU_CTRL0:
    case TMU_STAT0:
    case TMU_DATA0:
    case TMU_CTRL1:
        *base = offset & ~0xfull;
        return true;
    default:
        return false;
    }
}

/*
 * DATA0 is what the driver actually reads, and it reads it 16 bits wide:
 *
 *     data = readw(base + DATA0);
 *     *temp = data * 1000LL / 64LL;      // millidegrees
 *
 * so the register holds the temperature in units of 1/64 degree C.
 */
static uint16_t tmu_data0(IMX91TmuState *s)
{
    return (int16_t)((int64_t)s->temperature * 64 / 1000);
}

/* Has the guest actually asked for a measurement? */
static bool tmu_converting(IMX91TmuState *s)
{
    uint32_t ctrl1 = s->regs[TMU_CTRL1 / 4];

    if (!(ctrl1 & CTRL1_EN) || (ctrl1 & CTRL1_STOP)) {
        return false;
    }
    /* A one-shot START, or a periodic measurement mode, produces samples. */
    return (ctrl1 & CTRL1_START) || (ctrl1 & CTRL1_MEAS_MODE);
}

static uint64_t imx91_tmu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX91TmuState *s = opaque;
    hwaddr base;

    if (tmu_is_alias(offset, &base)) {
        offset = base;      /* CTRL0/STAT0/DATA0/CTRL1 + SET/CLR/TOG all read the reg */
    }

    switch (offset) {
    case TMU_STAT0:
        /*
         * DRDY asserts only once the guest has ENABLED the sensor and asked for a
         * conversion.  Not a fabricated ready bit: a guest that never enables the
         * sensor polls forever and times out, exactly as it would on silicon.
         */
        return s->regs[TMU_STAT0 / 4] |
               (tmu_converting(s) ? STAT0_DRDY0_IF : 0);

    case TMU_DATA0:
        if (!tmu_converting(s)) {
            return 0;
        }
        return tmu_data0(s);

    default:
        return s->regs[offset / 4];
    }
}

static void imx91_tmu_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX91TmuState *s = opaque;
    hwaddr base;

    if (tmu_is_alias(offset, &base)) {
        uint32_t *p = &s->regs[base / 4];

        switch (offset & 0xc) {
        case 0x0: *p  = value; break;
        case 0x4: *p |= value; break;       /* SET  -- how the driver enables it */
        case 0x8: *p &= ~value; break;      /* CLR */
        case 0xc: *p ^= value; break;       /* TOG */
        }

        /*
         * START and STOP are opposite REQUESTS, and the driver issues BOTH through
         * the SET alias:
         *
         *     writel(CTRL1_STOP,  base + CTRL1_SET);   // at probe
         *     ...
         *     writel(CTRL1_START, base + CTRL1_SET);   // once configured
         *
         * so a naive OR leaves both latched forever.  Resolve from THE VALUE JUST
         * WRITTEN, not from the state it landed in -- reading the conflict out of
         * the latch answers "which bit is set", when the question is "what did the
         * guest just ask for".
         */
        if (base == TMU_CTRL1 && (offset & 0xc) == 0x4) {
            if (value & CTRL1_START) {
                *p &= ~CTRL1_STOP;
            }
            if (value & CTRL1_STOP) {
                *p &= ~CTRL1_START;
            }
        }
        return;
    }

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx91_tmu_ops = {
    .read = imx91_tmu_read,
    .write = imx91_tmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        /* get_temp() reads DATA0 with readw_relaxed(): 16-bit access is required. */
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
};

static void imx91_tmu_reset(DeviceState *dev)
{
    IMX91TmuState *s = IMX91_TMU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[TMU_CTRL0 / 4]   = CTRL0_RESET;
    s->regs[TMU_STAT0 / 4]   = STAT0_RESET;
    s->regs[TMU_REF_DIV / 4] = REF_DIV_RESET;
}

static void imx91_tmu_init(Object *obj)
{
    IMX91TmuState *s = IMX91_TMU(obj);

    memory_region_init_io(&s->iomem, obj, &imx91_tmu_ops, s,
                          TYPE_IMX91_TMU, IMX91_TMU_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property imx91_tmu_properties[] = {
    /* Junction temperature in millidegrees C.  25 C is a plausible idle die. */
    DEFINE_PROP_INT32("temperature", IMX91TmuState, temperature, 25000),
};

static const VMStateDescription vmstate_imx91_tmu = {
    .name = TYPE_IMX91_TMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX91TmuState, IMX91_TMU_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx91_tmu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 91 Temperature Monitor (u_temp_anamix)";
    device_class_set_legacy_reset(dc, imx91_tmu_reset);
    device_class_set_props(dc, imx91_tmu_properties);
    dc->vmsd = &vmstate_imx91_tmu;
}

static const TypeInfo imx91_tmu_types[] = {
    {
        .name           = TYPE_IMX91_TMU,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX91TmuState),
        .instance_init  = imx91_tmu_init,
        .class_init     = imx91_tmu_class_init,
    },
};

DEFINE_TYPES(imx91_tmu_types)
