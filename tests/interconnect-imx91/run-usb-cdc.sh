#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Interconnect readiness (i.MX 91), USB-CDC serial link: prove the 91 carries a
# real /dev/ttyACM serial stream to a CDC-ACM gadget on ANOTHER emulator, over a
# QEMU usbredir socket - the "PuTTY into the board over USB-serial" path.
#
# This is the richer cousin of run-usb.sh (vendor bulk-echo): the device end is
# the fleet's MCX CDC-ACM gadget (frdm-mcxn947, tests/mcxn-usb-cdc/cdc.elf via
# serve.sh cdc), so the 91's real cdc_acm driver binds it as /dev/ttyACM0 and we
# round-trip a payload through the tty. Full chain both ways:
#   guest cdc_acm write -> ci_hdrc/EHCI -> usb-redir -> socket -> MCX gadget
#   EP1-OUT -> firmware echo -> EP1-IN -> cdc_acm read, byte-exact.
#
# Needs the SAME shared USB fixes as run-usb.sh (ChipIdea PORTSC.PSPD, bf8262fec5)
# plus, on the device end, the MCX CDC gadget's SET_LINE_CODING + write + the
# interrupt-callback SIGSEGV fixes. A first-write timing race (cdc_acm issuing its
# first bulk-OUT before the freshly-configured CDC-data endpoint is ready) is
# handled in ttyecho.c by a short settle + retry-on-EIO.
#
# SKIPs cleanly if the MCX gadget server or the cdc-acm.ko module are absent.
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
# cdc-acm.ko is a module (not in the busybox initramfs); pull it from the BSP.
BSP_MOD=$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work
CDCACM_KO=${CDCACM_KO:-$(ls "$BSP_MOD"/*/linux-imx/*/image/usr/lib/modules/*/kernel/drivers/usb/class/cdc-acm.ko 2>/dev/null | head -1)}
SOCK=${SOCK:-$(mktemp -u /tmp/imx91-usbcdc.XXXXXX.sock)}
MEM=${MEM:-2G}
TMO=${TMO:-240}
WARMUP=${WARMUP:-7}      # CDC enum is round-trip-heavy; warm the gadget longer

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$MCXSERVE" ] || skip "no MCX gadget server ($MCXSERVE); set MCXSERVE="
[ -n "$CDCACM_KO" ] && [ -e "$CDCACM_KO" ] || skip "no cdc-acm.ko (set CDCACM_KO=)"
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

WORK=$(mktemp -d); SRV=
trap 'rm -rf "$WORK" "$SOCK"; [ -n "$SRV" ] && kill "$SRV" 2>/dev/null' EXIT

# ---- build the ttyACM round-trip oracle (static aarch64) --------------------
"$CROSS" -O2 -static -o "$WORK/ttyecho" "$HERE/ttyecho.c" || die "ttyecho build failed"

# ---- stage the guest (busybox + cdc-acm.ko + the oracle) --------------------
STAGE="$WORK/root"; mkdir -p "$STAGE"
zcat "$INITRD_SRC" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
install -m755 "$WORK/ttyecho" "$STAGE/ttyecho"
install -m644 "$CDCACM_KO"    "$STAGE/cdc-acm.ko"
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
echo "=== 91 USB HOST: cdc_acm CDC-ACM serial round-trip ==="
insmod /cdc-acm.ko 2>&1
for i in $(seq 1 80); do [ -e /dev/ttyACM0 ] && break; sleep 1; done
dmesg | grep -iE 'new high-speed USB device|cdc_acm .*ttyACM|USB ACM device' | tail -3
if [ -e /dev/ttyACM0 ]; then
    echo "ENUM: /dev/ttyACM0 present after ${i}s"
    /ttyecho /dev/ttyACM0
else
    echo "ENUM:FAIL no /dev/ttyACM0 in 80s"
fi
echo "=== USB-CDC-TEST-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- launch the MCX CDC gadget server (device end), warm it, boot the 91 ----
rm -f "$SOCK"
echo "== launching MCX CDC-ACM gadget server (device end) on $SOCK =="
USB_SOCK="$SOCK" setsid bash "$MCXSERVE" cdc >"$WORK/server.log" 2>&1 &
SRV=$!
for i in $(seq 1 20); do [ -S "$SOCK" ] && break; sleep 0.5; done
[ -S "$SOCK" ] || { cat "$WORK/server.log"; die "gadget server socket never appeared"; }
sleep "$WARMUP"

LOG="$WORK/guest.log"
echo "== booting i.MX 91 as the usbredir host (importer) =="
timeout "$TMO" "$QEMU" -M imx91-11x11-evk -audio driver=none -smp 1 -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -chardev socket,id=ur0,path="$SOCK",server=off,reconnect-ms=2000 \
  -device usb-redir,chardev=ur0 \
  -nic user -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "============= USB-CDC LINK (i.MX 91 host <-> MCX CDC gadget) ============="
grep -aE 'new high-speed USB device|cdc_acm .*ttyACM|USB ACM device|ENUM:|TTYACM:' "$LOG" \
  | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/dev/##'
if grep -aq 'TTYACM:PASS' "$LOG"; then
    echo "PASS: 91 host bound the MCX CDC gadget as /dev/ttyACM0 + round-tripped a serial payload"
    exit 0
fi
grep -aqE 'ttyACM0' "$LOG" \
    && echo "note: ttyACM bound but the round-trip did not complete (try a larger WARMUP/TMO)"
die "USB-CDC link did not complete"
