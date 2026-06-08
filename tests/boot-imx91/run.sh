#!/bin/bash
# i.MX 91 EVK boot smoke test (91emulator project). Mirrors tests/boot-imx93.
#
# Boots the stock NXP BSP kernel + imx91-11x11-evk.dtb on the QEMU
# imx91-11x11-evk machine. With no initrd the kernel probes all peripherals
# then panics on "Unable to mount root fs" (expected) - that is the Path-C
# probe pass. Set INITRD=<cpio.gz> to reach userspace.
#
# NOTE: the machine is currently a bootstrap clone of the i.MX 93 (still
# carries the M33; run with -smp 3). After the single-core subtraction this
# drops to -smp 1.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
INITRD=${INITRD:-}
SMP=${SMP:-1}                              # i.MX 91 is single-core (1x A55)
MEM=${MEM:-4G}
CMDLINE=${CMDLINE:-"earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1"}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU   "$QEMU"   "qemu-system-aarch64 (build it first)"
need KERNEL "$KERNEL" "kernel Image"
need DTB    "$DTB"    "device tree imx91-11x11-evk.dtb"

INITRD_ARGS=()
if [ -n "$INITRD" ]; then
    need INITRD "$INITRD" "initramfs / rootfs cpio"
    INITRD_ARGS=(-initrd "$INITRD")
    CMDLINE="$CMDLINE rdinit=/init"
fi

set -x
exec "$QEMU" -M imx91-11x11-evk -smp "$SMP" -m "$MEM" -display none \
    -kernel "$KERNEL" -dtb "$DTB" "${INITRD_ARGS[@]}" \
    -append "$CMDLINE" \
    -serial mon:stdio -serial null \
    "$@"
