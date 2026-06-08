#!/bin/bash
# i.MX 91 functional smoke test: boots the stock BSP kernel to userspace and
# exercises the kept subsystems end-to-end (data flows, not just "driver bound"):
#   - uSDHC storage: mount a populated ext4 SD image, read marker, write+sync
#   - I2C: enumerate adapters + scan (WM8962 audio codec answers at 0x1a)
#   - FEC ethernet: bring up eth0 + udhcpc (slirp lease 10.0.2.15)
#   - dmesg: kept-subsystem bind/health
#
# Networking note: use -nic (populates nd_table so the board NICs get a peer),
# NOT bare -netdev. SD image must be a power-of-2 size; build with mke2fs -d
# under fakeroot and -O ^metadata_csum (see the 5 rootfs gotchas).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
INITRD=${INITRD:?set INITRD to a rootfs cpio.gz whose /init is test-init.sh}
SD=${SD:?set SD to a power-of-2 ext4 image with /marker.txt}

set -x
exec "$QEMU" -M imx91-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -drive if=sd,format=raw,file="$SD",index=0 \
    -nic user -nic user \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -serial mon:stdio -serial null "$@"
