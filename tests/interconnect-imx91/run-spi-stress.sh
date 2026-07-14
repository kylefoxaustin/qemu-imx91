#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# SPI-link back-pressure stress (i.MX 91): prove the spi-link peripheral never
# hangs the vCPU when the socket back-pressures. One 91 clocks a large continuous
# SPI stream (spistress.c) at a peer 91 that NEVER drains - the peer's spi-link rx
# FIFO fills, can_receive() goes to 0, the chardev stops reading the socket, and
# the socket send buffer fills. That used to block spi-link's qemu_chr_fe_write_all
# inside a TDR write and hang the guest (flagged by 93/95 under continuous full-
# duplex clocking). With the non-blocking tx-FIFO + G_IO_OUT drain, every transfer
# returns: the sender reaches STRESS:DONE instead of hanging.
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
ITERS=${ITERS:-300}
MEM=${MEM:-2G}
TMO=${TMO:-150}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
[ -x "$DTC" ] || skip "no dtc (need it to enable lpspi1); set DTC="
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SOCK=${SOCK:-$(mktemp -u /tmp/imx91-spistress.XXXXXX.sock)}
WORK=$(mktemp -d); trap 'rm -rf "$WORK" "$SOCK"; kill ${IPID:-} ${SPID:-} 2>/dev/null' EXIT
IPID=; SPID=

"$CROSS" -O2 -static -o "$WORK/spistress" "$HERE/spistress.c" || die "build failed"

# enable lpspi1 + a spidev child (drop dmas: PIO), same as run-spi.sh
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

build_initrd() {            # $1=role  $2=init-body  -> echoes path
    local stage="$WORK/$1"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/spistress" "$stage/spistress"
    printf 'ITERS=%s\n' "$ITERS" > "$stage/env"
    { printf '#!/bin/busybox sh\n'
      printf '/bin/busybox mount -t proc proc /proc\n'
      printf '/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null\n'
      printf '/bin/busybox mount -t sysfs sys /sys 2>/dev/null\n'
      printf '/bin/busybox --install -s /bin 2>/dev/null\n'
      printf 'exec > /dev/console 2>&1\n. /env\n'
      printf '%s\n' "$2"
      printf 'echo "=== STRESS-DONE ==="\n/bin/busybox poweroff -f\n'
    } > "$stage/init"
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$1.gz"
    echo "$WORK/$1.gz"
}

# idle peer: never opens spidev -> its spi-link rx FIFO fills -> back-pressures
IDLE_IRD=$(build_initrd idle 'echo IDLE-peer-up; sleep 60')
STRESS_IRD=$(build_initrd stress \
  'n=0; while [ ! -c /dev/spidev0.0 ] && [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done
   sleep 4; /spistress /dev/spidev0.0 "$ITERS"')

boot() {                    # $1=initrd  $2=chardev-args  $3=logfile
    timeout "$TMO" "$QEMU" -M imx91-11x11-evk -audio driver=none -smp 1 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB2" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        $2 -device spi-link,bus=lpspi1,chardev=spil \
        -serial file:"$3" -nic none -monitor none >/dev/null 2>&1 &
}

ILOG="$WORK/idle.log"; SLOG="$WORK/stress.log"
echo "== booting IDLE peer (spi-link socket listen, never drains) =="
boot "$IDLE_IRD" "-chardev socket,id=spil,path=$SOCK,server=on,wait=off" "$ILOG"; IPID=$!
sleep 4
echo "== booting STRESS sender ($ITERS x 2KB continuous clock) =="
boot "$STRESS_IRD" "-chardev socket,id=spil,path=$SOCK,server=off,reconnect-ms=1000" "$SLOG"; SPID=$!

wait $SPID 2>/dev/null
kill $IPID 2>/dev/null

echo "================== SPI-LINK BACK-PRESSURE STRESS =================="
grep -aE 'STRESS:' "$SLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
if grep -aq 'STRESS:DONE' "$SLOG"; then
    echo "PASS: sender clocked $((ITERS * 2)) KB at a non-draining peer without a vCPU hang"
    exit 0
fi
die "sender hung under back-pressure (no STRESS:DONE) - spi-link tx blocked the vCPU"
