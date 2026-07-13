/*
 * NXP i.MX 93 Clock Control Module (CCM)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Functional register model of the i.MX 9 CCM.  The register layout is taken
 * from IMX91RM.pdf rev 5 (§7, CCM memory map) and is asserted every run by
 * tests/imx91-reset-values, whose golden is the reference manual.
 *
 *   CLOCK_ROOT  0x0000..0x2F7F, 0x80 per root (95 roots)
 *     +0x00 CONTROL : DIV[7:0], MUX[9:8], OFF[24]
 *     +0x04 SET  +0x08 CLR  +0x0C TOG : aliases onto CONTROL
 *     +0x20 STATUS0 : CHANGING[31], SLICE_BUSY[28] -- never busy here
 *     +0x30 AUTHEN (+ SET/CLR/TOG aliases)
 *
 *   GPR_SHARED 0x4800, GPR_PRIVATE 0x4C00, 0x40 per block (8 each)
 *     +0x00 value  +0x04 SET  +0x08 CLR  +0x0C TOG   +0x30 AUTHEN
 *
 *   OSCPLL 0x5000 (20), LPCG 0x8000 (127), 0x40 per gate
 *     +0x00 DIRECT : bit 0 = clock on.  RESET = 1 -- the gates come up RUNNING.
 *     +0x10 LPM0  +0x14 LPM1  +0x1C LPM_CUR
 *     +0x20 STATUS0 : bit 0 = running (follows DIRECT)
 *     +0x24 STATUS1 : per-domain status, resets to 0xFFFF
 *     +0x30 AUTHEN
 *
 * The earlier version of this model was written against the Linux drivers
 * rather than the manual, and put STATUS at +0x04 -- where CONTROL_SET
 * actually lives.  Guest writes through the SET alias were silently dropped
 * and the BUSY poll was reading a control register.  Both read paths returned
 * zero, so it booted, and every test stayed green.  The reset-value gate found
 * it; no test we author against our own model ever could have.
 *
 * ⚠ Still not modelled: this CCM produces NO FREQUENCIES.  It is a register
 * surface, so a guest computing a rate from MUX/DIV gets an answer this model
 * never checks.  That is a KNOWN, NAMED hole -- see the timer clocks, which
 * hardcode 24 MHz and cannot follow the tree -- and an honestly-documented
 * hole is still a hole.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_ccm.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Root region: below GATE_BASE, blocks are ROOT_STRIDE apart. */
#define CCM_GATE_BASE       0x8000
#define CCM_ROOT_STRIDE     0x80
#define CCM_GATE_STRIDE     0x40

/*
 * Block layout, taken from IMX91RM.pdf rev 5 (§7 CCM memory map) and asserted
 * by tests/imx91-reset-values.  The previous model placed STATUS at +0x04 and
 * treated it as read-only.  The manual puts CONTROL_SET there -- so every write
 * a guest made through the SET alias was SILENTLY DISCARDED, and the BUSY poll
 * was reading a control register.  Both read paths happened to return 0, which
 * is why the model booted and every test stayed green:
 *
 *     A RESULT THAT IS CORRECT BY LUCK IS A RESULT YOU HAVE NOT CHECKED.
 *
 * Note Linux polls BUSY at reg+0x4 (clk-composite-93.c STAT_OFFSET) -- i.e. at
 * CONTROL_SET, not at STATUS0.  That poll passes on silicon only because
 * CONTROL bit 28 is unused and reads 0; it passes here for the same reason.
 * We model the manual, not the driver's off-by-one.
 */
#define CCM_ROOT_CONTROL    0x00    /* +0x04/08/0c: SET / CLR / TOG aliases */
#define CCM_ROOT_STATUS0    0x20    /* CHANGING[31], SLICE_BUSY[28] */
#define CCM_AUTHEN_OFFSET   0x30    /* +0x34/38/3c: SET / CLR / TOG aliases */

