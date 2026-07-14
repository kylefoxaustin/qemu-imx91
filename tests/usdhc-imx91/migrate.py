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


def start(sock, extra):
    if os.path.exists(sock):
        os.unlink(sock)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(sock)
    srv.listen(1)
    p = subprocess.Popen(
        [QEMU, "-M", "imx91-11x11-evk", "-display", "none", "-accel", "qtest",
         "-qtest", "unix:" + sock, "-monitor", "stdio", "-serial", "none",
         "-audio", "driver=none"] + extra,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    conn, _ = srv.accept()
    srv.close()
    os.unlink(sock)
    return p, conn.makefile("rwb")


def roundtrip(tmp, write):
    sock, state = os.path.join(tmp, "q.sock"), os.path.join(tmp, "vm.state")
    if os.path.exists(state):
        os.unlink(state)

    p, f = start(sock, [])
    if write is not None:
        qt(f, "writel 0x%x 0x%x" % (VEND_SPEC, write))
    before = int(qt(f, "readl 0x%x" % VEND_SPEC).split()[1], 16)
    p.stdin.write(('migrate "exec:cat > %s"\n' % state).encode())
    p.stdin.flush()
    time.sleep(3)
    p.kill()
    p.wait()

    p2, f2 = start(sock, ["-incoming", "exec:cat %s" % state])
    time.sleep(2)
    after = int(qt(f2, "readl 0x%x" % VEND_SPEC).split()[1], 16)
    p2.kill()
    p2.wait()
    return before, after, os.path.getsize(state)


def main():
    if not os.path.exists(QEMU):
        print("SKIP: %s not found" % QEMU)
        return 0

    cases = [
        ("untouched (== the reset value)",                     None),
        ("guest writes 0x00000000  <- THE DANGEROUS ZERO",     0),
        ("guest writes 0x30007b09",                            0x30007B09),
        ("guest writes 0x00000100  (i.MX6/7's FRC_SDCLK_ON)",  0x100),
    ]
    fail = 0
    sizes = {}
    with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
        for name, w in cases:
            before, after, size = roundtrip(tmp, w)
            sizes[name] = size
            ok = before == after
            print("  %s  %-46s before=0x%08x after=0x%08x" %
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
        print("  FAIL: the subsection is emitted even when the field == its reset value.")
        print("        That changes the migration wire format for EVERY existing SDHCI")
        print("        guest, including boards none of us own.")
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
