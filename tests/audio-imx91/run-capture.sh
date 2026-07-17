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
ALSA_SYSROOT=${ALSA_SYSROOT:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/sysroots-components/armv8a-mx91/alsa-lib}
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
LOG="$TMP/console.log"
timeout -s KILL 400 "$QEMU" -M imx91-11x11-evk -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial mon:stdio -serial null "$@" > "$LOG" 2>&1
[ -n "${SAVE_CONSOLE:-}" ] && cp "$LOG" "$SAVE_CONSOLE"

# ⭐ ASSERT, don't print-and-hope.  The MICFIL card must register, capture real
# samples, AND feed at the PROGRAMMED rate -- a fixed-48kHz feed passes the
# non-silence check but fails the rate check.  If the oracle could not be built
# (no ALSA sysroot) this degrades to a card-registration check with a loud note.
grep -aqE '\[micfilaudio' "$LOG" \
    || { echo "FAIL: MICFIL card never registered"; tail -40 "$LOG"; exit 1; }
if ! grep -aq 'CAPDUR' "$LOG"; then
    echo "NOTE: capture oracle absent (no ALSA sysroot) -- card registered, rate NOT checked" >&2
    echo "SKIP: build the oracle (set ALSA_SYSROOT) to assert the capture rate" >&2
    exit 77
fi
grep -aqE 'CAP\[.*\]: PASS' "$LOG" \
    || { echo "FAIL: MICFIL capture silent/stuck"; grep -aE 'CAP\[' "$LOG"; exit 1; }
python3 - "$LOG" <<'EOF'
import re, sys
log = open(sys.argv[1], errors="replace").read()
d = {int(r): float(dur) for r, dur in
     re.findall(r'CAPDUR\[[^\]]+\]: rate=(\d+) frames=16000 dur=([\d.]+)', log)}
if 48000 not in d or 16000 not in d:
    print("FAIL: missing rate-timed CAPDUR (rate check did not run)"); sys.exit(1)
# Same 16000 frames both times: at the correct rate 16k takes ~0.67s longer
# than 48k (1.0s vs 0.33s of audio).  A fixed-48kHz feed makes the two equal.
diff = d[16000] - d[48000]
ok = diff > 0.45
print("PASS: MICFIL feed tracks the programmed rate" if ok else
      "FAIL: MICFIL feed IGNORES the programmed rate (fixed 48kHz?)")
print("  dur@48k=%.3fs dur@16k=%.3fs diff=%.3fs (expect ~0.67, fixed-rate bug ~0)"
      % (d[48000], d[16000], diff))
sys.exit(0 if ok else 1)
EOF
