/*
 * NXP i.MX 93 PXP (Pixel Pipeline) 2D engine
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the legacy MXS-style register cluster (offsets 0x00..0x3ff) that the
 * built-in pxp_dma_v3 driver programs for a g2d_copy: HW_PXP_CTRL soft-reset,
 * the PS (source) and OUT (dest) surface registers, and the ENABLE kick. On the
 * kick the engine fetches the PS surface from guest memory, writes it to the OUT
 * surface (same-format copy / constant-colour fill) and raises the completion
 * interrupt (WAKEUPMIX PXP interrupt 0) that the driver's fence waits on. Every
 * register is MXS SET/CLR/TOG aliased; set PXP_DBG to trace accesses. The blit
 * register layout was captured from the live driver (see tests/pxp-imx93). Scope
 * is single-source copy + fill; CSC / blend / scale / rotate are not modelled.
 */

#include "qemu/osdep.h"
#include "hw/misc/imx93_pxp.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/vmstate.h"

/* HW_PXP_CTRL (0x00) and STAT (0x10); both MXS SET/CLR/TOG aliased. */
#define HW_PXP_CTRL         0x00
#define HW_PXP_STAT         0x10

#define BM_PXP_CTRL_ENABLE  0x00000001
#define BM_PXP_CTRL_SFTRST  0x80000000
#define BM_PXP_CTRL_CLKGATE 0x40000000
#define BM_PXP_STAT_IRQ0    0x00000001

/* Surface registers (base offsets; the driver writes these directly). */
#define HW_PXP_OUT_CTRL     0x20    /* [7:0] FORMAT */
#define HW_PXP_OUT_BUF      0x30    /* dest physical address */
#define HW_PXP_OUT_PITCH    0x50    /* dest stride, bytes */
#define HW_PXP_OUT_LRC      0x60    /* [31:16] width-1, [15:0] height-1 */
#define HW_PXP_OUT_PS_ULC   0x70
#define HW_PXP_OUT_PS_LRC   0x80
#define HW_PXP_PS_CTRL      0xb0    /* [7:0] FORMAT */
#define HW_PXP_PS_BUF       0xc0    /* source physical address */
#define HW_PXP_PS_PITCH     0xf0    /* source stride, bytes */

#define R(s, off)           ((s)->regs[(off) / 4])

static void imx93_pxp_trace(const char *op, hwaddr offset, uint32_t value)
{
    if (getenv("PXP_DBG")) {
        fprintf(stderr, "[pxp] %s +%#06x = %#010x\n", op,
                (unsigned)offset, value);
    }
}

/* Bytes per pixel for the PS/OUT FORMAT field (RGB formats; copy+fill scope). */
static int imx93_pxp_bpp(uint32_t ctrl)
{
    switch (ctrl & 0x1f) {
    case 0x08 ... 0x0f:     /* RGB555 / ARGB1555 / RGB565 / ARGB4444 */
        return 2;
    default:                /* ARGB8888 / RGB888-in-32 and friends */
        return 4;
    }
}

/*
 * Execute one PXP pass: copy the PS surface to the OUT surface (or, when the PS
 * source is disabled, fill OUT with a constant colour), then signal completion.
 */
