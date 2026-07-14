#!/usr/bin/env python3
"""Build a reset-value GOLDEN for the i.MX 91 from the REFERENCE MANUAL PDF.

    pdftotext -layout -f 1 -l 9999 IMX91RM.pdf rm.txt
    ./extract-rm-golden.py rm.txt rm-golden.json

PROVENANCE.  This is a port of mcxn947qemu's RM reset-value extractor
(mcxn947qemu 6ae5f1c4ef), which found four fabricated "always-ready" bits in
its own tree and, ported to rt1180emulator, found a PLL reporting a divider of
ZERO to firmware asking how fast it was running.  The ARCHITECTURE and every
RULE below are theirs.  The PARSER is not, and could not be:

    ⭐ A PARSER IS A MODEL OF A DOCUMENT.  THE i.MX 91 RM IS A DIFFERENT DOCUMENT.

Ported verbatim, mcxn's regexes matched EXACTLY ZERO rows here and the tool
emitted a confident, empty, PASSING golden -- which is the same failure
rt1180emulator hit porting it the other way, and the same failure the tool
exists to catch.  The differences, all of which silently emit nothing:

    MCX N RM                          i.MX 91 RM
    --------                          ----------
    "200h"                            "0"          offsets carry NO trailing 'h'
    "0100_0020h"                      "0202_0001"  resets carry NO trailing 'h'
    range+name on ONE line            offset range is LINE-WRAPPED inside its
                                      table cell:   "0-"  /  " 1E_0000"
    (no Protection column)            a trailing Protection column (Yes/No)
    CMSIS supplies base addresses     THE RM SUPPLIES ITS OWN BASES
                                      ("EDMA3_1.TCD base address: 4401_0000h")

We take bases from the RM rather than from a CMSIS header, deliberately: the
only CMSIS available here is MIMX9352 (the i.MX 93), and the 91 is a chop-down
of it.  A header for a DIFFERENT CHIP is not an independent oracle, it is a
plausible one -- and this whole exercise exists because plausible is how the
fleet has been bleeding.  The RM is the authority for the part we model.

THE RULES, INHERITED, AND NOT NEGOTIABLE (each was paid for by somebody):

  1. VALIDATE AGAINST HAND-READ ANCHORS BEFORE TRUSTING ONE BYTE.  A parser is a
     model of a document, and a model that is its own oracle passes every test.
     ANCHORS below were read out of the PDF BY EYE.  If they don't come back,
     the extractor is wrong and it REFUSES TO EMIT.  (mcxn: "the gate caught a
     bug in the gate" -- ours fired on the first run too; see the log.)

  2. DROP, DO NOT GUESS -- AND COUNT WHAT YOU DROPPED.  A wrong golden is worse
     than a missing one: it makes the CHECKER lie, and then your oracle is the
     thing that needs an oracle.  We drop (and count) array rows whose index is
     ambiguous, and (name,offset) rows where the MANUAL CONTRADICTS ITSELF.

  3. COVERAGE IS A FLOOR, NOT A CEILING -- AND IT IS AN ASSERTION, NOT A PRINT.
     rt1180emulator printed "unmatched: 4371" every run for a day while
     returning PASS, and the blind set contained the two blocks he most needed
     it to see.  AN HONEST NUMBER WITH NO THRESHOLD IS THE SAME ORGAN AS AN
     ALLOWLIST THAT NEVER SHRINKS.  check.py asserts this count.

  4. ARRAYS: DON'T ASK WHERE THE INDEX IS, ASK WHAT VARIES.  (rt1180emulator
     b4f99a0917, after he and mcxn each shipped a regex that was correct on
     their own manual and silently wrong on the other's.)  Split BOTH endpoint
     names into runs of digits and non-digits and compare:
         exactly ONE digit run differs -> that is the index, wherever it sits
         anything else                 -> DROP, and COUNT
     It cannot be wrong about where the index is because it never has to decide,
     and it REFUSES 2-D arrays (CTX0_CTR0 - CTX3_CTR1) instead of inventing a
     stride for them.

  ⚠ 5. THE RM's RESET COLUMN IS THE COLD-POR VALUE.  IT IS NOT WHAT FIRMWARE SEES
     ON A BOARD WITH A BOOT ROM.  (rt1180emulator, and it is the one thing that
     can turn this tool from an oracle into a bug factory.)  We boot -kernel and
     skip both the ROM and U-Boot, so cold-POR is closer to right for us than for
     most -- but where it is NOT, the deviation is ALLOWLISTED WITH A REASON.
     A deviation with a reason is a decision; a deviation without one is a bug
     you have agreed not to look at.
"""
import collections, json, re, sys

