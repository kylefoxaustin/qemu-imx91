/*
 * Copyright (c) 2018, Impinj, Inc.
 *
 * Chipidea USB block emulation code
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/usb/chipidea.h"
#include "qemu/module.h"

enum {
    CHIPIDEA_USBx_DCIVERSION   = 0x000,
    CHIPIDEA_USBx_DCCPARAMS    = 0x004,
    CHIPIDEA_USBx_DCCPARAMS_HC = BIT(8),
};

/*
 * The ChipIdea identification block (offsets 0x00..0x14, plus SBUSCFG at 0x90).
 *
 * ⭐ THESE ALL READ AS ZERO, AND THE GUEST BRANCHES ON THEM.
 *
 * ci_hdrc's ci_get_revision() does:
 *
 *     ver = hw_read_id_reg(ci, ID_ID, VERSION) >> __ffs(VERSION);
 *     if (ver == 0x2)  rev = REVISION + CI_REVISION_20;
 *     else if (ver == 0x0)  rev = CI_REVISION_1X;      <-- WE LAND HERE
 *
 * The i.MX 9 ID register is E4A1FA05h: VERSION = 2, REVISION = 5.  Returning zero
 * does not mean "no answer" -- ZERO IS A VALID VERSION, and it tells the driver it
 * is talking to a ChipIdea 1.x controller when the silicon is a 2.5.
 *
 *     A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM.
 *
 * (Latent today: ci->rev only gates quirks in udc.c, the device/gadget path, which
 * we do not emulate.  It is still the wrong answer to "what chip are you", and it is
 * the answer a driver would act on the moment gadget mode arrived.)
 *
 * Values are PROPERTIES, defaulting to the historical zero, so the i.MX6/i.MX7
 * machines that share this model are byte-for-byte unchanged; the i.MX 9 SoC sets
 * them from its reference manual.
 *
 * ⚠ HWDEVICE IS DELIBERATELY *NOT* SET, AND THAT IS THE INTERESTING ONE.
 * Silicon reports DC=1 with 8 endpoints.  But chipidea_dc_read() below deliberately
 * reports HOST-ONLY in DCCPARAMS, because we do not emulate device mode -- so
 * advertising 8 endpoints in HWDEVICE would leave the two registers CONTRADICTING
 * EACH OTHER about the same fact, which is the exact tell of a fabricated
 * capability.  We under-report, consistently, in both.  It is allowlisted with that
 * reason in tests/imx91-reset-values.
 */
enum {
    CHIPIDEA_USBx_ID        = 0x00,
    CHIPIDEA_USBx_HWGENERAL = 0x04,
    CHIPIDEA_USBx_HWHOST    = 0x08,
    CHIPIDEA_USBx_HWDEVICE  = 0x0c,
    CHIPIDEA_USBx_HWTXBUF   = 0x10,
    CHIPIDEA_USBx_HWRXBUF   = 0x14,
    CHIPIDEA_USBx_SBUSCFG   = 0x90,
};

static uint64_t chipidea_read(void *opaque, hwaddr offset,
                               unsigned size)
{
    ChipideaState *ci = CHIPIDEA(opaque);

    switch (offset) {
    case CHIPIDEA_USBx_ID:        return ci->id;
    case CHIPIDEA_USBx_HWGENERAL: return ci->hwgeneral;
    case CHIPIDEA_USBx_HWHOST:    return ci->hwhost;
    case CHIPIDEA_USBx_HWTXBUF:   return ci->hwtxbuf;
    case CHIPIDEA_USBx_HWRXBUF:   return ci->hwrxbuf;
    case CHIPIDEA_USBx_SBUSCFG:   return ci->sbuscfg;
    }
    return 0;
}

static void chipidea_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
}

/*
 * ⚠ THE ENDPOINT WINDOW SHARES chipidea_ops WITH THE ID BLOCK, AND OFFSETS ARE
 *   REGION-RELATIVE.
 *
 * chipidea_init() maps this same ops struct over TWO regions: ".misc" at 0x000 and
 * ".endpoints" at 0x1A4.  Adding an offset switch to chipidea_read() therefore
 * served the ID registers out of the ENDPOINT registers too -- USBMODE answered
 * HWGENERAL, ENDPTSETUPSTAT answered HWHOST, ENDPTFLUSH answered HWTXBUF.
 *
 * The USB tests did not care (host mode never reads those), and the reset-value gate
 * did: it is the only check here that is not written against this model.
 *
 *     THE ORACLE I DID NOT AUTHOR CAUGHT THE BUG I INTRODUCED WHILE FIXING THE BUG
 *     IT FOUND.                                                    -- mcxn947qemu
 *
 * So the endpoint window keeps its own (empty) ops, and the ID switch cannot leak
 * into it -- by construction, not by remembering.
 */
