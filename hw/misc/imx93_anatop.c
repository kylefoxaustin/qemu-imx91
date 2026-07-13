/*
 * NXP i.MX 93 ANATOP (Analog Top / PLL) module
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Fractional-N GPPLL blocks, 0x100 apart, taken from IMX91RM.pdf rev 5 and
 * asserted by tests/imx91-reset-values:
 *
 *     ARM_PLL @ 0x1000   SYS_PLL @ 0x1100   AUDIO_PLL @ 0x1200
 *     DRAM_PLL @ 0x1300  VIDEO_PLL @ 0x1400
 *
 *     +0x00 CTRL        : POWERUP[0], CLKMUX_EN[1], CLKMUX_BYPASS[2]
 *     +0x40 NUMERATOR   : MFN[31:2]
 *     +0x50 DENOMINATOR : MFD[29:0]      RESET = 1  (it is a DENOMINATOR)
 *     +0x60 DIV         : MFI[24:16], RDIV[15:13], ODIV[7:0]   RESET = 00C8_0000h
 *     +0xF0 PLL_STATUS  : LOCK[0]
 *
 * ⭐ THIS BLOCK USED TO PRODUCE NO FREQUENCIES AT ALL, AND ITS REGISTERS RESET TO
 *    ZERO -- INCLUDING THE DENOMINATOR.
 *
 * That is not a neutral default. Firmware asking the hardware "what speed am I
 * running at" calls CLOCK_GetPllFreq() / clk_fracn_gppll_recalc_rate(), which
 * COMPUTES the answer from exactly these fields -- so a zeroed MFI and a zeroed
 * MFD hand the guest an answer derived from a divider of zero, and it believes
 * it. rt1180emulator found precisely this in his own ANADIG, and it was the best
 * thing the reset-value oracle did in anyone's tree.
 *
 *     A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM.
 *
 * So the PLL rates are now COMPUTED, with the driver's own arithmetic, from the
 * registers the guest actually wrote -- and a PLL that is not powered up produces
 * NOTHING rather than a plausible number. A stopped clock gets diagnosed in a
 * minute; a plausible clock ships into somebody's product.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_anatop.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"
#include "qemu/log.h"

/* Fractional-N PLL blocks live at and above this offset, 0x100 apart. */
#define ANATOP_PLL_BASE     0x1000
#define ANATOP_PLL_STRIDE   0x100

/* Per-PLL register offsets. */
#define PLL_CTRL_OFFSET     0x00
#define PLL_SPREAD_OFFSET   0x30
#define PLL_NUM_OFFSET      0x40
#define PLL_DENOM_OFFSET    0x50
#define PLL_DIV_OFFSET      0x60
#define PLL_STATUS_OFFSET   0xF0

#define PLL_CTRL_POWERUP    (1u << 0)
#define PLL_LOCK_STATUS     (1u << 0)

/* Reset values, from the RM. */
#define PLL_DIV_RESET       0x00c80000    /* MFI = 200 */
#define PLL_DENOM_RESET     0x00000001    /* it is a DENOMINATOR: never zero */

/* Every fracn-gppll block on the chip, whether or not we synthesise its rate. */
static const hwaddr imx93_pll_block[] = {
    0x1000,  /* ARM_PLL   */
    0x1100,  /* SYS_PLL   -- rate not synthesised (its PFDs are fixed constants) */
    0x1200,  /* AUDIO_PLL */
    0x1300,  /* DRAM_PLL  */
    0x1400,  /* VIDEO_PLL */
};

/* Block offset of each PLL we synthesise. */
static const hwaddr imx93_pll_base[IMX93_PLL__COUNT] = {
    [IMX93_PLL_ARM]   = 0x1000,
    [IMX93_PLL_AUDIO] = 0x1200,
    [IMX93_PLL_DRAM]  = 0x1300,
    [IMX93_PLL_VIDEO] = 0x1400,
};

static const char *imx93_pll_name[IMX93_PLL__COUNT] = {
    [IMX93_PLL_ARM]   = "arm_pll",
    [IMX93_PLL_AUDIO] = "audio_pll",
    [IMX93_PLL_VIDEO] = "video_pll",
    [IMX93_PLL_DRAM]  = "dram_pll",
};

static bool anatop_is_pll_status(hwaddr offset)
{
    return offset >= ANATOP_PLL_BASE &&
           (offset & (ANATOP_PLL_STRIDE - 1)) == PLL_STATUS_OFFSET;
}

