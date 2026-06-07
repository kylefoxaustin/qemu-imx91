#!/usr/bin/env python3
# Copyright (c) 2026, Kyle Fox <kylefoxaustin@github>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Generate the committed data for tests/qtest/ethos-u-test.c: a single int8
# convolution, laid out in guest DRAM exactly as the NPU consumes it - a
# Vela-style command stream, an mlw-encoded weight stream, a per-channel
# scale/bias stream and an IFM - plus the expected OFM.
#
# The weights are reordered+encoded with the real Arm mlw_codec and the scales
# packed with Vela's encode_bias, so the qtest exercises the full in-QEMU path
# (DMA fetch -> mlw decode -> reorder inverse -> scale unpack -> int8 conv ->
# requant -> OFM writeback). The golden OFM is the reference pipeline output
# (the same integer math the kernels implement); no TensorFlow needed.
#
# Re-run after changing the case:  python3 tests/qtest/ethos-u-test-gen.py

import math
import numpy as np
import ethosu.mlw_codec as mlw
from ethosu.vela.weight_compressor import encode_bias

OUT = "tests/qtest/ethos-u-test-data.h"

# --- the convolution under test (single brick, single core) ---
IFM_H, IFM_W, IFM_C = 8, 8, 4
OFM_C = 8
K = 3                      # 3x3, stride 1, VALID padding
OFM_H, OFM_W = IFM_H - K + 1, IFM_W - K + 1
OFM_BLK_DEPTH = 16         # >= OFM_C -> single ofm block
IFM_ZP, OFM_ZP = -5, 3

# --- guest DRAM layout (DDR @ 0x80000000) ---
WS_BASE = 0x80100000       # region 0: scales then weights
ARENA_BASE = 0x80200000    # region 1: ifm then ofm
CMS_BASE = 0x80300000      # command stream
SCALE_OFF = 0x0
WEIGHT_OFF = 0x1000
IFM_OFF = 0x0
OFM_OFF = 0x10000

# --- reference integer pipeline (mirrors the C kernels) ---
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1


def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x >= (1 << 31) else x


def _ctrunc(n, d):
    q = abs(n) // d
    return q if n >= 0 else -q


def srdhm(a, b):
    a, b = i32(a), i32(b)
    ov = a == b == INT32_MIN
    ab = a * b
    nudge = (1 << 30) if ab >= 0 else (1 - (1 << 30))
    return INT32_MAX if ov else i32(_ctrunc(ab + nudge, 1 << 31))


def rdpot(x, e):
    x = i32(x)
    if e <= 0:
        return x
    mask = (1 << e) - 1
    rem = x & mask
    thr = (mask >> 1) + (1 if x < 0 else 0)
    return (x >> e) + (1 if rem > thr else 0)


def mqm(x, m, s):
    left = s if s > 0 else 0
    right = 0 if s > 0 else -s
    return rdpot(srdhm(i32(x * (1 << left)), m), right)


def quantise_scale(sc):
    sig, exp = math.frexp(sc)
    s = sig * (1 << 31)
    q = int(math.floor(s + 0.5)) if s >= 0 else int(math.ceil(s - 0.5))
    return q, 31 - exp


def ref_conv(ifm, w, bias, mult, vshift):
    ofm = np.zeros((OFM_H, OFM_W, OFM_C), np.int8)
    for oy in range(OFM_H):
        for ox in range(OFM_W):
            for oc in range(OFM_C):
                acc = int(bias[oc])
                for ky in range(K):
                    for kx in range(K):
                        for ic in range(IFM_C):
                            acc += (int(ifm[oy + ky, ox + kx, ic]) - IFM_ZP) \
                                * int(w[oc, ky, kx, ic])
                v = mqm(acc, i32(mult[oc]), 31 - vshift[oc]) + OFM_ZP
                ofm[oy, ox, oc] = max(-128, min(127, v))
    return ofm


