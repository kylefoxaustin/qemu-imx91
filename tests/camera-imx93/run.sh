#!/usr/bin/env bash
#
# Camera bring-up check for the i.MX93 QEMU machine.
#
# Boots the *mt9m114* device-tree variant (the parallel-camera path: MT9M114
# sensor -> parallel-CSI -> ISI), chains an overlay initramfs (/myinit) onto
# the imx-image-core rootfs, and dumps the V4L2 state. Expect a media device,
# four subdevs, and two ISI video nodes, with an empty deferred-probe list:
#
#   /dev/media0  /dev/v4l-subdev0..3  /dev/video0  /dev/video1
#   mt9m114 7-0048: chip found @ 0x90
#   mt9m114: MT9M114 is found
#
# No pixels are captured (no V4L2 capture backend) - the milestone is that the
# pipeline binds and the media graph registers.
#
# Override paths via env: KERNEL=/path/Image DTB=/path.dtb BASE_INITRD=...
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx93/tmp/deploy/images/imx93evk}

QEMU=${QEMU:-$REPO/build-imx93/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx93-11x11-evk-mt9m114.dtb}
BASE_INITRD=${BASE_INITRD:-$HOME/Documents/nxp/imx93-initramfs.cpio.gz}

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU        "$QEMU"        "qemu-system-aarch64 (build it first)"
need KERNEL      "$KERNEL"      "kernel Image"
need DTB         "$DTB"         "mt9m114 device tree (imx93-11x11-evk-mt9m114.dtb)"
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
