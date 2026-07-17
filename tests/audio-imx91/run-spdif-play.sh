#!/bin/bash
# i.MX 91 XCVR/SPDIF playback-RATE test.
#
# Boots the aud-hat dtb (which enables the xcvr node), loads the ASoC stack,
# and plays 1s of audio at 48k and 96k on the SPDIF card via the cross-compiled
# pcm_play oracle.  The XCVR TX word rate is taken from the SPDIF root clock, so
# both rates drain in ~1.0s; a model that clocks a fixed 48kHz drains the 96k
# stream ~2x too slow.  ASSERTS that -- a non-silence check would not see it.
#
# Companion to run.sh (SAI playback) and run-capture.sh (MICFIL capture).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk-aud-hat.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
MODULES_TGZ=${MODULES_TGZ:-$DEPLOY/modules-imx91evk.tgz}
CROSS=${CROSS:-aarch64-linux-gnu-}
ALSA_SYSROOT=${ALSA_SYSROOT:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/sysroots-components/armv8a-mx91/alsa-lib}
ALSA_LIBDIR=${ALSA_LIBDIR:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work/armv8a-mx91-poky-linux/alsa-lib/1.2.13/image/usr/lib}
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu-system-aarch64"; need KERNEL "$KERNEL" "kernel Image"
need DTB "$DTB" "aud-hat dtb"; need ROOTFS_TAR "$ROOTFS_TAR" "rootfs tar.zst"
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
LASOUND=$(ls "$ALSA_LIBDIR"/libasound.so.2.* 2>/dev/null | head -1)
skip() { echo "SKIP: $*  -- refusing to run without the PLAYBACK ORACLE" >&2; exit 77; }
[ -e "$ALSA_SYSROOT/usr/include/alsa/asoundlib.h" ] || skip "no ALSA headers at $ALSA_SYSROOT"
command -v "${CROSS}gcc" >/dev/null || skip "no cross compiler (${CROSS}gcc)"
[ -n "$LASOUND" ] || skip "no libasound.so.2 in the BSP"
"${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
        -o "$TMP/pcm_play" "$HERE/pcm_play.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null || skip "pcm_play would not compile"
echo "built pcm_play oracle (libasound: $LASOUND)"

fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$HERE/myinit-spdif' myinit && chmod 755 myinit
  # snd-soc-fsl-xcvr + imx-card are modules absent from imx-image-core; inject
  # the deploy modules tree.  --keep-directory-symlink so the merged-usr /lib
  # symlink survives (else the ELF interpreter path breaks).
  [ -f '$MODULES_TGZ' ] && tar --keep-directory-symlink -xzf '$MODULES_TGZ' 2>/dev/null
  cp '$TMP/pcm_play' pcm_play && chmod 755 pcm_play
  mkdir -p usr/lib
  cp '$LASOUND' usr/lib/ && ln -sf \$(basename '$LASOUND') usr/lib/libasound.so.2
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"

LOG="$TMP/console.log"
timeout -s KILL 400 "$QEMU" -M imx91-11x11-evk -m 4G -display none -audio driver=none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null > "$LOG" 2>&1
[ -n "${SAVE_CONSOLE:-}" ] && cp "$LOG" "$SAVE_CONSOLE"

grep -aqE '\[imxaudioxcvr|spdif' "$LOG" \
    || { echo "FAIL: SPDIF/XCVR card never registered"; tail -40 "$LOG"; exit 1; }
grep -aqE 'PLAY\[.*\]: PASS' "$LOG" \
    || { echo "FAIL: SPDIF playback did not complete"; grep -aE 'PLAY\[|set_params' "$LOG"; exit 1; }
python3 - "$LOG" <<'EOF'
import re, sys
log = open(sys.argv[1], errors="replace").read()
d = {int(r): float(dur) for r, dur in
     re.findall(r'PLAYDUR\[[^\]]+\]: rate=(\d+) frames=\d+ dur=([\d.]+)', log)}
if 48000 not in d or 96000 not in d:
    print("FAIL: missing PLAYDUR (rate check did not run)"); sys.exit(1)
# 1s of audio each: at the correct feed both take ~1.0s; a fixed-48kHz feed
# clocks the 96k stream out ~2x too slow (~2.0s).
ok = d[96000] < 1.5
print("PASS: XCVR SPDIF TX feeds at the programmed rate" if ok else
      "FAIL: XCVR SPDIF TX feeds a FIXED rate (48kHz?)")
print("  dur@48k=%.3fs dur@96k=%.3fs (96k ~1.0 tracking, ~2.0 fixed-rate bug)"
      % (d[48000], d[96000]))
sys.exit(0 if ok else 1)
EOF
