/*
 * Arm Ethos-U55/U65 microNPU - operation execution (DMA + compute)
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Executes the operations the command-stream decoder resolves, over the device
 * DMA address space. Phase 2 implements NPU_OP_DMA_START (a region-to-region
 * copy); the conv/depthwise/pool/elementwise compute kernels arrive in a later
 * phase.
 */

#include "qemu/osdep.h"
#include "hw/npu/ethos_u.h"
#include "ethos_u_internal.h"
#include "ethos_u_addr.h"
#include "system/dma.h"

#define ETHOS_U_DMA_MAX (16 * 1024 * 1024)

static void ethos_u_dma_copy(EthosUState *s, uint64_t dst, uint64_t src,
                             uint32_t len)
{
    g_autofree uint8_t *buf = NULL;

    if (len == 0 || len > ETHOS_U_DMA_MAX) {
        return;
    }
    buf = g_malloc(len);
    if (dma_memory_read(&s->dma_as, src, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    dma_memory_write(&s->dma_as, dst, buf, len, MEMTXATTRS_UNSPECIFIED);
}

void ethos_u_exec_op(void *ctx, uint16_t opcode, const EthosUOpDesc *op)
{
    EthosUState *s = ctx;

    switch (opcode) {
    case NPU_OP_DMA_START:
        ethos_u_dma_copy(s, op->dma_dst, op->dma_src, op->dma_len);
        break;
    case NPU_OP_CONV:
    case NPU_OP_DEPTHWISE:
    case NPU_OP_POOL:
    case NPU_OP_ELEMENTWISE:
        /* Compute kernels land in a later phase. */
        break;
    default:
        break;
    }
}
