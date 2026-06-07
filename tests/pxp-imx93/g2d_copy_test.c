/*
 * g2d_copy oracle for the i.MX93 PXP model.
 *
 * Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Drives a linear PXP copy through the real userspace stack:
 *   libg2d (imx-pxp-g2d) -> ioctl(/dev/pxp_device) -> pxp_dma_v3 -> PXP model.
 * Allocates two physically-contiguous buffers, fills the source with a known
 * pattern, g2d_copy()s it to the destination, waits for completion and verifies
 * the bytes match. Prints "PXP-G2D-COPY: PASS" / "FAIL" so a boot log can be
 * scored. Used both to capture the driver's blit register sequence (run it with
 * PXP_DBG set on the QEMU side) and as the end-to-end pass/fail oracle once the
 * blit datapath is modelled. g2d_basic_test is not in the BSP, hence this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "g2d.h"

#define COPY_BYTES (64 * 1024)

int main(void)
{
    void *handle = NULL;
    struct g2d_buf *src, *dst;
    unsigned char *sp, *dp;
    int i, rc, bad = 0;

    if (g2d_open(&handle) || !handle) {
        printf("PXP-G2D-COPY: FAIL (g2d_open)\n");
        return 1;
    }
    src = g2d_alloc(COPY_BYTES, 0);
    dst = g2d_alloc(COPY_BYTES, 0);
    if (!src || !dst) {
        printf("PXP-G2D-COPY: FAIL (g2d_alloc)\n");
        return 1;
    }

    sp = src->buf_vaddr;
    dp = dst->buf_vaddr;
    for (i = 0; i < COPY_BYTES; i++) {
        sp[i] = (unsigned char)(i * 7 + 0x11);   /* deterministic pattern */
    }
    memset(dp, 0xa5, COPY_BYTES);
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    rc = g2d_copy(handle, dst, src, COPY_BYTES);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-COPY: FAIL (g2d_copy/finish rc=%d)\n", rc);
    } else {
        for (i = 0; i < COPY_BYTES; i++) {
            if (dp[i] != sp[i]) {
                if (bad < 4) {
                    printf("  byte %d: got %#x want %#x\n", i, dp[i], sp[i]);
                }
                bad++;
            }
        }
        printf("PXP-G2D-COPY: %s (%d/%d bytes mismatched)\n",
               bad ? "FAIL" : "PASS", bad, COPY_BYTES);
    }

    g2d_free(src);
    g2d_free(dst);
    g2d_close(handle);
    return bad ? 1 : 0;
}
