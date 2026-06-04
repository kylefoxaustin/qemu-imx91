/*
 * NXP i.MX 93 ELE (EdgeLock Enclave / sentinel) Messaging Unit responder
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the s4muap MU (compatible "fsl,imx93-mu-s4") AND a minimal ELE
 * firmware responder behind it. The real ELE is a separate security
 * subsystem we do not emulate; instead, when Linux's fsl-se driver sends an
 * ELE command over the MU, this model synthesizes a generic SUCCESS response
 * so that se_if_probe() completes (se_soc_info / GET_INFO etc. no longer time
 * out). That in turn lets the OCOTP driver get its se-fw handle and register
 * the MAC nvmem cells, unblocking the FEC/eQOS ethernet probes.
 *
 * Only the transport + a success responder are modeled - no real enclave
 * services (RNG, crypto, fuse programming) are provided.
 */

#ifndef IMX93_ELE_H
#define IMX93_ELE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/units.h"

#define TYPE_IMX93_ELE "imx93.ele"
OBJECT_DECLARE_SIMPLE_TYPE(IMX93EleState, IMX93_ELE)

#define IMX93_ELE_REG_SIZE      (64 * KiB)
#define IMX93_ELE_NUM_TR        4
#define IMX93_ELE_NUM_RR        4
#define IMX93_ELE_MSG_MAX       64

struct IMX93EleState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq     irq_tx;    /* "tx" interrupt (SPI 31) */
    qemu_irq     irq_rx;    /* "rx" interrupt (SPI 30) */

    /* MU control/status shadow. */
    uint32_t gier, gcr, tcr, rcr;

    /* TX message accumulation (words arrive sequentially via TR regs). */
    uint32_t txbuf[IMX93_ELE_MSG_MAX];
    uint32_t txn;
    uint32_t msg_size;

    /* RX response registers + receive-full status bits. */
    uint32_t rr[IMX93_ELE_NUM_RR];
    uint32_t rsr;
};

#endif /* IMX93_ELE_H */
