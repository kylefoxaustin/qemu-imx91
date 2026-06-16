#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a static-aarch64 BusyBox initramfs for the i.MX 91 functional test
# (tests/functional/aarch64/test_imx91_evk.py). The /init mounts proc/sys,
# prints a known marker once PID 1 reaches userspace, and powers off cleanly.
#
# No cross-compiler needed: it fetches the prebuilt static aarch64 busybox
# from Ubuntu's arm64 busybox-static package. Set BUSYBOX=/path/to/busybox to
# use a local static aarch64 binary instead (offline).
#
# Output: ./busybox-imx91.cpio.gz  (override with OUT=)
# The built artifact is hosted on the imx91-vN GitHub release and referenced
# by Asset(url, sha256) in the functional test.
set -eu

DEB_URL=${DEB_URL:-http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3.1_arm64.deb}
OUT=${OUT:-./busybox-imx91.cpio.gz}
MARKER=${MARKER:-IMX91 FUNCTIONAL TEST: userspace reached}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ -n "${BUSYBOX:-}" ] && [ -f "${BUSYBOX:-}" ]; then
    BB="$BUSYBOX"
else
    echo "fetching busybox-static (arm64) ..."
    wget -qO "$WORK/bb.deb" "$DEB_URL"
    dpkg-deb -x "$WORK/bb.deb" "$WORK/x"
    BB=$(find "$WORK/x" -name busybox -type f | head -1)
fi

# Verify it's an AArch64 ELF without depending on the `file` package: ELF
# e_machine is 2 bytes at offset 18, little-endian; EM_AARCH64 == 0xB7.
em=$(od -An -tx1 -j18 -N2 "$BB" | tr -d ' ')
[ "$em" = "b700" ] || { echo "not an aarch64 busybox (e_machine=$em): $BB" >&2; exit 1; }

root="$WORK/root"
mkdir -p "$root"/{bin,proc,sys,dev}
cp "$BB" "$root/bin/busybox"
cat > "$root/init" <<INIT
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
echo "=== imx91 busybox userspace ==="
uname -a
echo "processors: \$(grep -c '^processor' /proc/cpuinfo)"
echo "$MARKER"
/bin/busybox poweroff -f
INIT
chmod +x "$root/init"

( cd "$root" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$OUT"
echo "wrote $OUT ($(stat -c%s "$OUT") bytes)"
echo "sha256: $(sha256sum "$OUT" | cut -d' ' -f1)"