/* LPCG / OSCPLL gate blocks (stride 0x40). */
#define CCM_GATE_DIRECT     0x00    /* bit 0 = clock on.  RESET = 1. */
#define CCM_GATE_LPM0       0x10
#define CCM_GATE_LPM1       0x14
#define CCM_GATE_LPM_CUR    0x1c
#define CCM_GATE_STATUS0    0x20    /* bit 0 = clock is on (follows DIRECT) */
#define CCM_GATE_STATUS1    0x24    /* per-domain status.  RESET = 0xffff. */

#define CCM_GATE_ON         0x1u
#define CCM_GATE_STATUS1_RS 0xffffu

/*
 * AUTHEN constant returned for every block: TrustZone non-secure access allowed
 * (TZ_NS, bit 9) and every domain whitelisted (bits 31:16).
 *
 * ⚠ DELIBERATE DEVIATION FROM THE RM, ALLOWLISTED WITH THIS REASON.
 * The manual's cold-POR value is FFFF_0000h -- TZ_NS CLEAR.  On a real board
 * TF-A sets it before Linux runs.  We boot -kernel and skip TF-A, and
 * clk-composite-93.c does:
 *
 *     if (!(authen & TZ_NS_MASK) || !(authen & BIT(WHITE_LIST_SHIFT + domain)))
 *             clk_ro = true;
 *
 * so honouring the RM here would mark EVERY CLOCK IN THE GUEST READ-ONLY.  The
 * RM's reset column is the cold-POR value, not what firmware sees on a board
 * with a boot ROM.  A deviation with a reason is a decision; a deviation
 * without one is a bug you have agreed not to look at.
 */
#define CCM_TZ_NS           (1u << 9)
#define CCM_WHITELIST_ALL   (0xffffu << 16)
#define CCM_AUTHEN_DEFAULT  (CCM_TZ_NS | CCM_WHITELIST_ALL)

typedef enum {
    CCM_BLK_NONE,
    CCM_BLK_ROOT,   /* CLOCK_ROOT: CONTROL + aliases, STATUS0, AUTHEN */
    CCM_BLK_GATE,   /* LPCG / OSCPLL: DIRECT, LPM, STATUS0/1, AUTHEN */
    CCM_BLK_GPR,    /* GPR_SHARED / GPR_PRIVATE: value + aliases, AUTHEN */
} CCMBlkKind;

static const struct {
    hwaddr start, end;
    uint32_t stride;
    CCMBlkKind kind;
} ccm_blocks[] = {
    { 0x0000, 0x2f80, 0x80, CCM_BLK_ROOT },   /* 95 clock roots */
    { 0x4800, 0x4a00, 0x40, CCM_BLK_GPR  },   /*  8 GPR_SHARED  */
    { 0x4c00, 0x4e00, 0x40, CCM_BLK_GPR  },   /*  8 GPR_PRIVATE */
    { 0x5000, 0x5500, 0x40, CCM_BLK_GATE },   /* 20 OSCPLL      */
    { 0x8000, 0x9fc0, 0x40, CCM_BLK_GATE },   /* 127 LPCG       */
};

/* Which block does @offset land in, and where inside it? */
static CCMBlkKind ccm_classify(hwaddr offset, hwaddr *blk, hwaddr *reg)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(ccm_blocks); i++) {
        if (offset >= ccm_blocks[i].start && offset < ccm_blocks[i].end) {
            uint32_t stride = ccm_blocks[i].stride;
            *reg = offset & (stride - 1);
            *blk = offset - *reg;
            return ccm_blocks[i].kind;
        }
    }
    return CCM_BLK_NONE;
}

/* SET/CLR/TOG alias applied to the register at @base. */
static void ccm_alias_write(IMX93CCMState *s, hwaddr base, hwaddr reg,
                            uint32_t value)
{
    uint32_t *p = &s->regs[base / 4];

    switch (reg & 0xc) {
    case 0x0: *p  = value; break;
    case 0x4: *p |= value; break;
    case 0x8: *p &= ~value; break;
    case 0xc: *p ^= value; break;
    }
}