# ---------------------------------------------------------------------------
# ANCHORS -- hand-read out of IMX91RM.pdf rev 5 by eye.  Rule 1.
# Two are NON-ZERO on purpose: an extractor that silently emits 0 for everything
# would sail through an all-zero anchor set, and "0" is the value we are most
# suspicious of in the model.  (inst, reg, addr, reset)
# ---------------------------------------------------------------------------
ANCHORS = [
    ("FSB1",      "VERID",   0x47510000, 0x02020001),  # §8.4.1  FSB memory map
    ("EDMA3_1",   "CH0_SBR", 0x4401000C, 0x00008007),  # §4.6.2.1 TCD memory map
    ("CCM_CTRL",  "CLOCK_ROOT0_CONTROL",
                             0x44450000, 0x00000000),  # a real zero, asserted
    # ⭐ AN ANCHOR ON THE SUB-BLOCK CLASS BELOW.  Without the override this row
    #    lands at 4268_0010h and the gate REFUSES rather than silently comparing
    #    the RM's control registers against the XCVR's frame RAM.
    ("SPDIF",     "EXT_CTRL", 0x42680810, 0x18004040),
    # The eDMA4 channel stride the DRIVER and the DEVICE TREE use (0x8000), not the
    # 0x1000 the RM's own table implies.  Anchored so the override cannot vanish.
    ("EDMA4_2.TCD", "CH1_SBR", 0x4201800C, 0x00008007),
]

#
# ⭐ THE RM DECLARES ONE BASE AND THEN NUMBERS A SUB-BLOCK FROM ZERO.
#
#     "THE RM USES DIFFERENT OFFSET BASES IN DIFFERENT SECTIONS."   -- mcxn947qemu
#
# I told mcxn this class was structurally absent from my tree because my golden keys
# on ADDRESS rather than NAME.  That was true, and it was the wrong reassurance: an
# address-keyed golden is only as good as the ADDRESS, and the RM computes it from a
# base it does not use.
#
#     SPDIF base address: 4268_0000h        <- what the RM declares
#     EXT_CTRL @ 0x010                      <- but numbered from the CONTROL sub-block
#
# The device tree is the ADDRESS oracle -- a different document, by different authors,
# and the map the GUEST actually uses:
#
#     xcvr@42680000  reg = <0x42680000 0x800>,   "ram"
#                          <0x42680800 0x400>,   "regs"     <- EXT_CTRL is +0x10 HERE
#                          <0x42680c00 0x080>,   "rxfifo"
#                          <0x42680e00 0x080>;   "txfifo"
#
# and sound/soc/fsl/fsl_xcvr.h agrees: FSL_XCVR_EXT_CTRL = 0x10, relative to "regs".
#
# So all 59 SPDIF rows were 0x800 too low -- INTO THE FRAME RAM -- and every "XCVR
# deviation" this gate has ever reported was an artifact of my own arithmetic.
#
#     ⭐ A WRONG GOLDEN MAKES THE CHECKER LIE, AND THEN YOUR ORACLE IS THE THING THAT
#        NEEDS AN ORACLE.
#
# Use each source for what it actually knows: THE RM FOR VALUES, THE DEVICE TREE FOR
# ADDRESSES.  Each override below is hand-verified against the DTB and the driver, and
# is pinned by an ANCHOR above so it cannot silently regress.
#
INSTANCE_BASE_OVERRIDE = {
    # instance : true base of the register table the RM numbers from zero
    "SPDIF": 0x42680800,    # DTB reg-names "regs"; fsl_xcvr.h EXT_CTRL = 0x10
}

