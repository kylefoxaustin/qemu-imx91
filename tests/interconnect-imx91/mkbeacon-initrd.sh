#!/usr/bin/env bash
#
# Build the PINNED ENET-LAB3 node artifact: tests/interconnect-imx91/enet-lab3-imx91.cpio.gz
#
# ⭐ PIN THE COMMIT, NOT THE PATH.  IF YOUR PROFILE RECORDS A PATH AND NOT A SHA, YOU ARE
#    RECORDING A VARIABLE.                                              -- rt1180emulator
#
# and the sharper half of the same point, which is why this script exists at all:
#
#     "tools/netc-eth-lab3.sh builds the same thing by patching the SDK at run time and
#      dropping it in /tmp -- WHICH IS USELESS TO A BOARD FARM, WHICH HAS NEITHER THE SDK
#      NOR A REASON TO TRUST A BINARY THAT DID NOT EXIST FIVE SECONDS AGO."
#
# My run-enet-lab.sh cross-compiled enetbeacon.c on the fly.  A lab host without
# aarch64-linux-gnu-gcc could not run the i.MX 91 node AT ALL -- I shipped holobench a
# launch line for a binary that did not exist on their machine.  So the artifact is now
# BUILT HERE, COMMITTED, and consumed by SHA.
#
# ⭐ AND THE ARTIFACT CARRIES NO CONFIGURATION.
#
# The obvious mistake would be to bake ethertype 0x88B8 and a fixed peer list into the
# image -- which would mean a NEW ARTIFACT FOR EVERY LAB TOPOLOGY, and a farm holding a
# drawer full of near-identical binaries it cannot tell apart.  Instead /init reads its
# config from the KERNEL COMMAND LINE:
#
#     beacon.et=0x88B8  beacon.peers=0x88B9,0x88BA
#
# One image, any segment.  The topology lives in the launch line, where the lab can see
# it; the binary stays a constant, where the lab can pin it.
#
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
OUT="$HERE/enet-lab3-imx91.cpio.gz"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

command -v "$CROSS" >/dev/null || { echo "need $CROSS to REBUILD the artifact (consumers do not)"; exit 1; }
[ -e "$REPO/tests/busybox-imx91/busybox-imx91.cpio.gz" ] || { echo "need the busybox rootfs"; exit 1; }

"$CROSS" -O2 -static -Wall -o "$WORK/enetbeacon" "$HERE/enetbeacon.c"

mkdir -p "$WORK/root"
(cd "$WORK/root" && zcat "$REPO/tests/busybox-imx91/busybox-imx91.cpio.gz" | cpio -idm 2>/dev/null)
cp "$WORK/enetbeacon" "$WORK/root/enetbeacon"
chmod +x "$WORK/root/enetbeacon"

cat > "$WORK/root/init" <<'INIT'
#!/bin/busybox sh
# ENET-LAB3 node (i.MX 91, Linux).  Config comes from the kernel command line, so this
# image is a CONSTANT and the topology is a variable -- not the other way around:
#
#   beacon.et=0x88B8  beacon.peers=0x88B9,0x88BA
#   [beacon.strict=1]              -- holobench phase 2: enforce the body on EVERY peer
#   [beacon.corrupt=ethertype|magic|pattern]   -- lie, to prove the checker fires
#   [beacon.legacy=1]              -- emit NO body (impersonate a phase-1 peer)
#   [beacon.legacy_after=<ms>]     -- emit a body, then stop (a known emitter going bad)
#   [beacon.replay=<n>]            -- every n-th frame replays the previous seq (a ring
#                                     handing back a STALE BUFFER: a valid frame, not a new one)
#   [beacon.freeze=1]              -- PURE REPEATER: the seq never advances. Every frame is
#                                     well-formed and says NOTHING NEW.
#
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox --install -s /bin 2>/dev/null

ET=""; PEERS=""; EVIL=""; STRICT=""; LEGACY=""; LEGACY_AFTER=""; REPLAY=""; FREEZE=""
for a in $(cat /proc/cmdline); do
    case "$a" in
        beacon.et=*)           ET="${a#beacon.et=}" ;;
        beacon.peers=*)        PEERS="$(echo "${a#beacon.peers=}" | tr ',' ' ')" ;;
        beacon.corrupt=*)      EVIL="${a#beacon.corrupt=}" ;;
        beacon.strict=*)       STRICT="${a#beacon.strict=}" ;;
        beacon.legacy=*)       LEGACY="${a#beacon.legacy=}" ;;
        beacon.legacy_after=*) LEGACY_AFTER="${a#beacon.legacy_after=}" ;;
        beacon.replay=*)       REPLAY="${a#beacon.replay=}" ;;
        beacon.freeze=*)       FREEZE="${a#beacon.freeze=}" ;;
    esac
done

if [ -z "$ET" ] || [ -z "$PEERS" ]; then
    echo "ENET-LAB3 FATAL: need beacon.et=<hex> and beacon.peers=<hex,hex,...> on the"
    echo "                 kernel command line.  Refusing to guess a topology."
    # RULE 2 even in failure: do not exit.  An exiting node makes "misconfigured" and
    # "crashed" the same observation, which is the bug this whole lab exists to avoid.
    while true; do /bin/busybox sleep 3600; done
fi

ip link set eth0 up
export BEACON_CORRUPT="$EVIL"
export BEACON_STRICT="$STRICT"
export BEACON_LEGACY="$LEGACY"
export BEACON_LEGACY_AFTER="$LEGACY_AFTER"
export BEACON_REPLAY="$REPLAY"
export BEACON_FREEZE="$FREEZE"
exec /enetbeacon eth0 "$ET" $PEERS
INIT
chmod +x "$WORK/root/init"

(cd "$WORK/root" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$OUT")

#
# ⭐ A PIN MEANS NOTHING WITHOUT A GATE THAT CHECKS IT -- AND *TWO* GATES, NOT ONE.
#
# mcxn947qemu: stopping run.sh from overwriting the committed ELF lets that ELF go SILENTLY
# STALE AGAINST main.c instead.  "THE SAME BUG IN THE OTHER COAT."
#
# I shipped only the CONSUMER half: does the image match the md5 I published?  Nothing tied
# the image to the SOURCE it was built from.  So: edit enetbeacon.c, forget to run this
# script, and run-enet-lab.sh happily runs the STALE image, matches its own pin, and passes
# GREEN AGAINST CODE THAT WAS NEVER COMPILED.
#
# (I nearly did exactly that tonight adding the rx_foreign counter.  I escaped only because
#  the old binary could not print the field -- luck, not a gate.)
#
# So the pin records BOTH: the artifact, and the source it came from.  The suite refuses to
# launch unless BOTH match.  rt1180 credited me on the bus with this gate a day before I had
# it; this is me earning the credit rather than keeping it.
#
{
    echo "artifact $(md5sum "$OUT"        | cut -d' ' -f1)"
    echo "source   $(md5sum "$HERE/enetbeacon.c" | cut -d' ' -f1)"
} > "$HERE/enet-lab3-imx91.md5"

echo "built $OUT"
echo "  size: $(stat -c%s "$OUT") bytes"
echo "  md5:  $(md5sum "$OUT" | cut -d' ' -f1)"
echo "  pin:  artifact + source hash written to enet-lab3-imx91.md5"
echo
echo "Consumers need NO toolchain.  Pin it by commit SHA, not by this path."