static uint64_t chipidea_ep_read(void *opaque, hwaddr offset, unsigned size)
{
    return 0;
}

static const struct MemoryRegionOps chipidea_ep_ops = {
    .read = chipidea_ep_read,
    .write = chipidea_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const struct MemoryRegionOps chipidea_ops = {
    .read = chipidea_read,
    .write = chipidea_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        /*
         * Our device would not work correctly if the guest was doing
         * unaligned access. This might not be a limitation on the
         * real device but in practice there is no reason for a guest
         * to access this device unaligned.
         */
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static uint64_t chipidea_dc_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    switch (offset) {
    case CHIPIDEA_USBx_DCIVERSION:
        return 0x1;
    case CHIPIDEA_USBx_DCCPARAMS:
        /*
         * Real hardware (at least i.MX7) will also report the
         * controller as "Device Capable" (and 8 supported endpoints),
         * but there doesn't seem to be much point in doing so, since
         * we don't emulate that part.
         */
        return CHIPIDEA_USBx_DCCPARAMS_HC;
    }

    return 0;
}

static void chipidea_dc_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
}

static const struct MemoryRegionOps chipidea_dc_ops = {
    .read = chipidea_dc_read,
    .write = chipidea_dc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        /*
         * Our device would not work correctly if the guest was doing
         * unaligned access. This might not be a limitation on the real
         * device but in practice there is no reason for a guest to access
         * this device unaligned.
         */
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void chipidea_init(Object *obj)
{
    EHCIState *ehci = &SYS_BUS_EHCI(obj)->ehci;
    ChipideaState *ci = CHIPIDEA(obj);
    int i;

    /*
     * ChipIdea reports the negotiated device speed in PORTSC.PSPD [27:26];
     * the i.MX ci_hdrc driver reads it to set the device speed, so without
     * this a high-speed device would enumerate as full-speed.
     */
    ehci->report_pspd = true;

    for (i = 0; i < ARRAY_SIZE(ci->iomem); i++) {
        const struct {
            const char *name;
            hwaddr offset;
            uint64_t size;
            const struct MemoryRegionOps *ops;
        } regions[ARRAY_SIZE(ci->iomem)] = {
            /*
             * Registers located between offsets 0x000 and 0xFC
             */
            {
                .name   = TYPE_CHIPIDEA ".misc",
                .offset = 0x000,
                .size   = 0x100,
                .ops    = &chipidea_ops,
            },
            /*
             * Registers located between offsets 0x1A4 and 0x1DC
             */
            {
                .name   = TYPE_CHIPIDEA ".endpoints",
                .offset = 0x1A4,
                .size   = 0x1DC - 0x1A4 + 4,
                .ops    = &chipidea_ep_ops,
            },
            /*
             * USB_x_DCIVERSION and USB_x_DCCPARAMS
             */
            {
                .name   = TYPE_CHIPIDEA ".dc",
                .offset = 0x120,
                .size   = 8,
                .ops    = &chipidea_dc_ops,
            },
        };

        memory_region_init_io(&ci->iomem[i],
                              obj,
                              regions[i].ops,
                              ci,
                              regions[i].name,
                              regions[i].size);

        memory_region_add_subregion(&ehci->mem,
                                    regions[i].offset,
                                    &ci->iomem[i]);
    }
}

static const Property chipidea_properties[] = {
    /* Default 0 = the historical behaviour, so i.MX6/i.MX7 are unchanged. */
    DEFINE_PROP_UINT32("id",        ChipideaState, id,        0),
    DEFINE_PROP_UINT32("hwgeneral", ChipideaState, hwgeneral, 0),
    DEFINE_PROP_UINT32("hwhost",    ChipideaState, hwhost,    0),
    DEFINE_PROP_UINT32("hwtxbuf",   ChipideaState, hwtxbuf,   0),
    DEFINE_PROP_UINT32("hwrxbuf",   ChipideaState, hwrxbuf,   0),
    DEFINE_PROP_UINT32("sbuscfg",   ChipideaState, sbuscfg,   0),
};

static void chipidea_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(klass);

    device_class_set_props(dc, chipidea_properties);

    /*
     * Offsets used were taken from i.MX7Dual Applications Processor
     * Reference Manual, Rev 0.1, p. 3177, Table 11-59
     */
    sec->capsbase   = 0x100;
    sec->opregbase  = 0x140;
    sec->portnr     = 1;

    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    dc->desc = "Chipidea USB Module";
}

static const TypeInfo chipidea_info = {
    .name          = TYPE_CHIPIDEA,
    .parent        = TYPE_SYS_BUS_EHCI,
    .instance_size = sizeof(ChipideaState),
    .instance_init = chipidea_init,
    .class_init    = chipidea_class_init,
};

static void chipidea_register_type(void)
{
    type_register_static(&chipidea_info);
}
type_init(chipidea_register_type)
