#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Interconnect readiness (i.MX 91), SPI link: prove two QEMU i.MX 91 instances
# pass real data over an SPI board-to-board link, bridged by a QEMU socket - the
# shape a lab coordinator wires for a SPI segment. A known payload travels guest
# A's LPSPI (via spidev) -> spi-link peripheral -> socket bridge -> guest B's
# spi-link -> LPSPI -> spidev, and is verified byte-exact on the receiver.
#
# SPI is master-driven, so each side is an LPSPI *master* with a spi-link SSI
# peripheral on its bus (-device spi-link,bus=lpspiN,chardev=...). spi-link
# forwards each shifted-out byte (MOSI) to the chardev and returns peer bytes on
# MISO from an rx FIFO - so the sender clocks the payload out and the receiver
# clocks dummy bytes to shift it in. The base EVK DTB disables the LPSPIs, so the
# harness enables lpspi1 (spi@44360000), drops its dmas (the model is PIO), and
# adds a spidev child so the guest exposes /dev/spidev*.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PAYLOAD=${PAYLOAD:-IMX91-SPI-LINK-payload-0123456789}
MEM=${MEM:-2G}
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$DTC" ] || skip "no dtc (need it to enable lpspi1); set DTC="
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx91-spilink.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT
SPID=; CPID=

# ---- build the spidev oracle (static aarch64) -------------------------------
"$CROSS" -O2 -static -o "$WORK/spilink" "$HERE/spilink.c" || die "spilink build failed"

# ---- patch the DTB: enable lpspi1 (spi@44360000) + a spidev child -----------
# Flip spi@44360000 to okay, drop its dmas (the imx93_lpspi model is PIO -
# ssi_transfer, no eDMA), and add a spidev@0 child (rohm,dh2228fv is in the
# kernel spidev allow-list) so the guest exposes /dev/spidev*.
"$DTC" -I dtb -O dts "$DTB" 2>/dev/null > "$WORK/base.dts" || die "dtc decompile failed"
awk '
  /spi@44360000 \{/ { inn = 1 }
  inn && /dmas =|dma-names =/ { next }
  inn && /status = "disabled"/ { print "\t\t\t\tstatus = \"okay\";"; next }
  inn && /^\t\t\t\};/ {
    print "\t\t\t\tspidev@0 {";
    print "\t\t\t\t\tcompatible = \"rohm,dh2228fv\";";
    print "\t\t\t\t\treg = <0x00>;";
    print "\t\t\t\t\tspi-max-frequency = <0xf4240>;";
    print "\t\t\t\t};";
    print; inn = 0; next
  }
  { print }
' "$WORK/base.dts" > "$WORK/spi.dts"
"$DTC" -I dts -O dtb "$WORK/spi.dts" 2>/dev/null > "$WORK/spi.dtb" || die "dtc recompile failed"
DTB2="$WORK/spi.dtb"

# ---- stage a role-based initramfs -------------------------------------------
build_initrd() {            # $1=role  -> echoes path
    local role=$1
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/spilink" "$stage/spilink"
    printf 'ROLE=%s\nPAYLOAD=%s\n' "$role" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
S=$(ls /dev/spidev* 2>/dev/null | head -1)
[ -c "$S" ] || { echo "SPILINK:FAIL:no /dev/spidev (lpspi1 not bound)"; \
                 dmesg | grep -iE 'spi|lpspi' | tail -5; busybox poweroff -f; }
echo "=== INTERCONNECT spi ($ROLE on $S) ==="
if [ "$ROLE" = recv ]; then
    /spilink recv "$S" "$PAYLOAD"
else
    sleep 8                       # let the receiver open + the socket connect
    /spilink send "$S" "$PAYLOAD"
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
    timeout "$TMO" "$QEMU" -M imx91-11x11-evk -audio driver=none -smp 1 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -device spi-link,bus=lpspi1,chardev=spil \
        -serial file:"$3" -nic none \
        >/dev/null 2>&1 &
}

# ---- receiver (socket server) first, then sender (client) -------------------
RLOG="$WORK/recv.log"; SLOG="$WORK/send.log"
echo "== booting i.MX 91 RECEIVER (spi-link socket listen) =="
boot "$RECV_IRD" "-chardev socket,id=spil,path=$SOCK,server=on,wait=off" "$RLOG"; SPID=$!
sleep 2
echo "== booting i.MX 91 SENDER (spi-link socket connect) =="
boot "$SEND_IRD" "-chardev socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== SPI LINK =================="
grep -aE 'INTERCONNECT|SPILINK:' "$RLOG" "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'SPILINK:PASS' "$RLOG"; then
    echo "PASS: payload crossed LPSPI<->spi-link<->socket<->spi-link<->LPSPI byte-exact between two i.MX 91 guests"
    exit 0
fi
die "spi link did not complete"
