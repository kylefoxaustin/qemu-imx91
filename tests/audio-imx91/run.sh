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
#
# ⭐ A TEST WHOSE VERDICT IS READ BY A HUMAN IS NOT A TEST.  IT IS A DEMO WITH GOOD MANNERS.
#
# holobench found two of their labs that COULD NOT PASS -- they booted both boards, wired the
# bus, and there was nobody on the other end.  For weeks.  "A LAB THAT CANNOT PASS IS NOT A
# FAILING LAB.  IT IS A LAB NOBODY EVER RAN TO THE END -- and it sits in labs/ looking
# EXACTLY like the ones that work."
#
# This script was worse.  It had NO ASSERTION AT ALL: it exec'd QEMU and printed to the
# console.  The oracle (pcm_play) printed PASS *inside the guest*, and nothing outside the
# guest ever read it.  Every "verified with real PCM, 48000 frames, peak 8000" I have
# reported this week -- I WAS THE ASSERTION.  I grepped it myself, by hand, each time.  If
# the SAI regressed tomorrow, nothing in this tree would say so.
#
# And the oracle was CONDITIONAL: with ALSA_SYSROOT unset it printed a note to stderr and
# silently degraded to "card-registration check only" -- a run that looks exactly like a
# real one.  That is holobench's bug, verbatim.
#
# Now: the sysroot is auto-detected, a missing oracle is a LOUD SKIP (77) and never a quiet
# degrade, and the script ASSERTS -- on pcm_play's verdict AND on the bytes in the captured
# WAV, because a guest that prints PASS while clocking silence is exactly the failure this
# is here to catch.
#
ALSA_SYSROOT=${ALSA_SYSROOT:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/sysroots-components/armv8a-mx91/alsa-lib}
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
# THE ORACLE IS MANDATORY.  No oracle => no run.  A skip is loud (77) and never green.
skip() { echo "SKIP: $*  -- the PLAYBACK ORACLE cannot be built, so NOTHING would be" >&2
         echo "      tested.  Refusing to run a card-registration check and call it audio." >&2
         exit 77; }
[ -e "$ALSA_SYSROOT/usr/include/alsa/asoundlib.h" ] || skip "no ALSA headers at $ALSA_SYSROOT"
command -v "${CROSS}gcc" >/dev/null || skip "no cross compiler (${CROSS}gcc)"
[ -n "$LASOUND" ] || skip "no libasound.so.2 in the rootfs or the BSP"
"${CROSS}gcc" -O2 -Wall -I"$ALSA_SYSROOT/usr/include" \
        -o "$TMP/pcm_play" "$HERE/pcm_play.c" "$LASOUND" \
        -Wl,--allow-shlib-undefined 2>/dev/null || skip "pcm_play would not compile"
echo "built pcm_play oracle (libasound: $LASOUND)"
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
# ALWAYS capture to a wav -- it is BOTH the mute (the host backend is never touched) AND the
# evidence (we assert on the samples).  WAV=<path> just keeps the capture afterwards.
CAP=${WAV:-$TMP/capture.wav}
timeout -s KILL 300 "$QEMU" -M imx91-11x11-evk -m 4G -display none \
    -audio "driver=wav,path=$CAP" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null "$@" > "$TMP/console.log" 2>&1

fail=0
grep -aqE '^PLAY\[.*\]: PASS' "$TMP/console.log" \
    && echo "  ok    the guest's ALSA oracle reports PASS" \
    || { echo "  FAIL  pcm_play never reported PASS:"; fail=1
         grep -aE '^PLAY\[|^=== AUDIO|snd_pcm|error' "$TMP/console.log" | head -5 | sed 's/^/          /'; }

# ⭐ AND THE BYTES, because a guest that prints PASS while clocking SILENCE is exactly the
#    regression this exists to catch.  The oracle's word is not the oracle.
python3 - "$CAP" <<'EOF' || fail=1
import struct, sys
try:
    d = open(sys.argv[1], "rb").read()
except OSError:
    print("  FAIL  no wav was captured at all"); sys.exit(1)
pcm = d[44:]
s = struct.unpack("<%dh" % (len(pcm) // 2), pcm[:len(pcm) // 2 * 2])
nz = [x for x in s if x]
peak = max(map(abs, s)) if s else 0
print("  %s  captured PCM: %d samples, %d non-zero, peak %d"
      % ("ok  " if len(nz) > 1000 and peak > 100 else "FAIL", len(s), len(nz), peak))
sys.exit(0 if len(nz) > 1000 and peak > 100 else 1)
EOF

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: Linux binds the WM8962/SAI3 card, pcm_play clocks a real square wave through"
    echo "      the SAI TX FIFO over cyclic eDMA, and the captured PCM is non-silent."
else
    echo "FAIL: see above.  (Before this commit the script exec'd QEMU and asserted NOTHING --"
    echo "      the verdict was whatever a human happened to read off the console.)"
fi
exit $fail
