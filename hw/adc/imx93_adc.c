/*
 * NXP i.MX 93 SAR-ADC
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models enough of the i.MX93 SAR-ADC for the imx93_adc driver.
 * Self-calibration (MCR.CALSTART) completes immediately, with
 * MSR.CALBUSY/CALFAIL clear. A normal conversion (MCR.NSTART) sets the
 * end-of-conversion status (ISR) and raises the EOC interrupt the driver waits
 * on; PCDRn then returns the per-channel conversion value.
 *
 * Fidelity: there is no analog pin to sample in emulation, so the conversion
 * value comes from the operator - each channel is a read/write QOM property
 * "adc-ch0".."adc-ch7" settable at runtime via QMP qom-set (the model's analog
 * of the board's pin voltage). Whatever the operator injects is what the guest
 * reads back, so ADC-consuming code sees a faithful, controllable datapath
 * rather than a hidden constant (the previous fixed mid-scale 0x800 was a
 * silent-wrong: a plausible value with a success flag and no way to tell or
 * drive it). The default is a distinct-per-channel test pattern
 * (0x100+ch*0x111) so an un-driven channel is still deterministic. Mirrors the
 * i.MX 95 ADC fix.
 */

#include "qemu/osdep.h"
#include "hw/adc/imx93_adc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define ADC_MCR     0x00
#define ADC_MSR     0x04
#define ADC_ISR     0x10
#define ADC_IMR     0x20
#define ADC_NCMR0   0xa4
#define ADC_PCDR0   0x100

#define MCR_NSTART      (1u << 24)
#define MCR_CALSTART    (1u << 14)
#define MCR_PWDN        (1u << 0)

/* MSR.ADCSTATUS[2:0] codes the driver polls for. */
#define MSR_STATUS_IDLE         0
#define MSR_STATUS_POWER_DOWN   1

#define ISR_ECH         (1u << 0)
#define ISR_EOC         (1u << 1)

#define PCDR_CDATA_MASK 0xfff

static void adc_update_irq(IMX93AdcState *s)
{
    qemu_set_irq(s->irq, !!(s->isr & s->imr & (ISR_EOC | ISR_ECH)));
}

/*
 * Default per-channel value when the operator hasn't driven a channel;
 * distinct per channel so an un-driven channel is still deterministic.
 */
static uint32_t adc_default(int ch)
{
    return (0x100 + ch * 0x111) & PCDR_CDATA_MASK;
}

static void adc_convert(IMX93AdcState *s)
{
    /*
     * No analog source: the per-channel PCDRn already holds the operator-set
     * (or default) conversion value, so a scan just latches end-of-conversion
     * and raises the IRQ - it does NOT overwrite the value with a constant.
     */
    s->isr |= ISR_EOC | ISR_ECH;
    adc_update_irq(s);
}

static uint64_t adc_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93AdcState *s = opaque;

    switch (offset) {
    case ADC_MCR:
        return s->mcr;
    case ADC_MSR:
        /* Report power-down vs idle; never calibrating/busy/failed. */
        return (s->mcr & MCR_PWDN) ? MSR_STATUS_POWER_DOWN : MSR_STATUS_IDLE;
    case ADC_ISR:
        return s->isr;
    case ADC_IMR:
        return s->imr;
    case ADC_NCMR0:
        return s->ncmr0;
    default:
        if (offset >= ADC_PCDR0 && offset < ADC_PCDR0 + 4 * IMX93_ADC_NCH) {
            /* operator-injected conversion value (12-bit) */
            return s->pcdr[(offset - ADC_PCDR0) / 4] & PCDR_CDATA_MASK;
        }
        return 0;
    }
}

static void adc_write(void *opaque, hwaddr offset, uint64_t value,
                      unsigned size)
{
    IMX93AdcState *s = opaque;

    switch (offset) {
    case ADC_MCR:
        s->mcr = value & ~(MCR_NSTART | MCR_CALSTART);
        /* CALSTART self-completes; nothing to do (MSR reports idle). */
        if (value & MCR_NSTART) {
            adc_convert(s);
        }
        break;
    case ADC_ISR:
        s->isr &= ~value;       /* write-1-to-clear */
        adc_update_irq(s);
        break;
    case ADC_IMR:
        s->imr = value;
        adc_update_irq(s);
        break;
    case ADC_NCMR0:
        s->ncmr0 = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void adc_reset(DeviceState *dev)
{
    IMX93AdcState *s = IMX93_ADC(dev);
    int ch;

    s->mcr = MCR_PWDN;      /* powered down until the driver enables it */
    s->isr = 0;
    s->imr = 0;
    s->ncmr0 = 0;
    for (ch = 0; ch < IMX93_ADC_NCH; ch++) {
        s->pcdr[ch] = adc_default(ch);
    }
}

static void adc_init(Object *obj)
{
    IMX93AdcState *s = IMX93_ADC(obj);
    int ch;

    /*
     * Per-channel conversion value, settable at runtime via QMP qom-set
     * (e.g. qom-set <path> adc-ch3 0x555) - the operator drives the "voltage".
     */
    for (ch = 0; ch < IMX93_ADC_NCH; ch++) {
        char name[16];
        snprintf(name, sizeof(name), "adc-ch%d", ch);
        object_property_add_uint32_ptr(obj, name, &s->pcdr[ch],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static void adc_realize(DeviceState *dev, Error **errp)
{
    IMX93AdcState *s = IMX93_ADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &adc_ops, s,
                          TYPE_IMX93_ADC, IMX93_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_adc = {
    .name = TYPE_IMX93_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mcr, IMX93AdcState),
        VMSTATE_UINT32(isr, IMX93AdcState),
        VMSTATE_UINT32(imr, IMX93AdcState),
        VMSTATE_UINT32(ncmr0, IMX93AdcState),
        VMSTATE_UINT32_ARRAY(pcdr, IMX93AdcState, IMX93_ADC_NCH),
        VMSTATE_END_OF_LIST()
    },
};

static void adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = adc_realize;
    dc->vmsd = &vmstate_adc;
    device_class_set_legacy_reset(dc, adc_reset);
    dc->desc = "i.MX93 SAR-ADC";
}

static const TypeInfo adc_types[] = {
    {
        .name = TYPE_IMX93_ADC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93AdcState),
        .instance_init = adc_init,
        .class_init = adc_class_init,
    },
};

DEFINE_TYPES(adc_types)
