/*
 * NXP i.MX 93 System Counter (SYS_CTR)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A free-running 24 MHz system counter (the timer-imx-sysctr clocksource reads
 * CNTCV_LO/HI) plus the compare-frame clockevent: writing CMPCV + enabling
 * CMPCR arms an interrupt (GIC SPI 74) that fires when the counter reaches the
 * compare value. The counter tracks the virtual clock so it agrees with the
 * Arm generic timer.
 */

#include "qemu/osdep.h"
#include "hw/timer/imx93_sysctr.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"

/*
 * ⭐ CNTFID0 IS THE COUNTER'S OWN FREQUENCY, AND IT IS DERIVED FROM THE COUNTER'S CLOCK.
 *
 * CNTFID0 is the register a guest reads to learn the tick rate and then DIVIDES BY to turn
 * counts into wall-clock.  It is not a constant here: it returns clock_get_hz() of the same
 * reference clock the counter ticks off (sysctr_hz()), so the reported frequency and the
 * real tick rate are ONE value and cannot drift apart.  (They used to be two hardcoded
 * 24 MHz constants -- SYSCTR_HZ and a CNTFID0_RESET -- that agreed only because both were
 * fabricated; change one and the guest's arithmetic silently went wrong.  On a FREQUENCY
 * register a wrong-but-plausible value is a divisor error nobody sees.)
 */
#define CNTSR       0x00004
#define CNTFID0     0x00020
#define CNTFID1     0x00024

#define CNTSR_RESET     0x00000100
#define CNTFID1_RESET   0x00000200      /* CNTFID0 comes from the clock; see above */

#define CNTCV_LO    0x00008
#define CNTCV_HI    0x0000c
#define CMP_OFFSET  0x10000
#define CMPCV_LO    (CMP_OFFSET + 0x20)
#define CMPCV_HI    (CMP_OFFSET + 0x24)
#define CMPCR       (CMP_OFFSET + 0x2c)

#define SYS_CTR_EN       0x1
#define SYS_CTR_IRQ_MASK 0x2

/*
 * The counter's frequency comes from its wired reference clock, not a constant.
 * Fall back to 24 MHz only if unclocked (defensive -- the board always wires it),
 * so an unwired sysctr still ticks rather than dividing by zero.
 */
static uint32_t sysctr_hz(IMX93SysctrState *s)
{
    uint64_t hz = s->clk ? clock_get_hz(s->clk) : 0;

    return hz ? (uint32_t)hz : 24000000;
}

static uint64_t sysctr_count(IMX93SysctrState *s)
{
    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), sysctr_hz(s),
                    NANOSECONDS_PER_SECOND);
}

static void sysctr_update(IMX93SysctrState *s)
{
    uint64_t now = sysctr_count(s);

    if (!(s->cmpcr & SYS_CTR_EN)) {
        timer_del(&s->cmp);
        qemu_set_irq(s->irq, 0);
        return;
    }
    if (s->cmpcv <= now) {
        timer_del(&s->cmp);
        qemu_set_irq(s->irq, !(s->cmpcr & SYS_CTR_IRQ_MASK));
    } else {
        int64_t fire = muldiv64(s->cmpcv, NANOSECONDS_PER_SECOND, sysctr_hz(s));

        qemu_set_irq(s->irq, 0);
        timer_mod(&s->cmp, fire);
    }
}

static void sysctr_fire(void *opaque)
{
    IMX93SysctrState *s = opaque;

    if (s->cmpcr & SYS_CTR_EN) {
        qemu_set_irq(s->irq, !(s->cmpcr & SYS_CTR_IRQ_MASK));
    }
}

static uint64_t sysctr_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93SysctrState *s = opaque;
    uint64_t cnt = sysctr_count(s);

    switch (offset) {
    case CNTSR:
        return CNTSR_RESET;
    case CNTFID0:
        return sysctr_hz(s);     /* the rate we ACTUALLY tick at -- one source of truth */
    case CNTFID1:
        return CNTFID1_RESET;
    case CNTCV_LO:
        return cnt & 0xffffffff;
    case CNTCV_HI:
        return (cnt >> 32) & 0xffffffff;
    case CMPCV_LO:
        return s->cmpcv & 0xffffffff;
    case CMPCV_HI:
        return (s->cmpcv >> 32) & 0xffffffff;
    case CMPCR:
        return s->cmpcr;
    default:
        return 0;
    }
}

static void sysctr_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    IMX93SysctrState *s = opaque;

    switch (offset) {
    case CMPCV_LO:
        s->cmpcv = (s->cmpcv & ~0xffffffffULL) | (value & 0xffffffff);
        sysctr_update(s);
        break;
    case CMPCV_HI:
        s->cmpcv = (s->cmpcv & 0xffffffffULL) | ((uint64_t)value << 32);
        sysctr_update(s);
        break;
    case CMPCR:
        s->cmpcr = value;
        sysctr_update(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps sysctr_ops = {
    .read = sysctr_read,
    .write = sysctr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void sysctr_reset(DeviceState *dev)
{
    IMX93SysctrState *s = IMX93_SYSCTR(dev);

    s->cmpcr = 0;
    s->cmpcv = 0;
    timer_del(&s->cmp);
}

static void sysctr_init(Object *obj)
{
    IMX93SysctrState *s = IMX93_SYSCTR(obj);

    /* Created here (not realize) so the board can wire it pre-realize. */
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void sysctr_realize(DeviceState *dev, Error **errp)
{
    IMX93SysctrState *s = IMX93_SYSCTR(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &sysctr_ops, s,
                          TYPE_IMX93_SYSCTR, IMX93_SYSCTR_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->cmp, QEMU_CLOCK_VIRTUAL, sysctr_fire, s);
}

static const VMStateDescription vmstate_sysctr = {
    .name = TYPE_IMX93_SYSCTR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cmpcr, IMX93SysctrState),
        VMSTATE_UINT64(cmpcv, IMX93SysctrState),
        VMSTATE_TIMER(cmp, IMX93SysctrState),
        VMSTATE_END_OF_LIST()
    },
};

static void sysctr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sysctr_realize;
    dc->vmsd = &vmstate_sysctr;
    device_class_set_legacy_reset(dc, sysctr_reset);
    dc->desc = "i.MX93 system counter";
}

static const TypeInfo sysctr_types[] = {
    {
        .name = TYPE_IMX93_SYSCTR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(IMX93SysctrState),
        .instance_init = sysctr_init,
        .class_init = sysctr_class_init,
    },
};

DEFINE_TYPES(sysctr_types)
