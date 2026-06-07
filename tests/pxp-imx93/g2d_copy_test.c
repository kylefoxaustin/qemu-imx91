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
#include <stdint.h>
#include <unistd.h>
#include "g2d.h"

#define COPY_BYTES (64 * 1024)

#define FILL_W      64
#define FILL_H      64
#define FILL_COLOR  0x11223344    /* RGBA8888 clrcolor */

/* g2d_clear(): fill a surface rectangle with a constant colour, verify it. */
static int test_fill(void *handle)
{
    struct g2d_buf *buf = g2d_alloc(FILL_W * FILL_H * 4, 0);
    struct g2d_surface s;
    uint32_t *px;
    int i, rc, bad = 0;

    if (!buf) {
        printf("PXP-G2D-FILL: FAIL (g2d_alloc)\n");
        return 1;
    }
    px = buf->buf_vaddr;
    for (i = 0; i < FILL_W * FILL_H; i++) {
        px[i] = 0xdeadbeef;
    }
    g2d_cache_op(buf, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888;
    s.planes[0] = buf->buf_paddr;
    s.left = 0; s.top = 0; s.right = FILL_W; s.bottom = FILL_H;
    s.stride = FILL_W; s.width = FILL_W; s.height = FILL_H;
    s.clrcolor = FILL_COLOR;

    rc = g2d_clear(handle, &s);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(buf, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-FILL: FAIL (g2d_clear/finish rc=%d)\n", rc);
        bad = 1;
    } else {
        for (i = 0; i < FILL_W * FILL_H; i++) {
            if (px[i] != FILL_COLOR) {
                bad++;
            }
        }
        printf("PXP-G2D-FILL: %s (%d/%d px wrong, px0=%#x)\n",
               bad ? "FAIL" : "PASS", bad, FILL_W * FILL_H, px[0]);
    }
    g2d_free(buf);
    return bad ? 1 : 0;
}

#define BLIT_W  64
#define BLIT_H  64

/* g2d_blit(): opaque same-format surface->surface blit, verify dst == src. */
static int test_blit(void *handle)
{
    struct g2d_buf *src = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_buf *dst = g2d_alloc(BLIT_W * BLIT_H * 4, 0);
    struct g2d_surface s, d;
    uint32_t *sp, *dp;
    int i, rc, bad = 0;

    if (!src || !dst) {
        printf("PXP-G2D-BLIT: FAIL (g2d_alloc)\n");
        return 1;
    }
    sp = src->buf_vaddr;
    dp = dst->buf_vaddr;
    for (i = 0; i < BLIT_W * BLIT_H; i++) {
        sp[i] = 0x10000000u + (uint32_t)i;     /* unique per pixel */
        dp[i] = 0xa5a5a5a5u;
    }
    g2d_cache_op(src, G2D_CACHE_FLUSH);
    g2d_cache_op(dst, G2D_CACHE_FLUSH);

    memset(&s, 0, sizeof(s));
    s.format = G2D_RGBA8888; s.planes[0] = src->buf_paddr;
    s.left = 0; s.top = 0; s.right = BLIT_W; s.bottom = BLIT_H;
    s.stride = BLIT_W; s.width = BLIT_W; s.height = BLIT_H;
    s.blendfunc = G2D_ONE; s.global_alpha = 255;
    d = s;
    d.planes[0] = dst->buf_paddr;
    d.blendfunc = G2D_ZERO;

    rc = g2d_blit(handle, &s, &d);
    if (rc == 0) {
        rc = g2d_finish(handle);
    }
    g2d_cache_op(dst, G2D_CACHE_INVALIDATE);

    if (rc != 0) {
        printf("PXP-G2D-BLIT: FAIL (g2d_blit/finish rc=%d)\n", rc);
        bad = 1;
    } else {
        for (i = 0; i < BLIT_W * BLIT_H; i++) {
            if (dp[i] != sp[i]) {
                bad++;
            }
        }
        printf("PXP-G2D-BLIT: %s (%d/%d px wrong, px0=%#x want %#x)\n",
               bad ? "FAIL" : "PASS", bad, BLIT_W * BLIT_H, dp[0], sp[0]);
    }
    g2d_free(src);
    g2d_free(dst);
    return bad ? 1 : 0;
}

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

    /* Second op: constant-colour fill (g2d_clear) via the Store engine. */
    bad += test_fill(handle);

    /* Third op: opaque surface->surface blit (g2d_blit, fetch->store). */
    bad += test_blit(handle);

    g2d_close(handle);
    return bad ? 1 : 0;
}
