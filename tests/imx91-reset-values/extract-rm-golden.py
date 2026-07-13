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
import json, re, sys

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
]

BASE_RE  = re.compile(r'^([A-Za-z0-9_.]+)\s+base address:\s*([0-9A-Fa-f_]+)h\s*$')
HEXNUM   = r'[0-9A-Fa-f][0-9A-Fa-f_]*'

# A table row in -layout mode.  The offset cell may be "0", or "0-" when the
# range end is wrapped onto the following line.  Protection column is optional.
ROW_RE = re.compile(
    r'^\s{1,12}(' + HEXNUM + r')\s*(-)?\s{2,}'     # offset [range-open]
    r'(.+?)\s*\(([^()]+)\)\s{2,}'                  # description (NAME[ - NAME])
    r'(\d{1,3})\s{2,}'                             # width
    r'([A-Za-z/ ]{1,12}?)\s{2,}'                   # access
    r'(' + HEXNUM + r')'                           # RESET VALUE
    r'(?:\s{2,}(?:Yes|No))?\s*$'                   # optional Protection
)
CONT_RE = re.compile(r'^\s{0,3}(' + HEXNUM + r')\s*$')

NOISE = ("NXP Semiconductors", "Reference Manual", "Table continues",
         "i.MX 91 Applications Processor")


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
    insts, i = [], 0

    while i < len(lines):
        line = lines[i]

        m = BASE_RE.match(line)
        if m:
            # A run of consecutive base-address lines = the instances sharing
            # the table that follows (LPI2C1..LPI2C8, then ONE register table).
            if not insts or insts[-1][2] != i - 1:
                insts = []
            insts.append((m.group(1), h(m.group(2)), i))
            i += 1
            continue

        m = ROW_RE.match(line)
        if m and insts:
            off_s, open_range, _desc, name, _w, _acc, reset_s = m.groups()
            off = h(off_s)
            off_end = None
            if open_range:
                # the range end is wrapped onto the next non-blank line
                j = i + 1
                while j < len(lines) and not lines[j].strip():
                    j += 1
                if j < len(lines):
                    c = CONT_RE.match(lines[j])
                    if c:
                        off_end = h(c.group(1))
                        i = j
            rows.append(([b for b, _, _ in insts],
                         [a for _, a, _ in insts],
                         name.strip(), off, off_end, h(reset_s)))
        i += 1

    # ---- expand arrays (rule 4) + emit one row per instance -----------------
    golden = []
    for inst_names, bases, name, off, off_end, reset in rows:
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
            stride = (off_end - off) // (n - 1)
            for k in range(n):
                nm = "".join(parts[:idx]) + str(start + k) + "".join(parts[idx + 1:])
                for inm, base in zip(inst_names, bases):
                    golden.append({"inst": inm, "reg": nm,
                                   "addr": base + off + k * stride,
                                   "reset": reset})
        else:
            for inm, base in zip(inst_names, bases):
                golden.append({"inst": inm, "reg": name,
                               "addr": base + off, "reset": reset})

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

    json.dump(golden, open(out_json, "w"), indent=1)

    print("golden written: %s" % out_json)
    print("  registers          : %d   <-- COVERAGE IS A FLOOR, NOT A CEILING" % len(golden))
    print("  anchors asserted   : %d   (hand-read from the PDF; all matched)" % len(ANCHORS))
    print("  array rows refused : %d   (index ambiguous / 2-D -- DROPPED, not guessed)"
          % dropped_ambig)
    print("  RM self-conflicts  : %d rows across %d addresses -- DROPPED"
          % (conflict_rows, len(conflicts)))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
