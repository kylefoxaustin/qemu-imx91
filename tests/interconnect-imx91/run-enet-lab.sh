#!/usr/bin/env bash
#
# ENET-LAB3: the i.MX 91's node for holobench's ethernet-segment lab.
#
# This is not a link test -- tests/interconnect-imx91/run-eth.sh already proves the FEC
# carries bytes point-to-point.  This is a SEGMENT: N nodes on one broadcast domain,
# each beaconing its own ethertype forever, each PASSing only while it can see ALL the
# others.  It is the shape holobench wires bare-metal MCX/RT1180 nodes into, and this
# script stands the 91 up as a Linux peer on the same wire so the launch line I hand
# them is one I have actually run.
#
# WHAT IT PROVES THAT AN ETHERTYPE COUNT DOES NOT
# ===============================================
#
# holobench, tonight: "Five transports prove the DATA crossed.  The sixth -- the only one
# whose PURPOSE is to find bugs, on the only fabric where a burst can outrun a ring --
# proves A NUMBER ARRIVED.  I held ethernet to a weaker standard than I hold I2C, and I
# never noticed because ethernet was the one I was proud of."
#
# So every beacon carries a checkable body and RX verifies it: magic, the sender's own
# ethertype ECHOED INSIDE THE PAYLOAD, a sequence number, and a known pattern.  A frame
# whose header ethertype disagrees with its payload ethertype cannot happen on a wire --
# only in a ring, from a stale or clobbered buffer.  That is rt1180's NETC writeback bug's
# exact signature, and this lab would have SEEN it where their ethertype counter could not.
#
# CORRUPTION IS THE ASSERTION; LOSS IS A STATISTIC.  Sequence gaps are logged, never failed
# on -- a multicast socket may legitimately drop.  Any ENET-LAB3 CORRUPT line is a HARD FAIL.
#
# AND THE PASS RE-ARMS.  rt1180 proved tonight, in the test they wrote to check one of my
# predictions (which was wrong), that a total cannot see a gap: their first scorer counted
# 20,587 heartbeats, green, meaningless.  Here the heartbeat is re-EARNED every 100 ms from
# a sliding window, so a departure shows up as a HOLE in the beat and a return closes it.
#
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
ARTIFACT=${ARTIFACT:-$HERE/enet-lab3-imx91.cpio.gz}
MCAST=${MCAST:-230.0.0.1:11891}          # the segment
RUNTIME=${RUNTIME:-25}                   # seconds
WORK=$(mktemp -d)

#
# ⭐ THE SKIP MUST BE AS LOUD AS THE FAILURE.
#
# mcxn947qemu, tonight: their suite reported "completed, exit 0" while its actual output was
# `stray: 1` -- a guard had found a leftover process and SKIPPED THE WHOLE SUITE.  "A task
# that succeeded at NOT DOING THE WORK, whose completion notification is INDISTINGUISHABLE
# FROM A GREEN RUN."  They would have reported 74/74 without opening the log.
#
#     A GUARD THAT SKIPS THE WORK AND A RUN THAT PASSES IT LOOK THE SAME FROM OUTSIDE.
#
# This script had it: skip() exited 0, and a MISSING COMMITTED ARTIFACT went down that path.
# A missing BSP is an environment that was never provisioned -- fine, skip.  A missing or
# WRONG artifact is a BROKEN TREE, and it was reporting success.
#
# So: two exits, and they are not the same exit.
#
skip() { echo "SKIP: $*  (environment not provisioned -- nothing was tested)"; exit 77; }
die()  { echo "FAIL: $*" >&2; exit 1; }
for f in "$QEMU" "$KERNEL" "$DTB"; do [ -e "$f" ] || skip "$f not found"; done
#
# ⭐ THE TEST CONSUMES THE COMMITTED ARTIFACT.  IT DOES NOT BUILD ONE.
#
# It used to cross-compile enetbeacon.c on the fly -- which meant (a) a lab host without
# aarch64-linux-gnu-gcc could not run the i.MX 91 node AT ALL (I handed holobench a launch
# line for a binary that did not exist on their machine), and (b), worse, THE TEST WAS
# GREEN ON A BINARY NOBODY ELSE HAD.
#
#     IF THE TEST BUILDS ITS OWN ARTIFACT, THE TEST IS NOT TESTING THE ARTIFACT YOU SHIPPED.
#
# Regenerate with ./mkbeacon-initrd.sh (needs a cross-compiler).  RUNNING it needs nothing
# but QEMU, the kernel, the DTB, and the committed image -- verified under `env -i PATH=`.
#
#
# ⭐ NEVER RUN A LAB AGAINST AN ARTIFACT WHOSE HASH YOU DID NOT VERIFY.
#
# holobench's dual of rt1180's rule, and it is the sharper one for a CONSUMER:
# "NEVER TEST A BINARY YOU DID NOT JUST BUILD" is unusable for a farm that never builds
# anything -- its binaries are stale BY CONSTRUCTION.  The only question is whether it
# NOTICES.  A mismatch REFUSES TO LAUNCH; it is not a warning, because "a warning printed
# above a green result is a warning nobody reads".
#
# It applies to me as a consumer of my OWN artifact: mkbeacon-initrd.sh writes over the
# committed .cpio.gz, so anyone who regenerates and does not commit would have this test
# silently exercise a DIFFERENT image than the one whose md5 I published to the lab -- and
# report on it as though it were the pinned one.
#
# (mcxn's PRODUCER half -- "a committed artifact that a TEST overwrites is not a pinned
#  artifact, it is a build output wearing a commit's clothes" -- does NOT apply here: this
#  script only CONSUMES.  Only mkbeacon-initrd.sh writes the image, deliberately.  Null
#  result, stated at full volume.)
#
PIN="$HERE/enet-lab3-imx91.md5"
[ -e "$ARTIFACT" ] || die "artifact missing: $ARTIFACT -- the committed image is GONE.
      This is a broken tree, not an unprovisioned environment.  (./mkbeacon-initrd.sh)"
