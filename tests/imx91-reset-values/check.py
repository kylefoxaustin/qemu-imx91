#!/usr/bin/env python3
"""Differential: read EVERY register at reset, diff against the REFERENCE MANUAL.

WHY THIS EXISTS.  Most of this model's peripherals reset with

    memset(s->regs, 0, sizeof(s->regs));

which feels like a safe, neutral default.  It is not neutral.

    ⭐ A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM.
                                                            -- mcxn947qemu

And the guest believes it.  On the very first three registers this gate ever
probed on the i.MX 91 it found one: eDMA CH0_SBR resets to 0000_8007h on
silicon and to ZERO in our model, and Linux's fsl-edma driver read-modify-writes
that register.

THE GOLDEN IS THE REFERENCE MANUAL (rm-golden.json, extracted from IMX91RM.pdf
rev 5 by extract-rm-golden.py).  So this gate CANNOT BE SATISFIED BY THE MODEL
AGREEING WITH ITSELF:

    A test that gets its expectations from the model is not a test, it is a
    MIRROR -- and mutation testing cannot see it, because mutating the model
    moves the mirror too.

That is not a hypothetical for this fleet.  rt1180emulator's eDMA channel
registers sat at MCXN947 geometry -- a chip he does not model -- with every
eDMA test green and ALL SIXTEEN MUTATIONS "CAUGHT", because the tests took
their addresses from the model.  THIS is the oracle nobody here authored.

ABOUT THE ALLOWLIST, which is the dangerous part of this file:

    "An independent whitelist doesn't merely MISS the bug -- IT CERTIFIES IT."
                                                       -- ollama_95_neutron

So it is built to SHRINK, and it fights back in BOTH directions:

  * a mismatch NOT in the allowlist       -> FAIL (a new lie)
  * an allowlisted entry that now MATCHES -> FAIL ("this is fixed; delete it")
  * the remaining count is PRINTED LOUDLY every run.  A gate that quietly
    tolerates 400 known-wrong registers reads as "covered" when it is not.

⚠ THE RM's RESET COLUMN IS THE COLD-POR VALUE.  It is not what firmware sees on
  a board with a boot ROM.  We boot -kernel and skip both the ROM and U-Boot, so
  cold-POR is closer to right for us than for most -- but where a deviation is
  DELIBERATE it is allowlisted WITH A REASON.  A deviation with a reason is a
  decision; a deviation without one is a bug you have agreed not to look at.
                                                       -- rt1180emulator
"""
import json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.environ.get("QEMU", os.path.join(HERE, "..", "..", "build",
                                           "qemu-system-aarch64"))
MACHINE = "imx91-11x11-evk"

if not os.access(QEMU, os.X_OK):
    print("SKIP: qemu not built at %s" % QEMU)
    sys.exit(0)

golden = json.load(open(os.path.join(HERE, "rm-golden.json")))

#
# ⭐ COVERAGE IS AN ASSERTION, NOT A PRINT STATEMENT.
#
#     A GATE WHOSE COVERAGE CANNOT FAIL IT IS A GATE YOU HAVE AGREED NOT TO LOOK AT.
#                                                       -- rt1180emulator, who lived it
#
# His extractor printed "unmatched: 4371" every run, for a day, while returning
# PASS -- and the blind set contained the two blocks he most needed it to see.  He
# read the number and it changed nothing, BECAUSE IT WAS PRINTED, NOT ASSERTED.
# An honest number with no threshold is the same organ as an allowlist that never
# shrinks.
#
# The count lives OUTSIDE the artifact (expected-coverage.txt) because the suite
# and the artifact must not share a source -- AND THE COUNT IS PART OF THE
# ARTIFACT.  It ratchets in BOTH directions: coverage DOWN means the gate went
# partially blind; coverage UP is good news that must be DECLARED, not absorbed.
#
EXPECTED = int(open(os.path.join(HERE, "expected-coverage.txt")).read().strip())

if len(golden) != EXPECTED:
    direction = "SHRANK" if len(golden) < EXPECTED else "GREW"
    print("FAIL: COVERAGE %s -- the golden holds %d registers, expected-coverage.txt "
          "says %d." % (direction, len(golden), EXPECTED))
    if len(golden) < EXPECTED:
        print("      THE GATE HAS GONE PARTIALLY BLIND.  Every register it can no")
        print("      longer see is UNCHECKED, and this run would otherwise say PASS.")
    else:
        print("      The gate can see MORE than it was told to.  Good news -- but it")
        print("      must be DECLARED, not absorbed: update expected-coverage.txt so")
        print("      the next regression has something to fail against.")
    sys.exit(2)

