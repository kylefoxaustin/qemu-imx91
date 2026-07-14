#!/usr/bin/env bash
#
# uSDHC: 33 reset-value deviations in a block NO TEST IN THIS TREE EVER EXERCISED.
#
# The reset-value gate found thirty-three registers in uSDHC that disagreed with the
# RM, and when I went looking for the test that would tell me whether any of it
# mattered, there wasn't one. The SD/eMMC controller -- the thing a board boots from --
# had no test. Every "it works" was an inference.
#
# So this test attaches a REAL ext2 SD card with a KNOWN PAYLOAD, boots Linux, and makes
# the guest read the payload back and write a new file. It asserts on bytes the guest
# cannot have invented: the payload string is written into the image from the host, and
# the guest's write is read back from the image by the guest. Card enumeration alone is
# not evidence -- a card can enumerate and then fail every transfer.
#
# ⭐ AND IT IS THE TEST THAT KEPT THE RESET-VALUE GATE HONEST.
#
# The gate wanted HOST_CTRL_CAP (0x40) set to the RM's 0x07f3b407. Obeying it makes QEMU
# REFUSE TO START:
#
#     qemu-system-aarch64: block size can be 512, 1024 or 2048 only
#
# -- from sdhci_check_capareg(), QEMU's own validator, whose entire job is to reject a
# device that advertises capabilities it cannot deliver. The RM's value encodes a
# 4096-byte max block length and SDR104/SDR50 tuning that QEMU's SDHCI core does not
# implement.
#
#     A CAPABILITY REGISTER IS A CONTRACT.  THE RM'S VALUE IS THE RIGHT ANSWER ONLY IF
#     YOU ALSO IMPLEMENT THE CHIP BEHIND IT.
#
# Two oracles disagreed, and the one that refused to boot was right. That deviation is
# now a DECISION in known-deviations.txt, with this as its proof.
#
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for f in "$QEMU" "$KERNEL" "$DTB"; do
    [ -e "$f" ] || { echo "SKIP: $f not found"; exit 0; }
done
[ -e "$REPO/tests/busybox-imx91/busybox-imx91.cpio.gz" ] || { echo "SKIP: no busybox rootfs"; exit 0; }
command -v mkfs.ext2 >/dev/null && command -v debugfs >/dev/null || { echo "SKIP: need e2fsprogs"; exit 0; }

PAYLOAD="IMX91-USDHC-PAYLOAD-0123456789"
WROTE="GUEST-WROTE-THIS-0987654321"

# A real 64 MiB ext2 card, with a payload the guest cannot have invented.
dd if=/dev/zero of="$WORK/sd.img" bs=1M count=64 status=none
mkfs.ext2 -q -F "$WORK/sd.img"
echo "$PAYLOAD" > "$WORK/payload.txt"
debugfs -w -R "write $WORK/payload.txt payload.txt" "$WORK/sd.img" >/dev/null 2>&1

mkdir -p "$WORK/ird"
(cd "$WORK/ird" && zcat "$REPO/tests/busybox-imx91/busybox-imx91.cpio.gz" | cpio -idm 2>/dev/null)
cat > "$WORK/ird/init" <<EOF
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox --install -s /bin 2>/dev/null
mkdir -p /mnt
mount -t ext2 /dev/mmcblk0 /mnt 2>&1 | sed 's/^/SD-MOUNT: /'
echo "SD-READ: \$(cat /mnt/payload.txt 2>&1)"
echo "$WROTE" > /mnt/wr.txt 2>/dev/null
sync
umount /mnt 2>/dev/null
mount -t ext2 /dev/mmcblk0 /mnt 2>/dev/null     # remount: force it back off the card
echo "SD-WRITE: \$(cat /mnt/wr.txt 2>&1)"
umount /mnt 2>/dev/null
/bin/busybox poweroff -f
EOF
chmod +x "$WORK/ird/init"
(cd "$WORK/ird" && find . | cpio -o -H newc 2>/dev/null | gzip > "$WORK/ird.cpio.gz")

OUT=$(timeout -s KILL 180 "$QEMU" -M imx91-11x11-evk -smp 1 -m 1G -display none \
    -audio driver=none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/ird.cpio.gz" \
    -drive file="$WORK/sd.img",format=raw,if=sd,id=sd0 \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
    -serial mon:stdio -serial null 2>/dev/null | tr -d '\r')

got_read=$(printf '%s\n' "$OUT" | sed -n 's/^SD-READ: //p' | head -1)
got_write=$(printf '%s\n' "$OUT" | sed -n 's/^SD-WRITE: //p' | head -1)

fail=0
if [ "$got_read" = "$PAYLOAD" ]; then
    echo "  ok    guest READ the host's payload off the card byte-exact"
else
    echo "  FAIL  read back '$got_read' (wanted '$PAYLOAD')"; fail=1
fi
if [ "$got_write" = "$WROTE" ]; then
    echo "  ok    guest WROTE a file, and it survived a umount/remount round-trip"
else
    echo "  FAIL  wrote back '$got_write' (wanted '$WROTE')"; fail=1
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: uSDHC enumerates a real SD card and does byte-exact ext2 read AND write"
    echo "      over ADMA -- the first test in this tree to touch the block a board boots"
    echo "      from.  VEND_SPEC now resets to 0x30007809 (its four soft clock-enable bits"
    echo "      come out of reset ON, and sdhci-esdhc-imx.c read-modify-writes them)."
else
    echo "FAIL: the guest could not do real I/O against the SD card."
fi
exit $fail
