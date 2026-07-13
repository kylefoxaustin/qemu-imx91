#ifndef CHIPIDEA_H
#define CHIPIDEA_H

#include "hw/usb/hcd-ehci.h"
#include "qom/object.h"

struct ChipideaState {
    /*< private >*/
    EHCISysBusState parent_obj;

    MemoryRegion iomem[3];

    /* Identification block (0x00..0x14, 0x90).  See chipidea.c. */
    uint32_t id;
    uint32_t hwgeneral;
    uint32_t hwhost;
    uint32_t hwtxbuf;
    uint32_t hwrxbuf;
    uint32_t sbuscfg;
};

#define TYPE_CHIPIDEA "usb-chipidea"
OBJECT_DECLARE_SIMPLE_TYPE(ChipideaState, CHIPIDEA)

#endif /* CHIPIDEA_H */