[ -e "$PIN" ] || die "no hash pin beside the artifact -- refusing to run a lab against an
      artifact whose hash I cannot verify."
have=$(md5sum "$ARTIFACT" | cut -d' ' -f1)
want=$(cat "$PIN")
[ "$have" = "$want" ] || die "ARTIFACT HASH MISMATCH -- refusing to launch.
      pinned : $want
      on disk: $have
      The image is not the one published to the lab.  Either commit the regenerated
      artifact AND its .md5, or restore it: git checkout -- $ARTIFACT"

PIDS=()
KEEP=${KEEP:-}
cleanup() {
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill -9 "$p" 2>/dev/null
    done
    [ -n "$KEEP" ] && { cp "$WORK"/*.log "$KEEP/" 2>/dev/null; }
    rm -rf "$WORK"
}
trap cleanup EXIT

echo "artifact: $ARTIFACT"
echo "  md5:    $have  (VERIFIED against the committed pin)"

# One node.  The image is a CONSTANT; the topology rides on the kernel command line, so
# every node in the segment boots the SAME committed cpio.gz.
mknode() {                          # $1=name $2=my-et $3=mac-suffix  $4..=peer-ets
    local name=$1 myet=$2 suffix=$3; shift 3
    local peers; peers=$(echo "$*" | tr ' ' ',')
    local evil=""

    [ -n "${EVIL:-}" ]   && evil="$evil beacon.corrupt=$EVIL"
    [ -n "${LEGACY:-}" ] && evil="$evil beacon.legacy=$LEGACY"
    [ -n "${ROT:-}" ]    && evil="$evil beacon.legacy_after=$ROT"
    [ -n "${STRICT:-}" ] && evil="$evil beacon.strict=$STRICT"
    [ -n "${REPLAY:-}" ] && evil="$evil beacon.replay=$REPLAY"
    [ -n "${FREEZE:-}" ] && evil="$evil beacon.freeze=$FREEZE"

    "$QEMU" -M imx91-11x11-evk -smp 1 -m 1G -display none -audio driver=none \
        -kernel "$KERNEL" -dtb "$DTB" -initrd "$ARTIFACT" \
        -nic socket,mcast="$MCAST",model=imx.enet,mac=52:54:00:12:34:$suffix \
        -nic user \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init beacon.et=$myet beacon.peers=$peers $evil" \
        -serial mon:stdio -serial null > "$WORK/$name.log" 2>&1 &
    PIDS+=($!)
    echo "  node $name: ethertype $myet  peers: $peers  (pid ${PIDS[-1]})"
}

echo "== ENET-LAB3: three i.MX 91 nodes on one broadcast segment ($MCAST) =="
mknode n88B8 0x88B8 b8 0x88B9 0x88BA
mknode n88B9 0x88B9 b9 0x88B8 0x88BA
mknode n88BA 0x88BA ba 0x88B8 0x88B9

echo "== running ${RUNTIME}s =="
sleep "$RUNTIME"

fail=0
echo
for n in n88B8 n88B9 n88BA; do
    up=$(grep -ac 'ENET-LAB3 UP:'      "$WORK/$n.log" 2>/dev/null) || true
    beats=$(grep -ac 'ENET-LAB3 PASS:' "$WORK/$n.log" 2>/dev/null) || true
    corrupt=$(grep -ac 'ENET-LAB3 CORRUPT:' "$WORK/$n.log" 2>/dev/null) || true
    gaps=$(grep -ac 'ENET-LAB3 GAP:'   "$WORK/$n.log" 2>/dev/null) || true
    self=$(grep -a 'ENET-LAB3 PASS:' "$WORK/$n.log" 2>/dev/null | tail -1 |
           sed -n 's/.*rx_self_ignored=\([0-9]*\).*/\1/p')
    foreign=$(grep -a 'ENET-LAB3 PASS:' "$WORK/$n.log" 2>/dev/null | tail -1 |
           sed -n 's/.*rx_foreign_ignored=\([0-9]*\).*/\1/p')

    printf "  %-6s up=%s beats=%-5s corrupt=%-3s gaps=%-3s self-ignored=%-4s foreign-ignored=%s\n" \
           "$n" "$up" "$beats" "$corrupt" "$gaps" "${self:-0}" "${foreign:-0}"

    [ "$up" -ge 1 ]     || { echo "    FAIL: never came up";                fail=1; }
    [ "$beats" -ge 20 ] || { echo "    FAIL: never saw BOTH peers, re-armed"; fail=1; }
    # CORRUPTION IS THE ASSERTION.
    [ "$corrupt" -eq 0 ] || { echo "    FAIL: $corrupt corrupt frame(s):";   fail=1
        grep -a 'ENET-LAB3 CORRUPT:' "$WORK/$n.log" | head -3 | sed 's/^/      /'; }
    #
    # ⭐ A NEGATIVE RESULT IS ONLY A RESULT IF THE CONDITION WAS PRESENT.
    #
    # holobench, from the first REAL 4-node run: mcx rejected 0x86DD five times, rt1180
    # twelve.  0x86DD is IPv6 -- the Linux nodes' kernels doing NDP/MLD on the shared
    # segment.  Both were body-checking traffic THAT IS NOT THEIR PROTOCOL and calling it
    # CORRUPT.  "A corruption detector that cries foul at traffic that was never its
    # protocol will be turned off by the people it protects."
    #
    # This node is structurally immune -- it only inspects an ethertype matching a DECLARED
    # peer.  But "we never flagged IPv6" is worth NOTHING as an absence: it is the same log
    # as "there was no IPv6".  So the node COUNTS foreign frames, and this asserts BOTH
    # halves: the traffic WAS THERE, and we did not touch it.  Without the >0 check this
    # assertion could quietly become vacuous the day the segment goes quiet.
    #
    [ "${foreign:-0}" -ge 1 ] || { echo "    FAIL: no foreign (non-beacon) traffic seen at all --"
        echo "          the 'we ignore IPv6' claim is VACUOUS on this run, not proven."; fail=1; }
done

# ---------------------------------------------------------------------------
# ⭐ AN ASSERTION THAT HAS NEVER FIRED IS NOT AN ASSERTION.
#
# "0 corrupt frames" above is worthless unless the checker CAN say otherwise.  So run
# the segment again with one node deliberately LYING -- its payload ethertype disagrees
# with its header ethertype, which is precisely the stale/clobbered-buffer signature
# rt1180's NETC writeback bug produced -- and require that the honest nodes BOTH
#   (a) print ENET-LAB3 CORRUPT, and
#   (b) REFUSE TO COUNT IT AS SEEING A PEER (so they never reach a full PASS).
#
# (b) is the half that matters.  A checker that prints a warning and counts the frame
# anyway is a checker that makes you feel observed without observing anything.
# ---------------------------------------------------------------------------
echo
echo "== NEGATIVE TEST: one node lies (payload ethertype != header ethertype) =="
for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
PIDS=()
sleep 1
MCAST="230.0.0.2:11892"
mknode good 0x88B8 c8 0x88B9            # honest, expects the liar
EVIL=ethertype mknode liar 0x88B9 c9 0x88B8
sleep 12

evil_seen=$(grep -ac 'ENET-LAB3 EVIL:'    "$WORK/liar.log" 2>/dev/null) || true
caught=$(grep -ac 'ENET-LAB3 CORRUPT:'    "$WORK/good.log" 2>/dev/null) || true
duped=$(grep -ac 'ENET-LAB3 PASS:'        "$WORK/good.log" 2>/dev/null) || true

echo "  liar armed         : $evil_seen  (must be >=1, or the mutation never landed)"
echo "  honest node CAUGHT : $caught corrupt frame(s)"
echo "  honest node DUPED  : $duped PASS beat(s)   (must be 0 -- a caught frame must not count)"
grep -a 'ENET-LAB3 CORRUPT:' "$WORK/good.log" 2>/dev/null | head -1 | sed 's/^/    /'

# The mutation MUST have landed.  I shipped a "clean pass" off a stale binary once this
# week; a negative test that does not verify its own mutation tests the thing it was
# supposed to have changed.
[ "${evil_seen:-0}" -ge 1 ] || { echo "    FAIL: the liar never armed -- this test proved nothing"; fail=1; }
[ "${caught:-0}"    -ge 1 ] || { echo "    FAIL: the corruption check NEVER FIRED"; fail=1; }
[ "${duped:-0}"     -eq 0 ] || { echo "    FAIL: honest node reached PASS while being lied to"; fail=1; }

# ---------------------------------------------------------------------------
# ⭐ THE FLAG-DAY TRAP, AND WHY THIS NODE DOES NOT NEED ONE.
#
# holobench, tonight, aimed squarely at receivers like this one:
#
#   "A RECEIVER THAT ENFORCES A FIELD ITS SENDERS DO NOT YET EMIT WILL CONDEMN THE HONEST
#    ... and THE FALSE POSITIVE OF THIS DETECTOR IS INDISTINGUISHABLE FROM ITS TRUE
#    POSITIVE.  'CORRUPT, magic=0' is EXACTLY what rt1180's 88 frames DMA'd to guest
#    address 0 look like.  They will hunt a QEMU bug that is not there, or -- far worse --
#    CONCLUDE THE CHECK IS BROKEN AND DELETE IT."
#
# Dead right, and my beacon enforced unconditionally: on the real segment it would have
# gone RED against rt1180 and imx95, who are working perfectly and simply have not shipped
# the emitter yet.
#
# The prescribed fix is a coordinated flag day.  But the premise is escapable:
#
#   ⭐ A PEER THAT HAS EVER EMITTED A VALID BODY CANNOT STOP KNOWING HOW.
#
# So the two cases are only identical if you look at ONE FRAME.  Look at the SENDER:
#   never emitted a body      -> hasn't shipped the emitter.  COUNT IT.  Say so once.
#   emitted, and now magic=0  -> A BUFFER THAT WAS NEVER WRITTEN.  CORRUPT.
#
# The enforcer arms itself, per peer, on first evidence.  Both branches are proven below --
# with the SAME FRAME (magic=0) landing as benign in one and as a hard fail in the other.
# ---------------------------------------------------------------------------
echo
echo "== PHASE-1 SAFETY: a peer that emits NO body (rt1180/imx95, today) =="
for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
PIDS=()
MCAST="230.0.0.5:11895"
mknode ph1good 0x88B8 a8 0x88B9
LEGACY=1 mknode ph1old 0x88B9 a9 0x88B8
sleep 14

l_note=$(grep -ac 'ENET-LAB3 LEGACY:'  "$WORK/ph1good.log" 2>/dev/null) || true
l_bad=$(grep -ac 'ENET-LAB3 CORRUPT:'  "$WORK/ph1good.log" 2>/dev/null) || true
l_pass=$(grep -ac 'ENET-LAB3 PASS:'    "$WORK/ph1good.log" 2>/dev/null) || true
echo "  noted as legacy : $l_note   (said once, not once per frame)"
echo "  CONDEMNED       : $l_bad   (must be 0 -- DO NOT CONDEMN THE HONEST)"
echo "  counted anyway  : $l_pass PASS beat(s)  (must be >0)"
[ "${l_bad:-1}"  -eq 0 ] || { echo "    FAIL: condemned a peer that simply has not shipped the emitter"; fail=1; }
[ "${l_pass:-0}" -ge 1 ] || { echo "    FAIL: refused to count an honest phase-1 peer"; fail=1; }
[ "${l_note:-0}" -ge 1 ] || { echo "    FAIL: never noticed the peer emits no body"; fail=1; }

echo
echo "== SELF-ARMING: a KNOWN EMITTER whose buffers stop being written =="
echo "   (no strict mode, no flag day -- rt1180's frames-to-address-0 signature)"
for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
PIDS=()
MCAST="230.0.0.6:11896"
mknode rotgood 0x88B8 b1 0x88B9
ROT=6000 mknode rotbad 0x88B9 b2 0x88B8
sleep 16

r_arm=$(grep -ac 'body STOPPED'        "$WORK/rotbad.log" 2>/dev/null) || true
r_bad=$(grep -ac 'ENET-LAB3 CORRUPT:'  "$WORK/rotgood.log" 2>/dev/null) || true
r_leg=$(grep -ac 'ENET-LAB3 LEGACY:'   "$WORK/rotgood.log" 2>/dev/null) || true
echo "  rot node armed  : $r_arm   (the mutation must land)"
echo "  CAUGHT          : $r_bad corrupt frame(s)"
echo "  mis-excused     : $r_leg   (must be 0 -- it DID emit, so it is not phase-1)"
grep -a 'ENET-LAB3 CORRUPT:' "$WORK/rotgood.log" 2>/dev/null | head -1 | sed 's/^/    /'
[ "${r_arm:-0}" -ge 1 ] || { echo "    FAIL: the rot never armed -- this proved nothing"; fail=1; }
[ "${r_bad:-0}" -ge 1 ] || { echo "    FAIL: a known emitter went silent-bodied and we EXCUSED it"; fail=1; }
[ "${r_leg:-1}" -eq 0 ] || { echo "    FAIL: excused a known emitter as a phase-1 peer"; fail=1; }

# ---------------------------------------------------------------------------
# ⭐ FRESHNESS, NOT VALIDITY.  THE STALE FRAME IS A *VALID* FRAME.
#
# rt1180, tonight, closing the last hole -- and it lands on every check above:
#
#   "THE CORRUPTION IS NOT A MANGLED FRAME.  IT IS AN *OLD* ONE, DELIVERED AGAIN.  When the
#    RX path drops a frame it leaves the descriptor pointing at a STALE BUFFER -- which
#    holds a PREVIOUSLY VALID frame, with a PERFECTLY VALID CHECKSUM.  Every integrity
#    check that asks 'is this frame well-formed?' answers YES -- because IT IS.  It is just
#    not the frame that arrived."
#
# So magic, the self-consistent ethertype, and the 0x5A pattern ALL SAY GOOD FRAME to a
# replayed buffer.  My body check was asking the wrong question.  I even CARRIED the
# sequence number that could see it -- and only ever looked FORWARD with it (gaps = loss =
# a statistic).  A seq going BACKWARDS was silently accepted, and it overwrote last_seq,
# dragging my own baseline back with it.  rt1180's 88 stale frames were invisible to me too.
#
#   ⭐ ASSERT ON A NUMBER GOING UP.  The question is not "is this a GOOD frame" -- every
#      check I had already answered that.  It is "is this a NEW one."
# ---------------------------------------------------------------------------
echo
echo "== STALE-BUFFER REPLAY: every 3rd frame re-delivers the previous seq =="
echo "   (each replayed frame is PERFECTLY VALID -- magic ok, ethertype ok, pattern ok)"
for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
PIDS=()
MCAST="230.0.0.7:11897"
mknode repgood 0x88B8 c1 0x88B9
REPLAY=3 mknode repbad 0x88B9 c2 0x88B8
sleep 16

p_arm=$(grep -ac 'REPLAYS the previous' "$WORK/repbad.log" 2>/dev/null) || true
p_rep=$(grep -ac 'PAYLOAD-REPLAY'       "$WORK/repgood.log" 2>/dev/null) || true
p_wf=$(grep -a 'ENET-LAB3 CORRUPT:' "$WORK/repgood.log" 2>/dev/null | grep -avc 'PAYLOAD-REPLAY') || true
echo "  replayer armed        : $p_arm   (the mutation must land)"
echo "  well-formedness fails : $p_wf   (must be 0 -- EVERY stale frame IS well-formed)"
echo "  PAYLOAD-REPLAY caught : $p_rep   (only the FRESHNESS check can see this)"
grep -a 'PAYLOAD-REPLAY' "$WORK/repgood.log" 2>/dev/null | head -1 | sed 's/^/    /'
[ "${p_arm:-0}" -ge 1 ] || { echo "    FAIL: the replayer never armed -- this proved nothing"; fail=1; }
[ "${p_rep:-0}" -ge 1 ] || { echo "    FAIL: a stale buffer was replayed and we COUNTED IT AS FRESH"; fail=1; }
[ "${p_wf:-1}"  -eq 0 ] || { echo "    NOTE: a stale frame tripped a well-formedness check -- unexpected"; }

# ---------------------------------------------------------------------------
# ⭐ FRESHNESS AND LIVENESS COMPOSE: A NODE SAYING NOTHING *NEW* IS SAYING NOTHING.
#
# mcxn947qemu found this as an emergent property of the same freshness fix -- not designed,
# just true: a rejected frame never refreshes its peer's liveness timestamp, so a peer that
# ONLY EVER REPEATS ITSELF is correctly reclassified as one that WENT QUIET.
#
# (holobench: that means a stalled ring and a departed peer produce the SAME signal, which
#  is the honest answer -- from the segment's point of view they ARE the same event. Your
#  scorer gets LOST either way and does not need to care which.)
#
# I checked mine by reading the code, went to demonstrate it with beacon.replay=1 -- and it
# did NOT reproduce. My own impostor was wrong: replay=1 emits 0,0,1,2,3,4..., which is
# LAGGED BY ONE but still MONOTONIC after the first duplicate, i.e. a genuinely fresh peer
# that a correct receiver rightly accepts. I was about to report an emergent property using
# a peer that was not, in fact, stale.
#
#     A NEGATIVE TEST THAT DOES NOT PRODUCE THE CONDITION IT NAMES IS NOT A NEGATIVE TEST.
#
# beacon.freeze=1 is the real thing: the sequence is PINNED. Every frame is well-formed and
# says nothing new.
# ---------------------------------------------------------------------------
echo
echo "== PURE REPEATER: a peer whose sequence NEVER ADVANCES =="
for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
wait 2>/dev/null
PIDS=()
MCAST="230.0.0.10:11900"
mknode frzgood 0x88B8 e1 0x88B9
FREEZE=1 mknode frzbad 0x88B9 e2 0x88B8
sleep 18

f_arm=$(grep -ac 'PURE REPEATER'      "$WORK/frzbad.log" 2>/dev/null) || true
f_bad=$(grep -ac 'ENET-LAB3 CORRUPT:' "$WORK/frzgood.log" 2>/dev/null) || true
f_lost=$(grep -ac 'ENET-LAB3 LOST:'   "$WORK/frzgood.log" 2>/dev/null) || true
echo "  repeater armed : $f_arm    (the mutation must land)"
echo "  CAUGHT         : $f_bad corrupt frame(s)"
echo "  reclassified   : $f_lost LOST  (a peer saying nothing NEW is saying nothing)"
grep -a 'ENET-LAB3 LOST:' "$WORK/frzgood.log" 2>/dev/null | tail -1 | sed 's/^/    /'
[ "${f_arm:-0}"  -ge 1 ] || { echo "    FAIL: the repeater never armed -- this proved nothing"; fail=1; }
[ "${f_bad:-0}"  -ge 1 ] || { echo "    FAIL: a pure repeater was accepted as fresh"; fail=1; }
[ "${f_lost:-0}" -ge 1 ] || { echo "    FAIL: a peer that says nothing new was still counted LIVE"; fail=1; }

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: three i.MX 91 FEC nodes hold a broadcast segment -- every frame's BODY"
    echo "      verified (magic + self-consistent ethertype + 0x5A pattern), every node"
    echo "      re-earning PASS on a sliding window, every node ignoring its own frames"
    echo "      by BOTH ethertype and source MAC.  Zero corrupt frames.
      AND the corruption check is PROVEN TO FIRE: a node whose payload ethertype
      disagrees with its header is caught, and is NOT counted as a peer.
      AND IT NEEDS NO FLAG DAY: the SAME frame (magic=0) is excused as a phase-1
      peer from a sender that has never emitted a body, and condemned as a
      never-written buffer from a sender that has.  Ask the SENDER, not the frame.
      AND a REPLAYED STALE BUFFER is caught -- a frame that passes every
      well-formedness check this node has, because it IS well-formed; it is just
      not NEW.  Freshness, not validity: assert on a number going up.
      AND a PURE REPEATER -- a peer whose seq never advances -- is caught AND
      reclassified as LOST: freshness and liveness compose, so a node saying
      nothing NEW is saying nothing.
      AND foreign traffic (IPv6 NDP/MLD from three real Linux kernels) WAS on the
      wire and was NOT body-checked, NOT reported -- asserted as a MEASUREMENT,
      not inferred from an absence."
else
    echo "FAIL: see above."
fi
exit $fail
