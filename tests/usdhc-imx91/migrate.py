#!/usr/bin/env python3
"""uSDHC VEND_SPEC must survive a migration -- INCLUDING when the guest has written ZERO.

⭐ A VMSTATE `needed` PREDICATE THAT ASKS "IS THE VALUE INTERESTING" WILL DROP THE FIELD
   PRECISELY WHEN THE GUEST HAS WRITTEN THE ONE VALUE IT MISTAKES FOR "ABSENT".

I added vendor_spec to the SDHCI VMState as a subsection (it was migrated NOWHERE, despite
driving PRNSTS[CLOCK_GATE_OFF]) and guarded it with:

    return s->vendor_spec != 0;          /* "only send it if there's something in it" */

which reads as common sense and is a data-loss bug.  On load, an ABSENT subsection leaves
the field holding whatever sdhci_reset() put there -- so a guest that deliberately wrote 0
came back holding 0x30007809.  Its own state, silently replaced by the reset value:

    guest writes 0x00000000  ->  migrate  ->  reads back 0x30007809

THIS IS THE DANGEROUS-ZEROS BUG, ONE LAYER OUT.  The whole reset-value audit in this tree
rests on "a zero is not the absence of a claim, it is a claim" -- and then I wrote a
migration predicate that treats zero as absence.

93emulator shipped the same sdhci fix and keyed on `vendor_spec_reset != 0` ("did the
platform opt in").  That fixes my bug -- but it never emits the subsection for i.MX6/7,
whose vendor_spec_reset is 0 by default and who nonetheless WRITE this register at runtime
(FRC_SDCLK_ON), so their value would still be lost.

The correct question is neither "is it interesting" nor "did the platform opt in".  On load,
an omitted subsection leaves the field holding exactly what reset wrote.  Therefore:

    ⭐ A SUBSECTION MAY BE OMITTED IFF THE FIELD ALREADY HOLDS WHAT RESET WOULD PUT THERE.

        needed  <=>  vendor_spec != vendor_spec_reset

...which fixes i.MX6/7 too, and leaves the generic-SDHCI wire format byte-for-byte unchanged.
This test asserts BOTH halves: every value survives a real migrate, AND the state file is
SMALLER when the field is untouched -- i.e. the subsection is genuinely omitted, not merely
harmless.  (A test that only checked the values would pass on a predicate that always
returns true, which would silently change the wire format for every existing SDHCI guest.)
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "..", "..", "build", "qemu-system-aarch64"))
VEND_SPEC = 0x428500C0          # uSDHC1
RESET = 0x30007809


def qt(f, cmd):
    f.write((cmd + "\n").encode())
    f.flush()
    while True:
        line = f.readline().decode()
        if not line:
            raise RuntimeError("qtest died")
        if line.startswith(("OK", "FAIL")):
            return line.strip()


def start(sock, extra, machine="imx91-11x11-evk"):
    if os.path.exists(sock):
        os.unlink(sock)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock)
    srv.listen(1)
    p = subprocess.Popen(
        [QEMU, "-M", machine, "-display", "none", "-accel", "qtest",
         "-qtest", "unix:" + sock, "-monitor", "stdio", "-serial", "none",
         "-audio", "driver=none"] + extra,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    conn, _ = srv.accept()
    srv.close()
    os.unlink(sock)
    return p, conn.makefile("rwb")


def roundtrip(tmp, write, machine="imx91-11x11-evk", base=VEND_SPEC, want_reset=None):
    """want_reset: assert the device really came up with this reset value BEFORE writing.

    ⭐ A TEST CASE THAT DOES NOT VERIFY ITS OWN SETUP IS TESTING SOMETHING ELSE.

    My first attempt at the i.MX6/7 case used
        -global driver=imx-usdhc,property=vendor-spec-reset,value=0
    on the i.MX 91.  It DID NOTHING -- fsl-imx91.c calls qdev_prop_set_uint32() in board
    realize, which OVERRIDES -global.  So the case ran with reset=0x30007809 and passed
    VACUOUSLY.  I only found out because the MUTATION RUN passed too, when it had to fail.
    (The same shape as the -global short-form trap that ate the TMU temperature sweep: the
    flag was accepted, silently did nothing, and the test was green about nothing.)
    """
    sock, state = os.path.join(tmp, "q.sock"), os.path.join(tmp, "vm.state")
    if os.path.exists(state):
        os.unlink(state)

    p, f = start(sock, [], machine)
    reset_seen = int(qt(f, "readl 0x%x" % base).split()[1], 16)
    if want_reset is not None and reset_seen != want_reset:
        p.kill(); p.wait()
        raise RuntimeError("SETUP FAILED: %s VEND_SPEC reset is 0x%08x, wanted 0x%08x "
                           "-- this case would have tested nothing"
                           % (machine, reset_seen, want_reset))
    if write is not None:
        qt(f, "writel 0x%x 0x%x" % (base, write))
    before = int(qt(f, "readl 0x%x" % base).split()[1], 16)
    p.stdin.write(('migrate "exec:cat > %s"\n' % state).encode())
    p.stdin.flush()
    time.sleep(3)
    p.kill()
    p.wait()

    p2, f2 = start(sock, ["-incoming", "exec:cat %s" % state], machine)
    time.sleep(2)
    after = int(qt(f2, "readl 0x%x" % base).split()[1], 16)
    p2.kill()
    p2.wait()
    return before, after, os.path.getsize(state)


def main():
    if not os.path.exists(QEMU):
        print("SKIP: %s not found" % QEMU)
        return 0

    #
    # ⭐ THE CASE NEITHER TREE WAS TESTING.
    #
    # 93emulator and I both wrote this migrate test, on different trees, and BOTH of us
    # only exercised a machine where vendor_spec_reset != 0.  That blinds both tests to
    # the exact bug that distinguishes the two WRONG predicates from the right one:
    #
    #   a platform that does NOT set vendor-spec-reset (it defaults to 0) but whose driver
    #   WRITES the register at runtime -- i.e. every i.MX6/7 board using TYPE_IMX_USDHC,
    #   which writes FRC_SDCLK_ON via esdhc_write().
    #
    # Under `needed = vendor_spec_reset != 0` that is FALSE forever, the subsection is
    # never emitted, and the guest's value DIES AT THE MIGRATION BOUNDARY.  Under
    # `needed = vendor_spec != vendor_spec_reset` it survives.
    #
    # -global lets us be an i.MX6/7 for the length of one test.  A test that only ever
    # runs the machine it was written for cannot see a bug that lives in the default.
    #
    IMX8MP_VEND_SPEC = 0x30B400C0     # fsl-imx8mp.c: uSDHC1 @ 0x30b40000
    cases = [
        # name, write, machine, base, want_reset
        ("untouched (== the reset value)", None,
         "imx91-11x11-evk", VEND_SPEC, RESET),
        ("guest writes 0x00000000  <- THE DANGEROUS ZERO", 0,
         "imx91-11x11-evk", VEND_SPEC, RESET),
        ("guest writes 0x30007b09", 0x30007B09,
         "imx91-11x11-evk", VEND_SPEC, RESET),
        ("guest writes 0x00000100", 0x100,
         "imx91-11x11-evk", VEND_SPEC, RESET),
        # ⭐ THE CASE NEITHER TREE WAS TESTING.  A REAL machine that never sets
        # vendor-spec-reset (so it defaults to 0) but whose driver WRITES the register
        # at runtime -- every i.MX6/7/8M board on TYPE_IMX_USDHC (FRC_SDCLK_ON).
        # Under `needed = vendor_spec_reset != 0` this is FALSE forever, the subsection
        # is never emitted, and the guest's value DIES AT THE MIGRATION BOUNDARY.
        ("i.MX8MP (reset=0, as i.MX6/7): writes 0x00000100", 0x100,
         "imx8mp-evk", IMX8MP_VEND_SPEC, 0x0),
    ]
    fail = 0
    sizes = {}
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        for name, w, mach, base, wr in cases:
            before, after, size = roundtrip(tmp, w, mach, base, wr)
            sizes[name] = size
            ok = before == after
            print("  %s  %-48s before=0x%08x after=0x%08x" %
                  ("ok  " if ok else "FAIL", name, before, after))
            if not ok:
                print("        the guest's own state was replaced by the RESET value.")
                fail = 1

    # The subsection must be OMITTED when the field holds the reset value -- otherwise we
    # have silently changed the wire format for every existing SDHCI guest.
    untouched = sizes["untouched (== the reset value)"]
    written = sizes["guest writes 0x00000000  <- THE DANGEROUS ZERO"]
    print()
    print("  state size: untouched=%d  written=%d  (delta %d = the subsection)"
          % (untouched, written, written - untouched))
    if untouched >= written:
        print("  FAIL: the subsection is emitted even when the field == its reset value,")
        print("        i.e. unconditionally on every platform that OPTS IN. Wasteful, and")
        print("        it means the predicate is not asking the right question -- but note")
        print("        it does NOT break pre-existing guests: they leave vendor-spec-reset")
        print("        at its default of 0, so no subsection is emitted for them either way.")
        print("        (Their bug is the OPPOSITE one, and the i.MX8MP case above is the")
        print("        only thing in this file that can see it.)")
        fail = 1
    else:
        print("  ok    omitted when it equals reset -> the generic SDHCI wire format is")
        print("        byte-for-byte unchanged.")

    print()
    print("PASS: VEND_SPEC survives migration for every value INCLUDING zero, and the"
          if not fail else "FAIL: see above.")
    if not fail:
        print("      subsection is omitted exactly when reset would reconstruct it.")
    return fail


if __name__ == "__main__":
    sys.exit(main())
