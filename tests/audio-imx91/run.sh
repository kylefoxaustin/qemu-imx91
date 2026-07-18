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
# ⭐ ASSERT THE PITCH AND DURATION, AT MORE THAN ONE RATE.
#
# pcm_play toggles the square wave every 55 frames, so the tone is rate/110 Hz and the
# clip is 1 second long AT THE RATE THAT IS ACTUALLY CLOCKED.  A model that pms at a
# fixed 48 kHz plays a 16 kHz stream 3x too fast: 436 Hz and 0.33 s instead of 145 Hz
# and 1.0 s.  So we play at BOTH 48000 and 16000 and check the pitch and length of each.
#
#   A DEFAULT THAT EQUALS THE ANSWER IS NOT AN ANSWER.  The old test only played 48 kHz
#   and only checked "non-silent", so it was green on a model that could not track a rate.
#
fail=0
for want_rate in 48000 16000; do
    cap="$TMP/cap-$want_rate.wav"
    timeout -s KILL 300 "$QEMU" -M imx91-11x11-evk -m 4G -display none \
        -audio "driver=wav,path=$cap,out.frequency=$want_rate" \
        -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel pcm.rate=$want_rate ${QEMU_APPEND:-}" \
        -serial mon:stdio -serial null "$@" > "$TMP/console-$want_rate.log" 2>&1
    [ -n "${SAVE_CONSOLE:-}" ] && cp "$TMP/console-$want_rate.log" "$SAVE_CONSOLE.$want_rate"

    grep -aqE '^PLAY\[.*\]: PASS' "$TMP/console-$want_rate.log" \
        || { echo "  FAIL  ${want_rate}Hz: pcm_play never reported PASS"; fail=1; continue; }

    # tone = rate/110, duration ~= 1.0s -- measured from the captured PCM, model-independent.
    #
    # ⭐ AND A STRUCTURAL CHECK THAT CATCHES SAMPLE LOSS, WHICH TONE+DURATION CANNOT.
    #
    # pcm_play toggles the square wave every 55 FRAMES, so with a rate-matched capture
    # (out.frequency=want_rate above -- no resample) every interior half-period is
    # EXACTLY 55 samples.  A dropped sample shortens a run to 54; a scattered ~0.4%
    # loss (the fleet's SAI audio_cb bug #2) turns ~20% of the runs off-55.  This is
    # catchable where the count is NOT: the total-frame count jitters by a whole ALSA
    # period (+/-1024) run-to-run, burying a 176-frame loss -- but the run STRUCTURE is
    # deterministic and host-independent.  (A statistical peak/tone/duration oracle
    # feels a 0.4% scattered loss not at all; 93emulator's screendump-mean oracle had
    # the same blind spot.)  Requires the non-resampled capture: on a resampled wav the
    # runs smear to 50/51 and the check is meaningless -- which is the whole point.
    python3 - "$cap" "$want_rate" <<'EOF' || fail=1
import struct, sys
cap, want = sys.argv[1], int(sys.argv[2])
d = open(cap, "rb").read()
wr = struct.unpack("<I", d[24:28])[0] if d[:4] == b"RIFF" else 0
pcm = d[44:]
s = struct.unpack("<%dh" % (len(pcm)//2), pcm[:len(pcm)//2*2])
mono = s[0::2]
if wr == 0 or len(mono) < 1000:
    print("  FAIL  %dHz: no usable capture" % want); sys.exit(1)
xz = sum(1 for i in range(1, len(mono)) if (mono[i-1] < 0) != (mono[i] < 0))
dur = len(mono) / wr
tone = xz / 2 / dur
want_tone = want / 110.0
tone_ok = abs(tone - want_tone) < want_tone * 0.15
dur_ok = abs(dur - 1.0) < 0.2
# Run-length structure: every interior half-period must be exactly 55 frames.
runs = []; cur = 0; sign = None
for v in mono:
    sg = 1 if v > 0 else (-1 if v < 0 else 0)
    if sg == 0:
        continue
    if sg == sign:
        cur += 1
    else:
        if sign is not None:
            runs.append(cur)
        cur = 1; sign = sg
if sign is not None:
    runs.append(cur)
interior = runs[1:-1]            # drop the first/last partial half-periods
off = [r for r in interior if r != 55]
struct_ok = len(interior) > 100 and len(off) < 0.02 * len(interior)
print("  %s  %dHz -> tone %.0fHz (want ~%.0f), duration %.2fs (want ~1.0), "
      "%d half-periods %d off-55 (%.1f%%)"
      % ("ok  " if tone_ok and dur_ok and struct_ok else "FAIL", want, tone,
         want_tone, dur, len(interior), len(off),
         100 * len(off) / max(1, len(interior))))
sys.exit(0 if tone_ok and dur_ok and struct_ok else 1)
EOF
done

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: the SAI/wm8962 card plays a real square wave at the CONFIGURED rate -- pitch"
    echo "      and duration correct at BOTH 48 kHz and 16 kHz.  The rate is taken from the"
    echo "      codec (wm8962 R27), not assumed: a fixed-48kHz model plays 16 kHz 3x too fast"
    echo "      and fails the 16 kHz pitch/duration check."
else
    echo "FAIL: see above.  (A model that hardcodes 48 kHz fails the 16 kHz case:"
    echo "      436 Hz / 0.33 s instead of 145 Hz / 1.0 s.)"
fi
exit $fail
