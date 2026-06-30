#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Interconnect readiness (i.MX 91), UART link: prove two QEMU i.MX 91 instances
# pass real data over a serial port, bridged by a QEMU socket chardev - the shape
# a lab coordinator wires for a UART link. A known payload travels guest A's
# LPUART2 (/dev/ttyLP1) -> socket bridge -> guest B's LPUART2 and is verified
# byte-exact on the receiver.
#
# The base EVK DTB only enables LPUART1 (the console, ttyLP0), so this harness
# generates a one-line-patched DTB that flips serial@44390000 (LPUART2) from
# status="disabled" to "okay" (the serial1 alias already points at it) - then the
# kernel exposes /dev/ttyLP1, which QEMU wires to each board's 2nd -serial.
#
# Topology: console on -serial #1 (LPUART1, a log file); the link on -serial #2
# (LPUART2) = a socket chardev, server instance listens, client connects.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
PAYLOAD=${PAYLOAD:-IMX91-UART-LINK-payload-0123456789}
MEM=${MEM:-2G}
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$DTC" ] || skip "no dtc (need it to enable LPUART2); set DTC="

SOCK=${SOCK:-$(mktemp -u /tmp/imx91-uartlink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

# ---- patch the DTB: enable LPUART2 (serial@44390000) ------------------------
# The base EVK DTB only enables LPUART1 (the console), so flip serial@44390000
# from status="disabled" to "okay" (the serial1 alias already points here). The
# node keeps its dmas= props: the model services DMA-mode RX (the LPUART asserts
# its eDMA request + an IDLE interrupt, and the eDMA pages bytes from DATA into
# the driver's cyclic ring), so no PIO workaround is needed.
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /serial@44390000 \{/ { inn = 1 }
  inn && /status = "disabled"/ { sub(/disabled/, "okay"); inn = 0 }
  { print }
' "$WORK/base.dts" > "$WORK/uart.dts"
"$DTC" -I dts -O dtb "$WORK/uart.dts" 2>/dev/null > "$WORK/uart.dtb" || die "dtc recompile failed"
DTB2="$WORK/uart.dtb"

# ---- stage a role-based initramfs -------------------------------------------
build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$role" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
U=/dev/ttyLP1
[ -c "$U" ] || { echo "LINK:FAIL:uart:no $U (LPUART2 not enabled)"; busybox poweroff -f; }
stty -F "$U" raw -echo 115200 2>/dev/null
echo "=== INTERCONNECT uart ($ROLE on $U) ==="
if [ "$ROLE" = recv ]; then
    if read -t 60 RX < "$U"; then
        if [ "$RX" = "$PAYLOAD" ]; then
            echo "LINK:PASS:recv:got byte-exact [$RX]"
        else
            echo "LINK:FAIL:recv:mismatch [$RX] != [$PAYLOAD]"
        fi
    else
        echo "LINK:FAIL:recv:timeout"
    fi
else
    sleep 8                       # let the receiver open + the socket connect
    printf '%s\n' "$PAYLOAD" > "$U"
    echo "LINK:SENT:$PAYLOAD"
    sleep 2
fi
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$role.gz"
    echo "$WORK/$role.gz"
}

RECV_IRD=$(build_initrd recv)
SEND_IRD=$(build_initrd send)

boot() {                    # $1=initrd  $2=chardev-args  $3=logfile
    timeout "$TMO" "$QEMU" -M imx91-11x11-evk -smp 1 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -serial file:"$3" -serial chardev:ul \
        >/dev/null 2>&1 &
}

# ---- receiver (socket server) first, then sender (client) -------------------
RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 91 RECEIVER (LPUART2 socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=ul,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 2
echo "== booting i.MX 91 SENDER (LPUART2 socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=ul,path=$SOCK,server=off" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== UART LINK =================="
grep -aE 'INTERCONNECT|LINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'LINK:PASS:recv' "$RLOG"; then
    echo "PASS: payload crossed LPUART2<->socket<->LPUART2 byte-exact between two i.MX 91 guests"
    exit 0
fi
die "uart link did not complete"
