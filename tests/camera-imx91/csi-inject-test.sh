#!/usr/bin/env bash
#
# End-to-end byte-exact test for the i.MX 91 ISI host frame source - the
# "virtual camera". Feeds a sequence of host images into the parallel-CSI / ISI
# capture pipeline (no real sensor) and proves the exact bytes come out of
# /dev/video0.
#
# The ISI's "frames" property points at a directory of raw frames; the model
# scans each one out in turn (looping). This boots the mt9m114 camera device
# tree, generates three solid-colour frames at the negotiated geometry, runs the
# V4L2 capture oracle in the guest, and checks that each captured frame's FNV-1a
# hash equals the fed frame's hash - i.e. the CSI/ISI path carried real image
# data, not the built-in test pattern.
#
#   -global driver=imx93.isi,property=frames,value=<dir-of-*.raw>
#
# Frames must be RAW, packed width*height*bpp in the format the guest negotiates
# (the mt9m114 path settles on 1280x720, 3 bytes/pixel -> used=2764800). The
# shared imx93_isi model + this property are byte-identical with the i.MX 93's.
#
# Env: QEMU= KERNEL= DTB= ROOTFS_TAR= GEOM= CROSS=.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk-mt9m114.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
CROSS=${CROSS:-aarch64-linux-gnu-}
GEOM=${GEOM:-1280x720x3}        # WxHxbpp of the negotiated capture format

for f in "$QEMU" "$KERNEL" "$DTB" "$ROOTFS_TAR"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 2; }
done
command -v python3 >/dev/null || { echo "need python3" >&2; exit 2; }
command -v "${CROSS}gcc" >/dev/null || { echo "need ${CROSS}gcc" >&2; exit 2; }
command -v fakeroot >/dev/null || { echo "need fakeroot" >&2; exit 2; }

W=${GEOM%%x*}; rest=${GEOM#*x}; H=${rest%%x*}; BPP=${rest##*x}
FSZ=$((W * H * BPP))
TMP=$(mktemp -d); QPID=""
trap 'rm -rf "$TMP"; [ -n "$QPID" ] && kill -9 "$QPID" 2>/dev/null' EXIT
mkdir -p "$TMP/frames"

# Three solid-value frames + their expected FNV-1a (matches v4l2_cap's hash).
python3 - "$TMP/frames" "$FSZ" <<'PY' | tee "$TMP/expect.txt"
import sys
d, fsz = sys.argv[1], int(sys.argv[2])
def fnv(b):
    h = 0x811c9dc5
    for x in b: h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h
for i, v in enumerate((0x11, 0x22, 0x33)):
    open(f"{d}/frame{i:03d}.raw", "wb").write(bytes([v]) * fsz)
    print("EXPECT %08x" % fnv(bytes([v]) * fsz))
PY

# Cross-build the capture oracle and an initramfs from the BSP rootfs.
"${CROSS}gcc" -O2 -Wall -static -o "$TMP/v4l2_cap" "$HERE/v4l2_cap.c"
cat > "$TMP/myinit" <<'EOF'
#!/bin/sh
PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH
mount -t proc proc /proc 2>/dev/null; mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
sleep 3
echo "=== CSI CAP START ==="
/v4l2_cap cap /dev/video0
echo "=== CSI CAP DONE ==="
while true; do sleep 5; done
EOF
chmod +x "$TMP/myinit"
fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$TMP/myinit' myinit && chmod 755 myinit
  cp '$TMP/v4l2_cap' v4l2_cap && chmod 755 v4l2_cap
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"

echo "csi-inject: booting mt9m114 DTB, ISI frames=$TMP/frames"
"$QEMU" -M imx91-11x11-evk -audio driver=none -m 4G -display none \
    -global driver=imx93.isi,property=frames,value="$TMP/frames" \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$TMP/initrd.cpio.gz" \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/myinit ignore_loglevel" \
    -serial "file:$TMP/serial.log" -serial null >/dev/null 2>&1 &
QPID=$!
for _ in $(seq 1 90); do
    grep -aq "CSI CAP DONE" "$TMP/serial.log" 2>/dev/null && break
    kill -0 "$QPID" 2>/dev/null || { echo "qemu exited early"; exit 1; }
    sleep 2
done

expect=$(grep -aoE "EXPECT [0-9a-f]{8}" "$TMP/expect.txt" | awk '{print $2}')
cap=$(grep -aoE "CAMERA-CAP.*fnv=[0-9a-f]{8}" "$TMP/serial.log" | grep -aoE "fnv=[0-9a-f]{8}" | sed 's/fnv=//')
echo "--- fed frame hashes ---";      echo "$expect"
echo "--- captured frame hashes ---"; echo "$cap"

ok=0; total=0
for c in $cap; do
    total=$((total + 1))
    echo "$expect" | grep -qx "$c" && ok=$((ok + 1))
done
echo "csi-inject: $ok/$total captured frames byte-exact-match a fed frame"
[ "$total" -ge 3 ] && [ "$ok" = "$total" ] && { echo "PASS"; exit 0; }
echo "FAIL"; exit 1