# --- command-stream emitter (matches ethos_u_cmdstream.c framing) ---
PAYLOAD = 0x4000
# cmd0 opcodes
SET_IFM_PAD_TOP = 0x100
SET_IFM_PAD_LEFT = 0x101
SET_IFM_PAD_RIGHT = 0x102
SET_IFM_PAD_BOTTOM = 0x103
SET_IFM_DEPTH_M1 = 0x104
SET_IFM_PRECISION = 0x105
SET_IFM_ZERO_POINT = 0x109
SET_IFM_WIDTH0_M1 = 0x10a
SET_IFM_HEIGHT0_M1 = 0x10b
SET_IFM_REGION = 0x10f
SET_OFM_WIDTH_M1 = 0x111
SET_OFM_HEIGHT_M1 = 0x112
SET_OFM_DEPTH_M1 = 0x113
SET_OFM_PRECISION = 0x114
SET_OFM_BLK_DEPTH_M1 = 0x117
SET_OFM_ZERO_POINT = 0x118
SET_OFM_REGION = 0x11f
SET_KERNEL_WIDTH_M1 = 0x120
SET_KERNEL_HEIGHT_M1 = 0x121
SET_KERNEL_STRIDE = 0x122
SET_ACTIVATION_MIN = 0x126
SET_ACTIVATION_MAX = 0x127
SET_WEIGHT_REGION = 0x128
SET_SCALE_REGION = 0x129
OP_CONV = 0x002
OP_STOP = 0x000
# cmd1 opcodes
SET_IFM_BASE0 = 0x000
SET_IFM_STRIDE_X = 0x004
SET_IFM_STRIDE_Y = 0x005
SET_IFM_STRIDE_C = 0x006
SET_OFM_BASE0 = 0x010
SET_OFM_STRIDE_X = 0x014
SET_OFM_STRIDE_Y = 0x015
SET_OFM_STRIDE_C = 0x016
SET_WEIGHT_BASE = 0x020
SET_WEIGHT_LENGTH = 0x021
SET_SCALE_BASE = 0x022


class CMS:
    def __init__(self):
        self.b = bytearray()

    def cmd0(self, op, imm=0):
        self.b += int(op & 0xffff).to_bytes(2, "little")
        self.b += int(imm & 0xffff).to_bytes(2, "little")

    def cmd1(self, op, data):
        self.b += int((op | PAYLOAD) & 0xffff).to_bytes(2, "little")
        self.b += (0).to_bytes(2, "little")
        self.b += int(data & 0xffffffff).to_bytes(4, "little")


