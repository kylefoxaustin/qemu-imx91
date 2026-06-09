#!/bin/bash
# i.MX 91 SAI3/WM8962 audio *capture* trace harness.
#
# Boots the base imx91-11x11-evk dtb, loads the ASoC drivers, enables
# dynamic-debug on the wm8962 codec + fsl-sai + soc-core, then runs the
# pcm_capture oracle and dumps the kernel log so we can see exactly where the
# capture path stalls (why RCSR.RE is never set). Companion to run.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
MODULES_TGZ=${MODULES_TGZ:-$DEPLOY/modules-imx91evk.tgz}
CROSS=${CROSS:-aarch64-linux-gnu-}
ALSA_SYSROOT=${ALSA_SYSROOT:-}
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu-system-aarch64"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" "dtb"; need ROOTFS_TAR "$ROOTFS_TAR" "rootfs tar.zst"
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# Link against the BSP-built libasound (the core rootfs ships none); inject it.
ALSA_LIBDIR=${ALSA_LIBDIR:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work/armv8a-mx91-poky-linux/alsa-lib/1.2.13/image/usr/lib}
LASOUND=$(ls "$ALSA_LIBDIR"/libasound.so.2.* 2>/dev/null | head -1)
if [ -n "$ALSA_SYSROOT" ] && command -v "${CROSS}gcc" >/dev/null && [ -n "$LASOUND" ]; then
    ok=1
    for src in pcm_capture pcm_capdiag; do
        "${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
            -o "$TMP/$src" "$HERE/$src.c" "$LASOUND" \
            -Wl,--allow-shlib-undefined 2>&1 || ok=0
    done
    [ "$ok" = 1 ] && echo "built capture oracles (libasound: $LASOUND)" \
                  || echo "note: could not build capture oracles" >&2
else
    echo "note: need ALSA_SYSROOT + cross gcc + libasound; trace-only" >&2
fi
fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$HERE/myinit-capture' myinit && chmod 755 myinit
  # The MICFIL card driver (snd-soc-imx-card) + micfil/dmic are modules, absent
  # from imx-image-core; inject the deploy modules tree so they can modprobe.
  # --keep-directory-symlink: the tarball has a top-level lib/, but the rootfs
  # /lib is a merged-usr symlink; without this tar replaces it and breaks the
  # ELF interpreter path (binaries then fail with 'required file not found').
  [ -f '$MODULES_TGZ' ] && tar --keep-directory-symlink -xzf '$MODULES_TGZ' 2>/dev/null
  if [ -f '$TMP/pcm_capture' ]; then
      cp '$TMP/pcm_capture' pcm_capture && chmod 755 pcm_capture
      cp '$TMP/pcm_capdiag' pcm_capdiag && chmod 755 pcm_capdiag
      mkdir -p usr/lib
      cp '$LASOUND' usr/lib/ && ln -sf \$(basename '$LASOUND') usr/lib/libasound.so.2
  fi
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"
set -x
exec "$QEMU" -M imx91-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null "$@"
