#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Interconnect readiness (i.MX 91), USB link: prove the 91 passes real data over
# a USB link to a device on ANOTHER emulator, over a QEMU usbredir socket - the
# shape Holobench wires as a USB lab (device end + host end share one socket).
#
# USB is host/device asymmetric: the 91's ChipIdea controller is a HOST (EHCI),
# so the 91 is the usbredir HOST/importer (stock -device usb-redir), and the
# DEVICE end is the fleet's MCX gadget (frdm-mcxn947 running the HS/ChipIdea
# usbredir device firmware; tests/mcxn-usb-link/serve.sh in the mcxn947qemu
# tree). This is the same pairing the i.MX93 <-> MCX link uses.
#
# The full chain proven: guest usbfs app -> i.MX 91 Linux USB stack ->
# ci_hdrc/EHCI -> QEMU usb-redir -> unix socket -> MCX firmware EP1 echo -> back,
# byte-for-byte. Depends on the ChipIdea PORTSC.PSPD fix (commit bf8262fec5)
# without which the 91 host downgrades the HS gadget to full-speed and clamps
# its 512-byte bulk EPs.
#
# SKIPs cleanly if the mcxn947qemu gadget server isn't present.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
# The MCX (device end) gadget server, from the sibling mcxn947qemu tree.
MCXSERVE=${MCXSERVE:-$HOME/Documents/GitHub/mcxn947qemu/tests/mcxn-usb-link/serve.sh}
SOCK=${SOCK:-$(mktemp -u /tmp/imx91-usb-link.XXXXXX.sock)}
MEM=${MEM:-2G}
TMO=${TMO:-240}
WARMUP=${WARMUP:-5}      # let the device-end qemu warm up (2x-TCG contention)

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$MCXSERVE" ] || skip "no MCX gadget server ($MCXSERVE); set MCXSERVE="
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

WORK=$(mktemp -d); SRV=
trap 'rm -rf "$WORK" "$SOCK"; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null' EXIT

# ---- build the usbfs bulk-echo oracle (static aarch64) ----------------------
"$CROSS" -O2 -static -o "$WORK/usbbulk" "$HERE/usbbulk.c" || die "usbbulk build failed"

# ---- stage the guest (busybox + the oracle + a poll-then-bulk init) ---------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD_SRC" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
install -m755 "$WORK/usbbulk" "$STAGE/usbbulk"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== 91 USB HOST: waiting for the MCX gadget 1fc9:0094 ==="
found=0
for i in $(seq 1 80); do
    if grep -qi 0094 /sys/bus/usb/devices/*/idProduct 2>/dev/null; then
        found=1; break
    fi
    sleep 1
done
dmesg | grep -iE 'new (high|full)-speed USB device|New USB device found' | tail -3
if [ "$found" = 1 ]; then
    echo "ENUM: device enumerated after ${i}s"
    /usbbulk
else
    echo "ENUM:FAIL timeout (no 1fc9:0094 in 80s)"
fi
echo "=== USB-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- launch the MCX gadget server (device end), warm it, then boot the 91 ---
rm -f "$SOCK"
echo "== launching MCX HS gadget server (device end) on $SOCK =="
USB_SOCK="$SOCK" setsid bash "$MCXSERVE" hs >"$WORK/server.log" 2>&1 &
SRV=$!
for i in $(seq 1 20); do [ -S "$SOCK" ] && break; sleep 0.5; done
[ -S "$SOCK" ] || { cat "$WORK/server.log"; die "gadget server socket never appeared"; }
sleep "$WARMUP"

LOG="$WORK/guest.log"
echo "== booting i.MX 91 as the usbredir host (importer) =="
timeout "$TMO" "$QEMU" -M imx91-11x11-evk -smp 1 -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -chardev socket,id=ur0,path="$SOCK",server=off,reconnect-ms=2000 \
  -device usb-redir,chardev=ur0 \
  -nic user -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "================== USB LINK (i.MX 91 host <-> MCX gadget) =================="
grep -aE 'new (high|full)-speed USB device|New USB device found|ENUM:|BULK:' "$LOG" \
  | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'BULK:PASS' "$LOG"; then
    echo "PASS: 91 host enumerated the MCX HS gadget + bulk-echoed 64 bytes over the live link"
    exit 0
fi
grep -aqE 'new high-speed USB device' "$LOG" \
    && echo "note: HS attach reached; enumeration/bulk did not complete (try a larger WARMUP/TMO)"
die "USB link did not complete"
