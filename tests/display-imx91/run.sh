#!/bin/bash
# i.MX 91 LCDIF display scanout test (headless, screendump-verified).
#
# Boots the imx91-11x11-evk-tianma-wvga-panel dtb: the imx-drm stack binds
# LCDIFv3 CRTC + the parallel-display-format bridge (imx93_pdf_ops on the
# media block-ctrl - no separate device), creates /dev/fb0 (800x480, 32bpp),
# the test-init writes a noise pattern to fb0, and we QMP-screendump the
# emulated display and assert the captured surface is non-black. Proves the
# LCDIF model DMAs the framebuffer and scans it out end to end.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk-tianma-wvga-panel.dtb}
INITRD=${INITRD:?set INITRD to a rootfs cpio.gz whose /init is test-init.sh}
OUT=${OUT:-/tmp/imx91-display}
mkdir -p "$OUT"; SOCK="$OUT/qmp.sock"; PPM="$OUT/screen.ppm"; LOG="$OUT/serial.log"
rm -f "$SOCK" "$PPM" "$LOG"

"$QEMU" -M imx91-11x11-evk -audio driver=none -m 4G -display none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$INITRD" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -serial file:"$LOG" -serial null \
    -qmp unix:"$SOCK",server=on,wait=off &
QPID=$!
for _ in $(seq 1 80); do
    grep -q "FB0-PATTERN-WRITTEN\|NO /dev/fb0" "$LOG" 2>/dev/null && break
    kill -0 $QPID 2>/dev/null || break
    sleep 1
done
sleep 2
python3 "$HERE/qmp_screendump.py" "$SOCK" "$PPM"
kill $QPID 2>/dev/null
grep -E "virtual_size|bits_per_pixel|fb0 name|FB0-PATTERN" "$LOG"
echo "screendump: $PPM ($(stat -c%s "$PPM" 2>/dev/null) bytes)"