#
# ⭐ AND SOMETIMES THE RM IS NOT MERELY USING A DIFFERENT BASE.  IT IS WRONG.
#
# The RM's EDMA4_2 TCD summary row reads
#
#     0 - 3_F000   Channel Control and Status (CH0_CSR - CH63_CSR)
#
# which is 0x3F000 across 63 gaps = a channel stride of 0x1000.  THREE independent
# sources say otherwise, and they are the ones the silicon actually obeys:
#
#     drivers/dma/fsl-edma-main.c   imx93_data4.chreg_space_sz = 0x8000
#                                   (chan base = membase + i*chreg_space_sz + chreg_off)
#     imx93.dtsi                    edma2 reg = <0x42000000 0x210000>, 64 channels
#                                   0x210000 = chreg_off 0x10000 + 64 * 0x8000  ✔
#                                   at a 0x1000 stride the block would need 0x50000
#     the model                     reads 8007h at CH1 and CH63 on the 0x8000 stride
#
# So the RM's ADDRESS ARITHMETIC contradicts the hardware, and my golden believed it:
# 56 of the 64 channels were being probed at addresses no channel occupies, and the
# gate reported 56 lies in a model that was CORRECT.  (Every 8th channel "matched" --
# 0x8000 / 0x1000 = 8 -- which is the tell.)
#
#     ⭐ THE RM IS AUTHORITATIVE FOR VALUES.  THE DRIVER AND THE DEVICE TREE ARE
#        AUTHORITATIVE FOR ADDRESSES.  USE EACH SOURCE FOR WHAT IT ACTUALLY KNOWS.
#                                       -- mcxn947qemu / rt1180emulator, twice over
#
# Pinned by an anchor below, so removing this override makes the extractor REFUSE
# rather than quietly resume slandering a correct model.
#
INSTANCE_ARRAY_STRIDE = {
    "EDMA4_2.TCD": 0x8000,
}

BASE_RE  = re.compile(r'^([A-Za-z0-9_.]+)\s+base address:\s*([0-9A-Fa-f_]+)h\s*$')
HEXNUM   = r'[0-9A-Fa-f][0-9A-Fa-f_]*'

# A table row in -layout mode.  TWO cells can wrap onto a continuation line, and
# each one, left unhandled, silently deletes a whole class of registers:
#
#   * the OFFSET cell, when it is a range:      "0-"  /  " 1E_0000"
#     -> without this, every ARRAY register on the chip vanishes (eDMA channels).
#
#   * the NAME cell, when the description is long:
#         "0   SW_MUX_CTL_PAD_DAP_TDI SW MUX Control Register   32  RW  0000_0000"
#         "    (SW_MUX_CTL_PAD_DAP_TDI)"
#     -> without this, THE ENTIRE PINMUX (IOMUXC1) vanishes.  It did.  The total
#        coverage count did not move, because a total cannot see a missing block.
#
# So the name is OPTIONAL here and recovered from the continuation line below.
# (This is rt1180emulator's original bug report to mcxn -- "the RM prints rows on
# FIVE lines, not four" -- arriving in a third manual, on a different cell.)
ROW_RE = re.compile(
    r'^\s{1,12}(' + HEXNUM + r')\s*(-)?\s{2,}'     # offset [range-open]
    r'(.+?)\s{2,}'                                 # description [ (NAME) ]
    r'(\d{1,3})\s{2,}'                             # width
    r'([A-Za-z/ ]{1,12}?)\s{2,}'                   # access
    r'(' + HEXNUM + r')'                           # RESET VALUE
    r'(?:\s{2,}(?:Yes|No))?\s*$'                   # optional Protection
)
NAME_IN_DESC = re.compile(r'\(([^()]+)\)\s*$')
CONT_OFF_RE  = re.compile(r'^\s{0,3}(' + HEXNUM + r')\s*$')
# The DESCRIPTION can wrap too, so the continuation line is not always a bare
# "(NAME)" -- it can be "Register (SW_PAD_CTL_PAD_DAP_TDO_TRACESWO)". Requiring a
# bare name silently refused every pad whose description ran long, including the
# TRACE/debug pad and the PDM mic pads, which reset NON-ZERO.
CONT_NAME_RE = re.compile(r'^\s+[^()]*\(([^()]+)\)\s*$')

# ⭐ A ROW MUST LOOK LIKE PROSE, NOT LIKE A BIT DIAGRAM.
#
# ROW_RE also matched the RM's BITFIELD DIAGRAMS -- rows of "0   0   1   —" whose
# "offset" is a bit number.  They were refused only because they carry no (NAME) in
# parentheses, i.e. CORRECT BY LUCK.  A register we invent from a bit diagram would
# be a FALSE WITNESS, and a wrong golden makes the CHECKER lie.
DESC_IS_PROSE = re.compile(r'[A-Za-z]{3}')