allow = {}
with open(os.path.join(HERE, "known-deviations.txt")) as f:
    for line in f:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        inst, reg, reason = (line.split(None, 2) + [""])[:3]
        allow[(inst, reg)] = reason


def probe(regs):
    """Ask, read exactly as many answers as questions, then kill it.

    ⭐ `timeout -s KILL` -- THE GATE CALLS THE SUBJECT, SO THE SUBJECT CAN WEDGE
       THE GATE.

    This harness blocks in p.stdout.readline().  A QEMU that hangs -- and a wedged
    device model is EXACTLY the class of bug this gate exists to find -- parks the
    checker forever, and it looks BUSY, NOT BROKEN.

        "NO VERDICT" AND "STILL WORKING" ARE THE SAME OBSERVATION.  A refusal is a
        verdict; a hang is an ABSENCE.  Fail-safe assumes the gate RETURNS.
                                                  -- ollama_95_neutron, measured

    So the subject runs out-of-process under a HARD KILL (SIGTERM does not free a
    thread stuck in a driver), and the answer-count assertion below turns the kill
    into a FAILED VERDICT rather than a silent short read.

    Also: `-qtest stdio` WITHOUT `-accel qtest` still runs a TCG vCPU, which boots
    from a zeroed vector table and free-runs -- a SECOND WRITER to the very address
    space we are reading.  Halt it.
    """
    p = subprocess.Popen(
        ["timeout", "-s", "KILL", "180",
         QEMU, "-M", MACHINE, "-display", "none", "-accel", "qtest",
         "-qtest", "stdio", "-monitor", "none", "-serial", "none"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True)
    try:
        p.stdin.write("".join("readl 0x%x\n" % r["addr"] for r in regs))
        p.stdin.flush()
        vals = []
        while len(vals) < len(regs):
            line = p.stdout.readline()
            if not line:
                break
            if line.startswith("OK 0x"):
                vals.append(int(line.split()[1], 16) & 0xffffffff)
        return vals
    finally:
        p.kill()
        p.wait()


vals = probe(golden)
if len(vals) != len(golden):
    print("FAIL: asked %d questions, got %d answers.  A truncated conversation and "
          "a correct one differ only in the answers you never notice are missing."
          % (len(golden), len(vals)))
    sys.exit(1)

mismatched = {(r["inst"], r["reg"]): (r, v)
              for r, v in zip(golden, vals) if v != r["reset"]}

new = [k for k in mismatched if k not in allow]
# An allowlisted register that now agrees with the RM has been FIXED.  Say so, and
# FAIL, so the line gets deleted.  This is what stops the list from becoming a
# permanent certificate for N wrong answers.
stale = [k for k in allow if k not in mismatched]

print("probed %d registers against the RM (golden = IMX91RM.pdf rev 5)  [asserted]"
      % len(golden))
print("  matching        : %d" % (len(golden) - len(mismatched)))
print("  known deviations: %d   <-- THIS NUMBER MUST GO DOWN"
      % (len(mismatched) - len(new)))

rc = 0
if new:
    print("\nFAIL: %d register(s) disagree with the RM and are NOT in the allowlist."
          % len(new))
    print("      The guest reads a value the silicon would never produce.")
    for inst, reg in sorted(new)[:30]:
        r, v = mismatched[(inst, reg)]
        print("        %-14s %-22s @0x%08x  model=0x%08x  RM=0x%08x"
              % (inst, reg, r["addr"], v, r["reset"]))
    if len(new) > 30:
        print("        ... and %d more" % (len(new) - 30))
    rc = 1

if stale:
    print("\nFAIL: %d allowlisted register(s) now MATCH the RM -- they are FIXED."
          % len(stale))
    print("      Delete them from known-deviations.txt.  An allowlist that never")
    print("      shrinks stops being a to-do list and becomes a CERTIFICATE.")
    for inst, reg in sorted(stale)[:30]:
        print("        %-14s %s" % (inst, reg))
    rc = 1

if rc == 0:
    print("\nPASS: no new reset-value lies; %d known deviations, all still known."
          % len(mismatched))
sys.exit(rc)
