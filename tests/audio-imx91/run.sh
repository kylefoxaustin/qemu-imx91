#!/bin/bash
# i.MX 91 SAI3/WM8962 audio playback test (real PCM, wav-capturable).
#
# Boots the base imx91-11x11-evk dtb, loads the ASoC drivers (incl. the
# snd-soc-wm8962 codec), and plays a generated square wave on the WM8962/SAI3
# card via the cross-compiled pcm_play oracle. The SAI3 TX FIFO drains the
# samples through cyclic eDMA; with QEMU's wav audio backend the played PCM is
# captured to a real .wav. Set WAV=out.wav to keep the capture; otherwise it is
# written to a temp file and its non-silence is asserted.
#
# pcm_play.c links against libasound (taken from the rootfs); set ALSA_SYSROOT
# to a sysroot containing usr/include/alsa (e.g. a BSP recipe-sysroot). Without
# the toolchain bits the test still checks card registration.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
CROSS=${CROSS:-aarch64-linux-gnu-}
ALSA_SYSROOT=${ALSA_SYSROOT:-}
WAV=${WAV:-}
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu-system-aarch64"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" "dtb"; need ROOTFS_TAR "$ROOTFS_TAR" "rootfs tar.zst"
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# Prefer libasound from the rootfs; fall back to the BSP-built one (the
# imx-image-core rootfs ships none) and inject it into the initramfs.
ALSA_LIBDIR=${ALSA_LIBDIR:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work/armv8a-mx91-poky-linux/alsa-lib/1.2.13/image/usr/lib}
( cd "$TMP" && zstd -dc "$(readlink -f "$ROOTFS_TAR")" | \
    tar -x 'usr/lib/libasound.so.2*' 2>/dev/null )
LASOUND=$(ls "$TMP"/usr/lib/libasound.so.2.* 2>/dev/null | head -1)
[ -n "$LASOUND" ] || LASOUND=$(ls "$ALSA_LIBDIR"/libasound.so.2.* 2>/dev/null | head -1)
if [ -n "$ALSA_SYSROOT" ] && command -v "${CROSS}gcc" >/dev/null && [ -n "$LASOUND" ]; then
    if "${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
            -o "$TMP/pcm_play" "$HERE/pcm_play.c" "$LASOUND" \
            -Wl,--allow-shlib-undefined 2>/dev/null; then
        echo "built pcm_play oracle (libasound: $LASOUND)"
    else
        echo "note: could not build pcm_play; card-registration check only" >&2
    fi
else
    echo "note: set ALSA_SYSROOT to build pcm_play; card-registration only" >&2
fi
fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$HERE/myinit' myinit && chmod 755 myinit
  if [ -f '$TMP/pcm_play' ]; then
      cp '$TMP/pcm_play' pcm_play && chmod 755 pcm_play
      mkdir -p usr/lib
      cp '$LASOUND' usr/lib/ && ln -sf \$(basename '$LASOUND') usr/lib/libasound.so.2
  fi
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"
# MUTED BY DEFAULT.  This test PLAYS PCM, and with AUDIO=() QEMU grabs whatever host
# backend it was built with (pulseaudio/sndio/alsa) -- i.e. the developer's speakers.
# Set WAV=<path> to capture the playback to a file instead; never to the host.
AUDIO=(-audio driver=none); [ -n "$WAV" ] && AUDIO=(-audio "driver=wav,path=$WAV")
set -x
exec "$QEMU" -M imx91-11x11-evk -m 4G -display none "${AUDIO[@]}" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null "$@"