def c_bytes(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12])
                     + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    rng = np.random.default_rng(0x93e)
    w = rng.integers(-100, 101, (OFM_C, K, K, IFM_C), dtype=np.int16)
    ifm = rng.integers(-128, 128, (IFM_H, IFM_W, IFM_C), dtype=np.int8)
    bias = rng.integers(-5000, 5000, OFM_C, dtype=np.int64)
    scales = [0.0009 + 0.0003 * j for j in range(OFM_C)]
    mult, vshift = zip(*[quantise_scale(s) for s in scales])

    ofm = ref_conv(ifm, w, bias, mult, vshift)

    # mlw-encode the weights (depth-first, single core, ifm 8-bit, decomp 8)
    enc, _ = mlw.reorder_encode(8, 8, w, OFM_BLK_DEPTH, 0, 0, 8, 8, 8)
    enc = bytes(enc)

    scale_stream = bytearray()
    for oc in range(OFM_C):
        scale_stream += encode_bias(np.int64(int(bias[oc])),
                                    int(mult[oc]), int(vshift[oc]))

    # IFM/OFM are NHWC int8: stride_c=1, stride_x=C, stride_y=C*W
    ifm_sx, ifm_sy, ifm_sc = IFM_C, IFM_C * IFM_W, 1
    ofm_sx, ofm_sy, ofm_sc = OFM_C, OFM_C * OFM_W, 1

    # IFM precision: 8-bit signed, NHWC -> bit0 signed, activation_precision 0
    ifm_prec = 1
    ofm_prec = 1

    c = CMS()
    c.cmd0(SET_IFM_REGION, 1)
    c.cmd1(SET_IFM_BASE0, IFM_OFF)
    c.cmd0(SET_IFM_WIDTH0_M1, IFM_W - 1)
    c.cmd0(SET_IFM_HEIGHT0_M1, IFM_H - 1)
    c.cmd0(SET_IFM_DEPTH_M1, IFM_C - 1)
    c.cmd0(SET_IFM_PRECISION, ifm_prec)
    c.cmd0(SET_IFM_ZERO_POINT, IFM_ZP & 0xffff)
    c.cmd1(SET_IFM_STRIDE_X, ifm_sx)
    c.cmd1(SET_IFM_STRIDE_Y, ifm_sy)
    c.cmd1(SET_IFM_STRIDE_C, ifm_sc)
    c.cmd0(SET_IFM_PAD_TOP, 0)
    c.cmd0(SET_IFM_PAD_LEFT, 0)
    c.cmd0(SET_IFM_PAD_RIGHT, 0)
    c.cmd0(SET_IFM_PAD_BOTTOM, 0)

    c.cmd0(SET_OFM_REGION, 1)
    c.cmd1(SET_OFM_BASE0, OFM_OFF)
    c.cmd0(SET_OFM_WIDTH_M1, OFM_W - 1)
    c.cmd0(SET_OFM_HEIGHT_M1, OFM_H - 1)
    c.cmd0(SET_OFM_DEPTH_M1, OFM_C - 1)
    c.cmd0(SET_OFM_PRECISION, ofm_prec)
    c.cmd0(SET_OFM_ZERO_POINT, OFM_ZP & 0xffff)
    c.cmd0(SET_OFM_BLK_DEPTH_M1, OFM_BLK_DEPTH - 1)
    c.cmd1(SET_OFM_STRIDE_X, ofm_sx)
    c.cmd1(SET_OFM_STRIDE_Y, ofm_sy)
    c.cmd1(SET_OFM_STRIDE_C, ofm_sc)

    c.cmd0(SET_KERNEL_WIDTH_M1, K - 1)
    c.cmd0(SET_KERNEL_HEIGHT_M1, K - 1)
    c.cmd0(SET_KERNEL_STRIDE, 0)        # stride 1, depth-first, dilation 1

    c.cmd0(SET_WEIGHT_REGION, 0)
    c.cmd1(SET_WEIGHT_BASE, WEIGHT_OFF)
    c.cmd1(SET_WEIGHT_LENGTH, len(enc))
    c.cmd0(SET_SCALE_REGION, 0)
    c.cmd1(SET_SCALE_BASE, SCALE_OFF)
    c.cmd0(SET_ACTIVATION_MIN, (-128) & 0xffff)
    c.cmd0(SET_ACTIVATION_MAX, 127)
    c.cmd0(OP_CONV, 0)
    c.cmd0(OP_STOP, 0)

    out = ["/*",
           " * Auto-generated by tests/qtest/ethos-u-test-gen.py - do not edit.",
           " * Single int8 conv: command stream + mlw weights + scales + IFM,",
           " * with the expected OFM (reference pipeline).",
           " *",
           " * SPDX-License-Identifier: GPL-2.0-or-later",
           " */",
           "#ifndef ETHOS_U_TEST_DATA_H",
           "#define ETHOS_U_TEST_DATA_H",
           "",
           f"#define EU_WS_BASE     0x{WS_BASE:08x}ULL",
           f"#define EU_ARENA_BASE  0x{ARENA_BASE:08x}ULL",
           f"#define EU_CMS_BASE    0x{CMS_BASE:08x}ULL",
           f"#define EU_SCALE_OFF   0x{SCALE_OFF:x}",
           f"#define EU_WEIGHT_OFF  0x{WEIGHT_OFF:x}",
           f"#define EU_IFM_OFF     0x{IFM_OFF:x}",
           f"#define EU_OFM_OFF     0x{OFM_OFF:x}",
           f"#define EU_OFM_LEN     {OFM_H * OFM_W * OFM_C}",
           "",
           c_bytes("eu_cms", c.b),
           c_bytes("eu_weights", enc),
           c_bytes("eu_scales", bytes(scale_stream)),
           c_bytes("eu_ifm", ifm.reshape(-1).astype(np.uint8).tobytes()),
           c_bytes("eu_ofm_golden",
                   ofm.reshape(-1).astype(np.uint8).tobytes()),
           "",
           "#endif /* ETHOS_U_TEST_DATA_H */"]

    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {OUT}: cms={len(c.b)}B weights={len(enc)}B "
          f"scales={len(scale_stream)}B ofm={OFM_H * OFM_W * OFM_C}")


if __name__ == "__main__":
    main()