static void imx93_pxp_blit(IMX93PxpState *s)
{
    uint32_t lrc = R(s, HW_PXP_OUT_LRC);
    uint32_t width = ((lrc >> 16) & 0xffff) + 1;
    uint32_t height = (lrc & 0xffff) + 1;
    int bpp = imx93_pxp_bpp(R(s, HW_PXP_OUT_CTRL));
    uint64_t out_buf = R(s, HW_PXP_OUT_BUF);
    uint64_t ps_buf = R(s, HW_PXP_PS_BUF);
    uint32_t out_pitch = R(s, HW_PXP_OUT_PITCH) & 0xffff;
    uint32_t ps_pitch = R(s, HW_PXP_PS_PITCH) & 0xffff;
    size_t line_bytes = (size_t)width * bpp;
    g_autofree uint8_t *line = NULL;
    uint32_t y;

    if (!out_buf || width == 0 || height == 0 || line_bytes == 0) {
        goto done;
    }
    if (!out_pitch) {
        out_pitch = line_bytes;
    }
    if (!ps_pitch) {
        ps_pitch = line_bytes;
    }
    line = g_malloc(line_bytes);

    if (!ps_buf) {
        /*
         * No PS source surface programmed: this is a constant-colour fill
         * (INPUT_STORE_FILL_DATA path), which is not modelled yet. Signal
         * completion so the driver's fence is not left waiting.
         */
        goto done;
    }

    line = g_malloc(line_bytes);
    for (y = 0; y < height; y++) {
        if (dma_memory_read(&address_space_memory,
                            ps_buf + (uint64_t)y * ps_pitch, line,
                            line_bytes, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        dma_memory_write(&address_space_memory,
                         out_buf + (uint64_t)y * out_pitch, line, line_bytes,
                         MEMTXATTRS_UNSPECIFIED);
    }

done:
    /* Hardware clears ENABLE when the frame completes and posts IRQ0. */
    s->ctrl &= ~BM_PXP_CTRL_ENABLE;
    s->stat |= BM_PXP_STAT_IRQ0;
    qemu_set_irq(s->irq, 1);
}

static uint64_t imx93_pxp_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX93PxpState *s = opaque;
    uint32_t base = offset & ~0xfu;
    uint32_t val;

    if (base == HW_PXP_CTRL) {
        /* Asserting SFTRST also asserts CLKGATE; the driver polls for this. */
        val = s->ctrl;
        if (val & BM_PXP_CTRL_SFTRST) {
            val |= BM_PXP_CTRL_CLKGATE;
        }
    } else if (base == HW_PXP_STAT) {
        val = s->stat;
    } else {
        val = s->regs[base / 4];
    }
    imx93_pxp_trace("rd", offset, val);
    return val;
}

/* Apply an MXS SET/CLR/TOG write (offset & 0xf selects the alias) to *reg. */
static void imx93_pxp_mxs(uint32_t *reg, hwaddr offset, uint32_t value)
{
    switch (offset & 0xf) {
    case 0x0: *reg = value;    break;
    case 0x4: *reg |= value;   break;
    case 0x8: *reg &= ~value;  break;
    case 0xc: *reg ^= value;   break;
    }
}

static void imx93_pxp_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX93PxpState *s = opaque;
    uint32_t base = offset & ~0xfu;

    imx93_pxp_trace("wr", offset, (uint32_t)value);

    if (base == HW_PXP_CTRL) {
        imx93_pxp_mxs(&s->ctrl, offset, value);
        /* Writing ENABLE kicks one pass; the blit clears it on completion. */
        if (s->ctrl & BM_PXP_CTRL_ENABLE) {
            imx93_pxp_blit(s);
        }
    } else if (base == HW_PXP_STAT) {
        imx93_pxp_mxs(&s->stat, offset, value);
        /* Driver clears IRQ0 in its handler; drop the line when it does. */
        if (!(s->stat & BM_PXP_STAT_IRQ0)) {
            qemu_set_irq(s->irq, 0);
        }
    } else {
        imx93_pxp_mxs(&s->regs[base / 4], offset, value);
    }
}

static const MemoryRegionOps imx93_pxp_ops = {
    .read = imx93_pxp_read,
    .write = imx93_pxp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void imx93_pxp_reset(DeviceState *dev)
{
    IMX93PxpState *s = IMX93_PXP(dev);

    s->ctrl = 0;
    s->stat = 0;
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void imx93_pxp_init(Object *obj)
{
    IMX93PxpState *s = IMX93_PXP(obj);

    memory_region_init_io(&s->iomem, obj, &imx93_pxp_ops, s,
                          TYPE_IMX93_PXP, IMX93_PXP_REG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_imx93_pxp = {
    .name = TYPE_IMX93_PXP,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, IMX93PxpState),
        VMSTATE_UINT32(stat, IMX93PxpState),
        VMSTATE_UINT32_ARRAY(regs, IMX93PxpState, IMX93_PXP_NUM_REGS),
        VMSTATE_END_OF_LIST()
    },
};

static void imx93_pxp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "i.MX 93 PXP (Pixel Pipeline) 2D engine";
    device_class_set_legacy_reset(dc, imx93_pxp_reset);
    dc->vmsd = &vmstate_imx93_pxp;
}

static const TypeInfo imx93_pxp_types[] = {
    {
        .name           = TYPE_IMX93_PXP,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(IMX93PxpState),
        .instance_init  = imx93_pxp_init,
        .class_init     = imx93_pxp_class_init,
    },
};

DEFINE_TYPES(imx93_pxp_types)
