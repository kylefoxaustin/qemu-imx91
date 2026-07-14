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

skip() { echo "SKIP: $*"; exit 0; }
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
[ -e "$ARTIFACT" ] || skip "artifact not found: $ARTIFACT (run ./mkbeacon-initrd.sh)"

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
echo "  md5:    $(md5sum "$ARTIFACT" | cut -d' ' -f1)"

# One node.  The image is a CONSTANT; the topology rides on the kernel command line, so
# every node in the segment boots the SAME committed cpio.gz.
mknode() {                          # $1=name $2=my-et $3=mac-suffix  $4..=peer-ets
    local name=$1 myet=$2 suffix=$3; shift 3
    local peers; peers=$(echo "$*" | tr ' ' ',')
    local evil=""

    [ -n "${EVIL:-}" ] && evil="beacon.corrupt=$EVIL"

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

    printf "  %-6s up=%s beats=%-5s corrupt=%-3s gaps=%-3s self-frames-ignored=%s\n" \
           "$n" "$up" "$beats" "$corrupt" "$gaps" "${self:-0}"

    [ "$up" -ge 1 ]     || { echo "    FAIL: never came up";                fail=1; }
    [ "$beats" -ge 20 ] || { echo "    FAIL: never saw BOTH peers, re-armed"; fail=1; }
    # CORRUPTION IS THE ASSERTION.
    [ "$corrupt" -eq 0 ] || { echo "    FAIL: $corrupt corrupt frame(s):";   fail=1
        grep -a 'ENET-LAB3 CORRUPT:' "$WORK/$n.log" | head -3 | sed 's/^/      /'; }
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

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: three i.MX 91 FEC nodes hold a broadcast segment -- every frame's BODY"
    echo "      verified (magic + self-consistent ethertype + 0x5A pattern), every node"
    echo "      re-earning PASS on a sliding window, every node ignoring its own frames"
    echo "      by BOTH ethertype and source MAC.  Zero corrupt frames.
      AND the corruption check is PROVEN TO FIRE: a node whose payload ethertype
      disagrees with its header is caught, and is NOT counted as a peer."
else
    echo "FAIL: see above."
fi
exit $fail