NOISE = ("NXP Semiconductors", "Reference Manual", "Table continues",
         "i.MX 91 Applications Processor")

# ⭐ A REFUSAL YOU DO NOT COUNT IS NOT EVEN A REFUSAL.
#
# ROW_RE demands a HEX reset value, so a row whose reset column reads "See section"
# does not match it AT ALL -- it is not dropped, it is INVISIBLE.  mcxn947qemu's PORT
# block refused exactly this shape (the RM declines to answer because the value is
# PER-INSTANCE) and the refused rows were the SWD DEBUG PINS.
#
#     A REFUSAL IS NOT A CHECK -- AND AN UNCOUNTED REFUSAL IS NOT EVEN A REFUSAL.
#
# So they are matched separately, COUNTED, and PRINTED.
DEFERRED_RE = re.compile(
    r'^\s{1,12}(' + HEXNUM + r')\s{2,}(.+?)\s{2,}(\d{1,3})\s{2,}'
    r'([A-Za-z/ ]{1,12}?)\s{2,}(See section|Refer.*)\s*$')


def h(s):
    return int(s.replace("_", ""), 16)



def runs(name):
    """Split a name into alternating runs of non-digits and digits."""
    return re.findall(r'\d+|\D+', name)


def array_index(lo, hi):
    """Rule 4: which run VARIES?  Returns (prefix_runs, i, start, end) or None.

    Not 'where is the index' -- 'what actually varies'.  Exactly one differing
    digit run is an array; anything else is refused, including 2-D arrays whose
    stride no single number can describe.
    """
    a, b = runs(lo), runs(hi)
    if len(a) != len(b):
        return None
    diff = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    if len(diff) != 1:
        return None                     # 0 -> not an array; >1 -> 2-D. REFUSE.
    i = diff[0]
    if not (a[i].isdigit() and b[i].isdigit()):
        return None
    return a, i, int(a[i]), int(b[i])


