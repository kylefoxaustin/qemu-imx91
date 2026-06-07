#!/usr/bin/env bash
#
# Audio bring-up check for the i.MX93 QEMU machine.
#
# The NXP BSP builds the ASoC stack as modules, so this chains a tiny overlay
# initramfs (/myinit, see ./myinit) onto the imx-image-core rootfs, modprobes
# the SAI/MICFIL drivers + codecs, and dumps /proc/asound. Expect:
#
#   0 [btscoaudio  ]: simple-card  - bt-sco-audio   (SAI1, playback+capture)
#   1 [wm8962audio ]: fsl-asoc-card - wm8962-audio  (SAI3 -> wm8962 codec)
#   wm8962 0-001a: customer id 0 revision A
#
# The wm8962 codec model on lpi2c1 + the SAI register/FIFO model let the card
# register. The SAI transmit-FIFO datapath is exercised kernel-free by
# tests/qtest/imx93-sai-test.c; full end-to-end PCM playback (cyclic eDMA3
# feeding the FIFO) is the remaining datapath work.
#
# Override any path via env:  KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk.dtb}
# imx-image-core rootfs cpio: needs /lib/modules (the ASoC drivers are =m).
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "device tree"
need BASE_INITRD "$BASE_INITRD" "base imx-image-core initramfs cpio.gz"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
install -m755 "$HERE/myinit" "$TMP/myinit"
( cd "$TMP" && echo myinit | cpio -o -H newc 2>/dev/null > overlay.cpio )
cat "$BASE_INITRD" "$TMP/overlay.cpio" > "$TMP/combined.cpio.gz"

set -x
exec "$QEMU" -M imx93-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/combined.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null
