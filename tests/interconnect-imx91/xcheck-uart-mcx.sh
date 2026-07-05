#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Mixed-SoC UART board-to-board lab, proven locally: uart-link-imx-mcx.
# One i.MX 91 (full Linux) is the PEER; one MCXN947 (bare-metal M33) runs the
# real tests/mcxn-uart-link firmware. Their LPUART2s are bridged by a QEMU socket
# chardev (MCX listens, 91 connects) - exactly the shape holobench wires in a lab.
#
# Protocol (the MCX firmware owns the pattern; the 91 is a dumb GO+echo peer):
#   1. the M33 enables RX and spin-waits for a GO byte (robust to boot/connect
#      timing - it waits indefinitely, so the 91's slow boot is absorbed);
#   2. the 91 boots, opens /dev/ttyLP1, sends "G" (uartpeer), then ECHOES;
#   3. the M33 sends 32 bytes expect(i)=i*7+3, the 91 echoes them back;
#   4. the M33 RX-ISR verifies the echo byte-for-byte -> "UART LINK PASS 32".
#
# This pre-stages/validates the 91 SIDE of the lab against the MCX's real
# firmware. holobench drives the same two nodes through its coordinator; the 91
# deliverables it needs are here: LPUART2=serial_hd(1)=/dev/ttyLP1 (enable
# serial@44390000), a socket chardev on the 2nd -serial, and the uartpeer tool.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MCX=${MCX_ROOT:-$HOME/Documents/GitHub/mcxn947qemu}

QEMU91=${QEMU91:-$ROOT/build/qemu-system-aarch64}
QEMUMCX=${QEMUMCX:-$MCX/build/qemu-system-arm}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
ARMCC=${ARMCC:-arm-none-eabi-gcc}
MEM=${MEM:-2G}
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU91" "$QEMUMCX" "$IMAGE" "$DTB" "$INITRD_SRC" \
         "$MCX/tests/mcxn-uart-link/main.c" "$MCX/tests/mcxn-uart-link/link.ld"; do
    [ -e "$f" ] || skip "missing $f"
done
[ -x "$DTC" ] || skip "no dtc (need it to enable LPUART2); set DTC="
command -v "$CROSS" >/dev/null || skip "no aarch64 cross compiler ($CROSS)"
command -v "$ARMCC" >/dev/null || skip "no arm cross compiler ($ARMCC)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx91-mcx-uart.XXXXXX.sock)}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK" "$SOCK"; kill ${MPID:-} ${PPID91:-} 2>/dev/null' EXIT
MPID=; PPID91=

# ---- build the two firmwares -----------------------------------------------
"$CROSS" -O2 -static -o "$WORK/uartpeer" "$HERE/uartpeer.c" || die "uartpeer build failed"
"$ARMCC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
    -T "$MCX/tests/mcxn-uart-link/link.ld" "$MCX/tests/mcxn-uart-link/main.c" \
    -o "$WORK/uartlink.elf" || die "uartlink.elf build failed"

# ---- patch the 91 DTB: enable LPUART2 (serial@44390000) -> /dev/ttyLP1 ------
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /serial@44390000 \{/ { inn = 1 }
  inn && /status = "disabled"/ { sub(/disabled/, "okay"); inn = 0 }
  { print }
' "$WORK/base.dts" > "$WORK/uart.dts"
"$DTC" -I dts -O dtb "$WORK/uart.dts" 2>/dev/null > "$WORK/uart.dtb" || die "dtc recompile failed"

# ---- stage the 91 peer initramfs -------------------------------------------
stage="$WORK/peer"; mkdir -p "$stage"
zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
install -m755 "$WORK/uartpeer" "$stage/uartpeer"
cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
U=/dev/ttyLP1
n=0; while [ ! -c "$U" ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
[ -c "$U" ] || { echo "UARTPEER:FAIL:no $U (LPUART2 not enabled)"; busybox poweroff -f; }
echo "=== 91 UART PEER on $U ==="
/uartpeer "$U" 32
echo "=== PEER-DONE ==="
/bin/busybox poweroff -f
INIT
chmod +x "$stage/init"
( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/peer.gz"

MLOG="$WORK/mcx-console.log"; PLOG="$WORK/peer.log"

# ---- boot the MCX (socket server, M33 waits for GO) ------------------------
echo "== booting MCXN947 (uartlink.elf, LPUART2 socket listen) =="
timeout "$TMO" "$QEMUMCX" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$MLOG" \
    -chardev "socket,id=ul,path=$SOCK,server=on,wait=off" -serial chardev:ul \
    -kernel "$WORK/uartlink.elf" -no-reboot >/dev/null 2>&1 &
MPID=$!
sleep 2

# ---- boot the 91 (socket client, runs uartpeer) ----------------------------
echo "== booting i.MX 91 PEER (LPUART2 socket connect, uartpeer G+echo) =="
timeout "$TMO" "$QEMU91" -M imx91-11x11-evk -smp 1 -m "$MEM" -display none \
    -kernel "$IMAGE" -dtb "$WORK/uart.dtb" -initrd "$WORK/peer.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -serial file:"$PLOG" \
    -chardev "socket,id=ul,path=$SOCK,server=off,reconnect-ms=1000" -serial chardev:ul \
    -nic none -monitor none >/dev/null 2>&1 &
PPID91=$!

wait $PPID91 2>/dev/null
for _ in $(seq 1 80); do
    grep -qE "UART LINK (PASS|FAIL)" "$MLOG" 2>/dev/null && break
    sleep 0.1
done
kill $MPID 2>/dev/null

echo "============== MIXED-SoC UART LINK (91 <-> MCX) =============="
echo "--- 91 peer ---";  grep -aE 'UARTPEER:' "$PLOG" | grep -avE '^\[ *[0-9]' | sed 's#.*/##'
echo "--- MCX M33 ---";  grep -aE 'UART LINK' "$MLOG"
if grep -aq "UART LINK PASS" "$MLOG" && grep -aq "UARTPEER:PASS" "$PLOG"; then
    echo "PASS: 32 bytes crossed 91 LPUART2 <-> socket <-> MCX LPUART2 byte-exact (Linux-aarch64 <-> bare-metal M33)"
    exit 0
fi
die "mixed-SoC uart link did not complete (no UART LINK PASS)"
