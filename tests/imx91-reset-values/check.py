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
import collections, json, os, subprocess, sys, threading

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
# ...AND IT IS ASSERTED PER PERIPHERAL, NOT JUST IN TOTAL.
#
# A TOTAL CANNOT SEE A MISSING BLOCK, AND A PRESENCE CHECK CANNOT SEE A HALF-EMPTY
# ONE.  Both of those were live in this gate within an hour of each other:
#
#   * The entire PINMUX (IOMUXC1, 256 registers) was invisible -- their NAME cell
#     wraps to a continuation line -- and the total said 6388 and PASSED.  When it
#     was recovered it turned out to be hiding 174 real lies.
#   * Then the fix was negative-tested by reverting it, and the block-PRESENCE gate
#     still said "127/127, none blind" -- because IOMUXC1 keeps the few rows whose
#     names happen to fit on one line.  A block with 5 registers and a block with
#     256 are indistinguishable to "is it there?".
#
#     ⭐ EVERY COVERAGE CONTROL IS ITSELF A NUMBER THAT NEEDS AN EXPECTED VALUE.
#        Assert the SHAPE of the coverage, not just its size.
#
# So expected-coverage.txt holds a per-instance manifest, OUTSIDE the golden, and
# it ratchets in both directions -- shrink means we went blind, growth is good news
# that must be DECLARED so the next regression has something to fail against.
EXPECTED = {}
TOTAL = None
for line in open(os.path.join(HERE, "expected-coverage.txt")):
    line = line.split("#", 1)[0].strip()
    if not line:
        continue
    k, v = line.rsplit(None, 1)
    if k == "TOTAL":
        TOTAL = int(v)
    else:
        EXPECTED[k] = int(v)

actual = collections.Counter(g["inst"] for g in golden)
drift = []
for inst in sorted(set(EXPECTED) | set(actual)):
    want, have = EXPECTED.get(inst, 0), actual.get(inst, 0)
    if want != have:
        drift.append((inst, want, have))

if len(golden) != TOTAL or drift:
    print("FAIL: COVERAGE DRIFT -- the golden holds %d registers, expected-coverage.txt "
          "says %d." % (len(golden), TOTAL))
    print("      A total cannot see a missing block, and a presence check cannot see")
    print("      a half-empty one.  Every register the gate can no longer see is")
    print("      UNCHECKED, and this run would otherwise have said PASS.")
    for inst, want, have in drift[:20]:
        verdict = "WENT BLIND" if have < want else "sees more -- DECLARE it"
        print("        %-26s expected %5d  got %5d   <-- %s"
              % (inst, want, have, verdict))
    if len(drift) > 20:
        print("        ... and %d more instances" % (len(drift) - 20))
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

    #
    # ⭐ ASK AND LISTEN AT THE SAME TIME.  A HARNESS THAT WRITES EVERY QUESTION
    #    BEFORE READING ANY ANSWER DEADLOCKS ON ITS OWN PIPE -- AND ONLY ONCE
    #    COVERAGE GROWS PAST THE BUFFER.
    #
    # This used to write all the questions, then read all the answers.  It worked
    # at 6388 registers and DEADLOCKED at 9282: ~190 KB of questions overflows the
    # 64 KB pipe, so we block writing while QEMU blocks writing answers nobody is
    # draining.  The kill-timeout then fired and the write died with EPIPE -- which
    # surfaced as a TRACEBACK, not a verdict.
    #
    # Two lessons, both the fleet's, both earned here:
    #   * the harness assumed every measurement returns (ollama_95_neutron); and
    #   * the failure was LATENT IN COVERAGE -- the gate got better at seeing the
    #     chip and that is what broke it.  A harness must scale with its own floor.
    #
    #
    # ⭐ READ EACH REGISTER AT ITS OWN WIDTH.
    #
    # This used to readl() everything.  An 8-bit register read 32 bits wide pulls in
    # its three neighbours; a 16-bit register on an odd halfword is a MISALIGNED
    # read.  That does not lose a register -- IT INVENTS A COMPARISON, and a false
    # MATCH is a lie you will never see.  (rt1180emulator kept only the 32-bit rows
    # and went blind to their entire motor drive; I kept them all and read them
    # wrong, which is the same hole with better manners.)
    #
    OP = {8: "readb", 16: "readw", 32: "readl", 64: "readq"}

    def ask():
        try:
            p.stdin.write("".join("%s 0x%x\n" % (OP[r.get("width", 32)], r["addr"])
                                  for r in regs))
            p.stdin.flush()
            p.stdin.close()
        except (BrokenPipeError, ValueError):
            pass                      # subject died; the answer count will say so

    writer = threading.Thread(target=ask, daemon=True)
    writer.start()
    try:
        vals = []
        while len(vals) < len(regs):
            line = p.stdout.readline()
            if not line:
                break                 # subject died or was killed: SHORT READ
            if line.startswith("OK 0x"):
                vals.append(int(line.split()[1], 16))
        return vals
    finally:
        p.kill()
        p.wait()
        writer.join(timeout=5)


vals = probe(golden)
if len(vals) != len(golden):
    print("FAIL: asked %d questions, got %d answers.  A truncated conversation and "
          "a correct one differ only in the answers you never notice are missing."
          % (len(golden), len(vals)))
    sys.exit(1)

def mask(r):
    return (1 << r.get("width", 32)) - 1

mismatched = {(r["inst"], r["reg"]): (r, v)
              for r, v in zip(golden, vals)
              if (v & mask(r)) != (r["reset"] & mask(r))}

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