def main(rm_txt, out_json):
    lines = open(rm_txt, errors="replace").read().splitlines()

    rows = []            # (inst_list, name, off, off_end, reset)
    dropped_ambig = 0    # array rows we refused (rule 4)
    dropped_2d = 0
    unnamed = []         # rows whose NAME cell we could not find -- dropped
    deferred = []        # rows where the RM DECLINES to give a reset value
    rows_since_base = False
    declared = []        # every instance the RM declares a base address for
    insts, i = [], 0

    while i < len(lines):
        line = lines[i]

        m = BASE_RE.match(line)
        if m:
            # A run of base-address lines = the instances sharing the ONE register
            # table that follows (LPI2C1..LPI2C8, then a single table).
            #
            # DO NOT define the run by line adjacency.  A base-address line can be
            # the last thing on a page, with the footer, the running chapter title
            # and the table all landing overleaf -- so "the previous line" breaks
            # the run and SILENTLY DISCARDS the instance.  That is exactly how
            # USB.USBNC_OTG1 and the three SRC slices went blind while USBNC_OTG2
            # (whose base sits *after* the page break) came through fine.  Trying to
            # enumerate the furniture is a losing game: the running header carries
            # the CHAPTER TITLE, so the noise list would have to know every chapter.
            #
            # Use the document's structure instead: bases ACCUMULATE, and the group
            # closes when its table's first row arrives.
            if rows_since_base:
                insts = []
                rows_since_base = False
            nm_i = m.group(1)
            b = INSTANCE_BASE_OVERRIDE.get(nm_i, h(m.group(2)))
            insts.append((nm_i, b, i))
            declared.append(nm_i)
            i += 1
            continue

        m = DEFERRED_RE.match(line)
        if m and insts:
            nm = NAME_IN_DESC.search(m.group(2))
            deferred.append((insts[-1][0], h(m.group(1)),
                             nm.group(1) if nm else m.group(2).strip()[:34]))
            i += 1
            continue

        m = ROW_RE.match(line)
        if m and insts:
            off_s, open_range, desc, width_s, _acc, reset_s = m.groups()
            off = h(off_s)
            off_end = None
            name = None

            if not DESC_IS_PROSE.search(desc):
                i += 1
                continue        # a bitfield-diagram row, not a register

            nm = NAME_IN_DESC.search(desc)
            if nm:
                name = nm.group(1)

            # Look ahead for wrapped cells: the offset-range end and/or the name.
            j = i + 1
            while j < len(lines) and (off_end is None or name is None):
                nxt = lines[j]
                if not nxt.strip():
                    j += 1
                    continue
                if open_range and off_end is None:
                    c = CONT_OFF_RE.match(nxt)
                    if c:
                        off_end = h(c.group(1))
                        i = j
                        j += 1
                        continue
                if name is None:
                    c = CONT_NAME_RE.match(nxt)
                    if c:
                        name = c.group(1)
                        i = j
                        j += 1
                        continue
                break

            # DROP, DO NOT GUESS: a row whose name we never found is not a
            # register we may invent a name for.  It is counted, below.
            if name is None:
                unnamed.append((insts[-1][0], off, h(reset_s)))
            else:
                rows_since_base = True
                rows.append(([b for b, _, _ in insts],
                             [a for _, a, _ in insts],
                             name.strip(), off, off_end, h(reset_s),
                             int(width_s)))
        i += 1

    # ---- expand arrays (rule 4) + emit one row per instance -----------------
    golden = []
    for inst_names, bases, name, off, off_end, reset, width in rows:
        if "-" in name:
            lo, hi = [p.strip() for p in name.split("-", 1)]
            r = array_index(lo, hi)
            if r is None or off_end is None:
                dropped_2d += 1 if r is None else 0
                dropped_ambig += 1
                continue
            parts, idx, start, end = r
            n = end - start + 1
            if n < 2 or off_end <= off or (off_end - off) % (n - 1):
                dropped_ambig += 1
                continue
            stride = INSTANCE_ARRAY_STRIDE.get(inst_names[0],
                                              (off_end - off) // (n - 1))
            for k in range(n):
                nm = "".join(parts[:idx]) + str(start + k) + "".join(parts[idx + 1:])
                for inm, base in zip(inst_names, bases):
                    golden.append({"inst": inm, "reg": nm,
                                   "addr": base + off + k * stride,
                                   "reset": reset, "width": width})
        else:
            for inm, base in zip(inst_names, bases):
                golden.append({"inst": inm, "reg": name,
                               "addr": base + off, "reset": reset,
                               "width": width})

    # ---- rule 2: the MANUAL CONTRADICTS ITSELF.  DROP, and COUNT. ----------
    # rt1180emulator: "you drop a row only when CMSIS attributes it to >1
    # peripheral TYPE.  That does NOT cover the case where the MANUAL is the
    # ambiguous one, and there your tool emits one of the two values ARBITRARILY
    # and calls it a golden."  A wrong golden makes the CHECKER lie.
    by_addr = {}
    for g in golden:
        by_addr.setdefault(g["addr"], set()).add(g["reset"])
    conflicts = {a for a, v in by_addr.items() if len(v) > 1}
    conflict_rows = sum(1 for g in golden if g["addr"] in conflicts)
    golden = [g for g in golden if g["addr"] not in conflicts]

    # de-dup identical (addr, reset)
    seen, uniq = set(), []
    for g in golden:
        if g["addr"] in seen:
            continue
        seen.add(g["addr"])
        uniq.append(g)
    golden = uniq

    # ---- rule 1: THE ANCHOR GATE.  Refuse to emit if the parser is wrong. ---
    idx = {g["addr"]: g for g in golden}
    bad = []
    for inst, reg, addr, reset in ANCHORS:
        g = idx.get(addr)
        if g is None:
            bad.append("%s %s @0x%08x  MISSING from the golden" % (inst, reg, addr))
        elif g["reset"] != reset:
            bad.append("%s %s @0x%08x  golden=0x%08x  hand-read=0x%08x"
                       % (inst, reg, addr, g["reset"], reset))
    if bad:
        print("ANCHOR GATE FAILED -- the extractor is wrong, so it will NOT emit.")
        print("A parser is a model of a document; a model that is its own oracle")
        print("passes every test.  These were read out of the PDF BY EYE:")
        for b in bad:
            print("    " + b)
        sys.exit(2)

    #
    # ⭐ THE BLIND-BLOCK GATE.  A TOTAL CANNOT SEE A MISSING PERIPHERAL.
    #
    # The first version of this extractor printed "6388 registers" and passed --
    # while the ENTIRE PINMUX (IOMUXC1, every SW_MUX_CTL_PAD_* on the chip) was
    # invisible, because those rows wrap their NAME onto a continuation line.  The
    # total never moved, so nothing complained.
    #
    #     A NUMBER WITH NO EXPECTED VALUE IS A FACT, NOT A CONTROL.  -- rt1180emulator
    #
    # and the corollary that cost me: an ASSERTED TOTAL is still only a control on
    # the TOTAL.  A whole block can vanish inside a coverage number that is itself
    # correctly asserted.  So: the RM declares a base address for every instance it
    # documents.  THAT is the expected set, it lives OUTSIDE the parser, and an
    # instance that yields ZERO registers is a PARSER FAILURE -- not an empty
    # peripheral -- and it REFUSES TO EMIT.
    #
    covered = {g["inst"] for g in golden}
    blind = [d for d in sorted(set(declared)) if d not in covered]
    if blind:
        print("BLIND BLOCK GATE FAILED -- the extractor will NOT emit.")
        print("The RM declares a base address for these instances and the parser")
        print("produced ZERO registers for them.  A total cannot see a missing block:")
        for b in blind:
            print("    %s" % b)
        sys.exit(2)

    json.dump(golden, open(out_json, "w"), indent=1)

    print("golden written: %s" % out_json)
    print("  registers          : %d   <-- COVERAGE IS A FLOOR, NOT A CEILING" % len(golden))
    print("  instances          : %d / %d declared by the RM  [none blind]"
          % (len(covered), len(set(declared))))
    print("  anchors asserted   : %d   (hand-read from the PDF; all matched)" % len(ANCHORS))
    print("  array rows refused : %d   (index ambiguous / 2-D -- DROPPED, not guessed)"
          % dropped_ambig)
    #
    # ⭐ A REFUSAL IS NOT A CHECK.  (mcxn947qemu, whose extractor correctly refused
    #    every PORT PCR row -- and the refused rows turned out to be the SWD DEBUG
    #    PINS, reset non-zero, read-modify-written by firmware.  The refusal was
    #    right.  Never looking at the refusals was not.)
    #
    # A refused row you never revisit is an allowlist that never shrinks.  So the
    # refusals are PRINTED, not merely counted -- and the ones with a NON-ZERO reset
    # are the ones that can hurt.
    #
    if deferred:
        print("  RM DECLINES to give a reset for %d row(s) -- UNCHECKED, and the RM"
              % len(deferred))
        print("      says so on purpose (the value is per-instance, or documented"
              " elsewhere):")
        for inst, off, nm in deferred[:10]:
            print("        %-16s +0x%04x  %s" % (inst, off, nm))
        if len(deferred) > 10:
            print("        ... and %d more" % (len(deferred) - 10))

    hot = [u for u in unnamed if u[2] != 0]
    print("  unnamed rows       : %d   (NAME cell not found -- DROPPED, not invented)"
          % len(unnamed))
    if hot:
        print("      of which %d have a NON-ZERO reset and are therefore UNCHECKED:" % len(hot))
        for inst, off, reset in hot[:12]:
            print("        %-16s +0x%04x  reset=0x%08x" % (inst, off, reset))
    print("  RM self-conflicts  : %d rows across %d addresses -- DROPPED"
          % (conflict_rows, len(conflicts)))

    #
    # ⭐ WIDTH.  DO NOT KEEP ONLY THE 32-BIT REGISTERS -- AND DO NOT READ THE
    #    OTHERS AS IF THEY WERE.
    #
    # rt1180emulator's gate kept only 32-bit registers and dropped 410 others, and
    # the blind set was THE ENTIRE MOTOR DRIVE: eFlexPWM's DTCNT (dead-time count)
    # resets to 07FFh on silicon and their memset made it ZERO.
    #
    #     ZERO DEAD TIME IS A DIRECT SHORT ACROSS THE DC BUS, THROUGH BOTH
    #     TRANSISTORS OF AN INVERTER LEG.  Every PWM test was green.
    #
    # Mine did something subtly worse: it KEPT them and read them all with readl.
    # An 8-bit register read 32 bits wide pulls in its three neighbours, and a
    # 16-bit register at an odd halfword is a MISALIGNED read.  That does not
    # merely lose a register -- IT INVENTS A COMPARISON.  A false match is a lie
    # you will never see.
    #
    by_w = collections.Counter(g["width"] for g in golden)
    print("  widths             : %s   (probed with readb/readw/readl/readq)"
          % ", ".join("%d-bit x%d" % (w, n) for w, n in sorted(by_w.items())))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