/*
 * ⭐ EVERY PLL REGISTER HAS SET / CLR / TOG ALIASES, AND WE WERE SWALLOWING THEM.
 *
 * Within a PLL block the RM puts a base register at +0x00 (CTRL), +0x30 (SPREAD),
 * +0x40 (NUMERATOR), +0x50 (DENOMINATOR) and +0x60 (DIV), each with SET/CLR/TOG at
 * +4/+8/+C.  This model had no alias handling at all: a write to DIV_SET landed in
 * the alias's OWN backing store and THE DIVIDER NEVER MOVED.  Measured:
 *
 *     writel DIV_SET 0x00640000   ->  DIV      reads 0x00c80000   (UNCHANGED)
 *                                     DIV_SET  reads 0x00640000   (a DEAD register)
 *
 * This is the CCM CONTROL_SET bug, verbatim, one block over -- and it was INERT
 * until the ANATOP started COMPUTING the PLL rate from DIV this morning.  The
 * moment the register meant something, the dropped write did too: a guest
 * programming its PLL through the SET alias now gets NO FREQUENCY CHANGE, silently.
 *
 *     FIXING ONE REGISTER MAKES ITS ALIASES LOAD-BEARING.  A dead write is only
 *     harmless while nothing reads what it should have written.
 */
static bool anatop_is_alias(hwaddr offset, hwaddr *base)
{
    hwaddr reg;

    if (offset < ANATOP_PLL_BASE) {
        return false;
    }
    reg = offset & (ANATOP_PLL_STRIDE - 1);

    switch (reg & ~0xfull) {
    case PLL_CTRL_OFFSET:
    case PLL_SPREAD_OFFSET:
    case PLL_NUM_OFFSET:
    case PLL_DENOM_OFFSET:
    case PLL_DIV_OFFSET:
        *base = offset & ~0xfull;
        return true;
    default:
        return false;
    }
}

/*
 * clk_fracn_gppll_recalc_rate(), verbatim:
 *
 *     Fvco = (Fref / rdiv) * (MFI + MFN / MFD)
 *     rdiv == 0 is treated as 1;  odiv 0 -> 2, odiv 1 -> 3.
 *
 * Returns 0 -- meaning NO CLOCK, not "some default" -- when the PLL is not
 * powered up, when there is no reference, or when the guest has programmed a
 * denominator of zero. Every one of those is a state in which real silicon
 * produces no usable output, and a model that invents one here is the reason
 * firmware ships believing a frequency it never had.
 */
static uint64_t imx93_anatop_pll_hz(IMX93AnatopState *s, IMX93AnatopPll pll)
{
    hwaddr base = imx93_pll_base[pll];
    uint32_t ctrl  = s->regs[(base + PLL_CTRL_OFFSET) / 4];
    uint32_t num   = s->regs[(base + PLL_NUM_OFFSET) / 4];
    uint32_t denom = s->regs[(base + PLL_DENOM_OFFSET) / 4];
    uint32_t div   = s->regs[(base + PLL_DIV_OFFSET) / 4];
    uint64_t fref  = clock_get_hz(s->osc_in);
    uint64_t mfn, mfd, mfi, rdiv, odiv, fvco;

    if (!(ctrl & PLL_CTRL_POWERUP) || fref == 0) {
        return 0;
    }

    mfn  = (num >> 2) & 0x3fffffff;
    mfd  = denom & 0x3fffffff;
    mfi  = (div >> 16) & 0x1ff;
    rdiv = (div >> 13) & 0x7;
    odiv = div & 0xff;

    rdiv = rdiv ? rdiv : 1;
    switch (odiv) {
    case 0:  odiv = 2; break;
    case 1:  odiv = 3; break;
    default: break;
    }

    if (mfd == 0) {
        /* The guest wrote a zero denominator.  Refuse to invent a frequency. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "imx93.anatop: %s has MFD=0; no output\n",
                      imx93_pll_name[pll]);
        return 0;
    }

    fvco = fref * mfi * mfd + fref * mfn;
    return fvco / (mfd * rdiv * odiv);
}

static void imx93_anatop_update(IMX93AnatopState *s)
{
    int i;

    for (i = 0; i < IMX93_PLL__COUNT; i++) {
        clock_update_hz(s->pll_out[i], imx93_anatop_pll_hz(s, i));
    }
}

static uint64_t imx93_anatop_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93AnatopState *s = opaque;

    hwaddr base;

    if (anatop_is_pll_status(offset)) {
        /*
         * LOCK follows POWERUP.  It used to be pinned high unconditionally --
         * a fabricated ready-bit, the fourth of its kind found across this fleet.
         * clk_fracn_gppll_prepare() asserts POWERUP and only THEN polls LOCK, so
         * this is safe here; a guest that polls LOCK without powering the PLL up
         * now spins, which is what silicon does and what we want it to do.
         */
        uint32_t ctrl = s->regs[(offset & ~0xffull) / 4];
        return (ctrl & PLL_CTRL_POWERUP) ? PLL_LOCK_STATUS : 0;
    }
    if (anatop_is_alias(offset, &base)) {
        /* CTRL/SPREAD/NUM/DENOM/DIV and their SET/CLR/TOG all read the register. */
        return s->regs[base / 4];
    }
    return s->regs[offset / 4];
}

