#!/bin/bash
# i.MX 91 V4L2 camera capture test (mt9m114 -> parallel-CSI -> ISI -> /dev/video0).
#
# Boots the imx91-11x11-evk-mt9m114 dtb, cross-compiles the static V4L2 capture
# oracle (v4l2_cap.c), stages it + /myinit into an initramfs built from the BSP
# imx-image-core rootfs, and captures frames. The sensor->pcsi link is disabled
# by default (as on real HW); the oracle enables it, propagates the sensor
# format down the chain, then streams MMAP buffers. The ISI model DMAs a moving
# test pattern into the ping-pong buffers, so a real V4L2 client gets real,
# changing frames. Expect: CAMERA-CAP[/dev/video0]: PASS (5/5 frames).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk-mt9m114.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
CROSS=${CROSS:-aarch64-linux-gnu-}
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu-system-aarch64"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" "mt9m114 dtb"; need ROOTFS_TAR "$ROOTFS_TAR" "rootfs tar.zst"
command -v "${CROSS}gcc" >/dev/null || { echo "error: ${CROSS}gcc not found" >&2; exit 1; }
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
"${CROSS}gcc" -O2 -Wall -static -o "$TMP/v4l2_cap" "$HERE/v4l2_cap.c"
fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$HERE/myinit' myinit && chmod 755 myinit
  cp '$TMP/v4l2_cap' v4l2_cap && chmod 755 v4l2_cap
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"
set -x
exec "$QEMU" -M imx91-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null "$@"