static uint64_t imx93_ccm_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93CCMState *s = opaque;
    hwaddr blk, reg;

    switch (ccm_classify(offset, &blk, &reg)) {
    case CCM_BLK_ROOT:
        if (reg <= 0x0c) {
            /* CONTROL and its SET/CLR/TOG aliases all read the register. */
            return s->regs[(blk + CCM_ROOT_CONTROL) / 4];
        }
        if (reg == CCM_ROOT_STATUS0) {
            /* Clock change always complete: CHANGING and SLICE_BUSY clear. */
            return 0;
        }
        break;

    case CCM_BLK_GATE:
        if (reg == CCM_GATE_STATUS0) {
            /* Status follows the gate: bit 0 = the clock is running. */
            return s->regs[(blk + CCM_GATE_DIRECT) / 4] & CCM_GATE_ON;
        }
        if (reg == CCM_GATE_STATUS1) {
            return CCM_GATE_STATUS1_RS;
        }
        break;

    case CCM_BLK_GPR:
        if (reg <= 0x0c) {
            return s->regs[blk / 4];
        }
        break;

    case CCM_BLK_NONE:
        break;
    }

    if (reg >= CCM_AUTHEN_OFFSET && reg <= CCM_AUTHEN_OFFSET + 0x0c) {
        return CCM_AUTHEN_DEFAULT;
    }
    return s->regs[offset / 4];
}

static void imx93_ccm_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93CCMState *s = opaque;
    hwaddr blk, reg;

    /* AUTHEN and its aliases are a constant in this model (see above). */
    if (ccm_classify(offset, &blk, &reg) != CCM_BLK_NONE &&
        reg >= CCM_AUTHEN_OFFSET && reg <= CCM_AUTHEN_OFFSET + 0x0c) {
        return;
    }

    switch (ccm_classify(offset, &blk, &reg)) {
    case CCM_BLK_ROOT:
        if (reg <= 0x0c) {
            ccm_alias_write(s, blk + CCM_ROOT_CONTROL, reg, value);
            return;
        }
        if (reg == CCM_ROOT_STATUS0) {
            return;                                  /* read-only */
        }
        break;

    case CCM_BLK_GATE:
        if (reg == CCM_GATE_STATUS0 || reg == CCM_GATE_STATUS1 ||
            reg == CCM_GATE_LPM_CUR) {
            return;                                  /* read-only */
        }
        break;

    case CCM_BLK_GPR:
        if (reg <= 0x0c) {
            ccm_alias_write(s, blk, reg, value);
            return;
        }
        break;

    case CCM_BLK_NONE:
        break;
    }

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps imx93_ccm_ops = {
    .read = imx93_ccm_read,
    .write = imx93_ccm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void imx93_ccm_reset(DeviceState *dev)
{
    IMX93CCMState *s = IMX93_CCM(dev);
    int i;

    /*
     * A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM -- and
     * for the 147 LPCG/OSCPLL gates it was the wrong one: the RM resets every
     * DIRECT to 1 (clock RUNNING out of reset), and this model claimed they
     * were all off.  Asserted by tests/imx91-reset-values.
     */
    memset(s->regs, 0, sizeof(s->regs));

    for (i = 0; i < ARRAY_SIZE(ccm_blocks); i++) {
        hwaddr off;

        if (ccm_blocks[i].kind != CCM_BLK_GATE) {
            continue;
        }
        for (off = ccm_blocks[i].start; off < ccm_blocks[i].end;
             off += ccm_blocks[i].stride) {
            s->regs[(off + CCM_GATE_DIRECT) / 4] = CCM_GATE_ON;
        }
    }
}

static void imx93_ccm_init(Object *obj)
{
    IMX93CCMState *s = IMX93_CCM(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_ccm_ops, s,
                          TYPE_IMX93_CCM, IMX93_CCM_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_imx93_ccm = {
    .name = TYPE_IMX93_CCM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93CCMState, IMX93_CCM_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_ccm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 Clock Control Module";
    device_class_set_legacy_reset(dc, imx93_ccm_reset);
    dc->vmsd = &vmstate_imx93_ccm;
}

static const TypeInfo imx93_ccm_types[] = {
    {
        .name           = TYPE_IMX93_CCM,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93CCMState),
        .instance_init  = imx93_ccm_init,
        .class_init     = imx93_ccm_class_init,
    },
};

DEFINE_TYPES(imx93_ccm_types)
