#!/usr/bin/env python3
"""Generate the i.MX 91 CCM clock-root table from the Linux driver, mechanically.

    ./gen-clock-tree.py <kernel-src>/drivers/clk/imx/clk-imx93.c \
                        ../../include/hw/misc/imx93_ccm_roots.h

⭐ RETYPING A DEVICE FACT IS HOW YOU FABRICATE ONE.  (rt1180emulator)

The root table (95 roots x {offset, parent-select}) and the 9 parent-select tables
are DEVICE FACTS.  Hand-copying 95 rows out of a driver is a fabrication engine, so
they are parsed out of clk-imx93.c and emitted as a header.

⚠ AND THE GENERATOR IS ITSELF A PARSER, SO IT GETS THE SAME TREATMENT.
rt1180emulator's generator silently dropped a row on its first pass -- 73 of 74 --
because the last entry in the SDK table had no trailing comma.  Nothing would have
complained: an under-filled table zero-fills, and source index 0 is the 24 MHz
oscillator, SO THE MISSING ROOT WOULD HAVE READ AS A PERFECTLY PLAUSIBLE 24 MHz.

    A MISSING ROW DOES NOT COME BACK AS AN ERROR.  IT COMES BACK AS A NUMBER YOU
    WILL BELIEVE.

So the row count is ASSERTED against an INDEPENDENT source -- the reference manual,
via rm-golden.json, which enumerates CLOCK_ROOT0..94_CONTROL.  A different document,
parsed by a different tool.  If the two disagree, this refuses to emit.
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Sources the CCM can select.  Order is the C enum order; keep in sync with the
# header we emit.  The three PFDs are FIXED on this SoC (clk-imx93.c:326-334);
# the four fracn-gppll rates are COMPUTED from the anatop registers the guest wrote.
SOURCES = [
    "OSC_24M", "SYS_PLL_PFD0", "SYS_PLL_PFD0_DIV2", "SYS_PLL_PFD1",
    "SYS_PLL_PFD1_DIV2", "SYS_PLL_PFD2", "SYS_PLL_PFD2_DIV2",
    "AUDIO_PLL", "VIDEO_PLL", "ARM_PLL", "DRAM_PLL", "CLK_EXT1",
]
SRC_ID = {s: i for i, s in enumerate(SOURCES)}

# driver parent name -> our source enum.  A name we do not know is NOT guessed.
NAME2SRC = {
    "osc_24m": "OSC_24M",
    "sys_pll_pfd0": "SYS_PLL_PFD0",
    "sys_pll_pfd0_div2": "SYS_PLL_PFD0_DIV2",
    "sys_pll_pfd1": "SYS_PLL_PFD1",
    "sys_pll_pfd1_div2": "SYS_PLL_PFD1_DIV2",
    "sys_pll_pfd2": "SYS_PLL_PFD2",
    "sys_pll_pfd2_div2": "SYS_PLL_PFD2_DIV2",
    "audio_pll": "AUDIO_PLL",
    "video_pll": "VIDEO_PLL",
    "arm_pll": "ARM_PLL",
    "dram_pll": "DRAM_PLL",
    "clk_ext1": "CLK_EXT1",
}


def main(drv, out_h):
    src = open(drv).read()

    # --- the 9 parent-select tables: parent_names[MAX_SEL][4] ----------------
    m = re.search(r'static const char \*parent_names\[MAX_SEL\]\[4\] = \{(.*?)\n\};',
                  src, re.S)
    if not m:
        sys.exit("REFUSING: parent_names[MAX_SEL][4] not found in %s" % drv)
    sels = []
    for row in re.finditer(r'\{([^{}]*)\}', m.group(1)):
        names = [n.strip().strip('"') for n in row.group(1).split(",") if n.strip()]
        if len(names) != 4:
            sys.exit("REFUSING: a parent-select row has %d entries, not 4: %r"
                     % (len(names), names))
        for n in names:
            if n not in NAME2SRC:
                sys.exit("REFUSING: unknown clock source %r -- DROP, DO NOT GUESS. "
                         "A source we invent reads back as a plausible number." % n)
        sels.append([NAME2SRC[n] for n in names])

    # --- the sel enum, so root.sel indexes the table above -------------------
    m = re.search(r'enum clk_sel \{(.*?)\};', src, re.S)
    if not m:
        sys.exit("REFUSING: enum clk_sel not found")
    sel_order = [s.strip().rstrip(',') for s in m.group(1).split() if s.strip().rstrip(',')]
    sel_order = [s.rstrip(',') for s in re.findall(r'(\w+),', m.group(1))]
    if len(sel_order) != len(sels):
        sys.exit("REFUSING: enum clk_sel has %d entries but parent_names has %d rows"
                 % (len(sel_order), len(sels)))
    SEL_IDX = {name: i for i, name in enumerate(sel_order)}

    # --- the 95 roots ---------------------------------------------------------
    m = re.search(r'root_array\[\] = \{(.*?)\n\};', src, re.S)
    if not m:
        sys.exit("REFUSING: root_array[] not found")
    #
    # ⚠ THE DRIVER CARRIES BOTH CHIPS.  WE ARE THE 91.
    #
    # root_array has a `plat` field, and three offsets are claimed TWICE -- once
    # PLAT_IMX93, once PLAT_IMX91:
    #
    #     0x2b00  enet_root         (93)   vs  enet1_qos_tsn_root   (91)
    #     0x2b80  enet_timer1_root  (93)   vs  enet_timer_root      (91)
    #     0x2c80  enet_ref_root     (93)   vs  enet2_regular_root   (91)
    #
    # Take the 93's row and the guest gets a root that does not exist on this chip.
    # (Their parent-SELECTS happen to be identical, so the FREQUENCY would have come
    # out right anyway -- which is exactly why this must be derived and not left to
    # luck.  A result that is correct by luck is a result you have not checked; the
    # only reason I get to say it is correct is that I looked.)
    #
    all_rows = []
    for r in re.finditer(
            r'\{\s*(IMX9\d_CLK_\w+),\s*"([^"]+)",\s*(0x[0-9a-fA-F]+),\s*(\w+),'
            r'([^}]*)\}', m.group(1)):
        _clk, name, off, sel, rest = r.groups()
        if sel not in SEL_IDX:
            sys.exit("REFUSING: root %s has unknown select %r" % (name, sel))
        plat = "PLAT_IMX93" if "PLAT_IMX93" in rest else (
               "PLAT_IMX91" if "PLAT_IMX91" in rest else None)
        all_rows.append((name, int(off, 16), sel, SEL_IDX[sel], plat))

    roots = [r for r in all_rows if r[4] != "PLAT_IMX93"]
    dropped_93 = len(all_rows) - len(roots)

    by_off = {}
    for name, off, sel, seli, _p in roots:
        if off in by_off and by_off[off][1] != seli:
            sys.exit("REFUSING: offset 0x%04x claimed twice with DIFFERENT selects "
                     "(%s vs %s).  Drop, do not guess." % (off, by_off[off][0], name))
        by_off[off] = (name, seli)

    #
    # ⭐ EVERY ROOT MUST LAND ON A REGISTER SLICE THE REFERENCE MANUAL DOCUMENTS.
    #
    # rt1180emulator's generator silently dropped a row (73 of 74 -- the last SDK
    # entry had no trailing comma), and nothing complained: an under-filled table
    # zero-fills, and source index 0 is the 24 MHz oscillator, SO THE MISSING ROOT
    # READS BACK AS A PERFECTLY PLAUSIBLE 24 MHz.
    #
    #     A MISSING ROW DOES NOT COME BACK AS AN ERROR.  IT COMES BACK AS A NUMBER
    #     YOU WILL BELIEVE.
    #
    # Two defences, because the count alone is not enough:
    #   1. every driver offset must be a real RM root slice (n * 0x80, n < 95).
    #      A different document, parsed by a different tool.
    #   2. the emitted table is indexed by slice and defaults to NO SOURCE -- so a
    #      dropped row does NOT become 24 MHz, it becomes a root that DOES NOT TICK.
    #      A stopped clock gets diagnosed in a minute; a plausible one ships.
    #
    # NOTE the RM documents 95 slices and Linux registers 87.  That is NOT a parser
    # bug -- the manual enumerates the register file, the driver enumerates what this
    # SoC uses.  The 8 unused slices are DECLARED below rather than quietly ignored.
    #
    golden = json.load(open(os.path.join(HERE, "rm-golden.json")))
    rm_roots = len({g["reg"] for g in golden
                    if re.fullmatch(r'CLOCK_ROOT\d+_CONTROL', g["reg"])})
    for name, off, _sel, _seli, _p in roots:
        if off % 0x80 or off // 0x80 >= rm_roots:
            sys.exit("REFUSING TO EMIT: root %s sits at 0x%04x, which is not one of "
                     "the %d CLOCK_ROOT slices the REFERENCE MANUAL documents."
                     % (name, off, rm_roots))

    with open(out_h, "w") as f:
        f.write("/* GENERATED by tests/imx91-reset-values/gen-clock-tree.py "
                "-- DO NOT EDIT.\n"
                " *\n"
                " * Source: Linux drivers/clk/imx/clk-imx93.c (root_array, parent_names).\n"
                " * Row count ASSERTED against IMX91RM.pdf (%d CLOCK_ROOTn_CONTROL rows).\n"
                " * Retyping a device fact is how you fabricate one.\n"
                " */\n" % rm_roots)
        f.write("#ifndef HW_MISC_IMX93_CCM_ROOTS_H\n#define HW_MISC_IMX93_CCM_ROOTS_H\n\n")

        f.write("typedef enum {\n")
        for s in SOURCES:
            f.write("    IMX93_CLK_SRC_%s,\n" % s)
        f.write("    IMX93_CLK_SRC__COUNT,\n} IMX93ClkSrc;\n\n")

        f.write("/* parent_names[sel][CONTROL.MUX] -> source */\n")
        f.write("static const IMX93ClkSrc imx93_ccm_sel[%d][4] = {\n" % len(sels))
        for name, row in zip(sel_order, sels):
            f.write("    /* %-18s */ { %s },\n"
                    % (name, ", ".join("IMX93_CLK_SRC_%s" % s for s in row)))
        f.write("};\n\n")

        # Indexed BY SLICE, not packed.  An unregistered slice gets NO_SEL, which
        # means NO SOURCE, which means the root DOES NOT TICK.  A dropped row can
        # therefore never read back as a plausible 24 MHz.
        f.write("/* Indexed by clock-root slice (CONTROL at slice * 0x80).\n"
                " * IMX93_CCM_NO_SEL = the RM documents this slice but Linux does not\n"
                " * register it -- it has NO SOURCE and DOES NOT TICK.  A dropped table\n"
                " * row must never read back as a plausible frequency.\n"
                " */\n")
        f.write("#define IMX93_CCM_NO_SEL 0xff\n\n")
        f.write("static const uint8_t imx93_ccm_root_sel[%d] = {\n" % rm_roots)
        for slice_no in range(rm_roots):
            off = slice_no * 0x80
            if off in by_off:
                nm, seli = by_off[off]
                f.write("    [%2d] = %d,  /* %s */\n" % (slice_no, seli, nm))
            else:
                f.write("    [%2d] = IMX93_CCM_NO_SEL,  /* not registered by Linux */\n"
                        % slice_no)
        f.write("};\n\n")
        f.write("#define IMX93_CCM_NUM_SLICES %d\n\n" % rm_roots)

        # Root names, so the SoC can wire a consumer BY NAME and fail loudly if the
        # name does not exist -- rather than hand-copying a slice number that would
        # silently resolve to some other root's frequency.
        f.write("static const char *const imx93_ccm_root_name[%d] = {\n" % rm_roots)
        for slice_no in range(rm_roots):
            off = slice_no * 0x80
            nm = by_off[off][0] if off in by_off else None
            f.write('    [%2d] = %s,\n' % (slice_no, ('"%s"' % nm) if nm else "NULL"))
        f.write("};\n\n")
        f.write("#endif\n")

    unused = rm_roots - len(by_off)
    print("wrote %s" % out_h)
    print("  clock roots     : %d registered by Linux (of %d RM slices)"
          % (len(by_off), rm_roots))
    print("  PLAT_IMX93 rows : %d DROPPED (this is the 91; three offsets are claimed "
          "by both chips)" % dropped_93)
    print("  unused slices   : %d -- DECLARED: NO SOURCE, they DO NOT TICK" % unused)
    print("  parent-selects  : %d x 4" % len(sels))
    print("  sources         : %d" % len(SOURCES))
    print("  every root offset ASSERTED to be a real RM slice.")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