static void imx93_anatop_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    IMX93AnatopState *s = opaque;
    hwaddr base;

    /* PLL_STATUS is read-only. */
    if (anatop_is_pll_status(offset)) {
        return;
    }
    if (anatop_is_alias(offset, &base)) {
        uint32_t *p = &s->regs[base / 4];

        switch (offset & 0xc) {
        case 0x0: *p  = value; break;
        case 0x4: *p |= value; break;      /* SET */
        case 0x8: *p &= ~value; break;     /* CLR */
        case 0xc: *p ^= value; break;      /* TOG */
        }
        imx93_anatop_update(s);
        return;
    }
    s->regs[offset / 4] = value;
    imx93_anatop_update(s);
}

static const MemoryRegionOps imx93_anatop_ops = {
    .read = imx93_anatop_read,
    .write = imx93_anatop_write,
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

static void imx93_anatop_reset(DeviceState *dev)
{
    IMX93AnatopState *s = IMX93_ANATOP(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * The RM's reset values, and they are load-bearing, not cosmetic: MFI and MFD
     * are the inputs to the frequency the guest COMPUTES for itself.  A zeroed
     * denominator is a divide-by-zero in the guest's own arithmetic.
     *
     * Applied to ALL FIVE fracn blocks, including SYS_PLL -- whose rate we do not
     * synthesise (its PFDs are fixed) but whose registers a guest can still read.
     * A register we do not consume is still a register we can lie through.
     */
    for (i = 0; i < ARRAY_SIZE(imx93_pll_block); i++) {
        hwaddr base = imx93_pll_block[i];

        s->regs[(base + PLL_DIV_OFFSET) / 4]   = PLL_DIV_RESET;
        s->regs[(base + PLL_DENOM_OFFSET) / 4] = PLL_DENOM_RESET;
    }

    imx93_anatop_update(s);
}

static void imx93_anatop_osc_update(void *opaque, ClockEvent event)
{
    imx93_anatop_update(IMX93_ANATOP(opaque));
}

static void imx93_anatop_init(Object *obj)
{
    IMX93AnatopState *s = IMX93_ANATOP(obj);
    int i;

    memory_region_init_io(&s->iomem, obj, &imx93_anatop_ops, s,
                          TYPE_IMX93_ANATOP, IMX93_ANATOP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    s->osc_in = qdev_init_clock_in(DEVICE(obj), "osc_in",
                                   imx93_anatop_osc_update, s, ClockUpdate);
    for (i = 0; i < IMX93_PLL__COUNT; i++) {
        s->pll_out[i] = qdev_init_clock_out(DEVICE(obj), imx93_pll_name[i]);
    }
}

static const VMStateDescription vmstate_imx93_anatop = {
    .name = TYPE_IMX93_ANATOP,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX93AnatopState, IMX93_ANATOP_NUM_REGS),
        VMSTATE_CLOCK(osc_in, IMX93AnatopState),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_anatop_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 ANATOP (PLLs)";
    device_class_set_legacy_reset(dc, imx93_anatop_reset);
    dc->vmsd = &vmstate_imx93_anatop;
}

static const TypeInfo imx93_anatop_types[] = {
    {
        .name           = TYPE_IMX93_ANATOP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93AnatopState),
        .instance_init  = imx93_anatop_init,
        .class_init     = imx93_anatop_class_init,
    },
};

DEFINE_TYPES(imx93_anatop_types)
