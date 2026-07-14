#!/bin/bash
# i.MX 91 non-stock device-tree boot matrix (91emulator project).
#
# The imx91-11x11-evk machine is meant to run more than its namesake DTB: the
# BSP ships ~100 variant DTBs (other boards - FRDM, 9x9 QSB - and per-peripheral
# variants - mqs, i3c, 8mic, lpuart, flexspi-nand, panels, usbwifi). This test
# boots a representative spread to userspace and asserts no CPU abort.
#
# It also builds one *synthetic* DTB that points a driver-backed device at an
# address the SoC model does NOT implement (0x40010000) to prove the catch-all
# background region: the access is logged + reads 0 instead of data-aborting, so
# even a hand-edited / custom DTB boots. Needs a dtc (set DTC=) for that case;
# without one it is skipped with a note.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
TIMEOUT=${TIMEOUT:-80}
# Representative spread: other boards + peripheral variants. Override with $@.
MATRIX=("$@")
if [ ${#MATRIX[@]} -eq 0 ]; then
    MATRIX=(imx91-11x11-evk imx91-11x11-frdm imx91-11x11-frdm-imx91s
            imx91-9x9-qsb imx91-9x9-qsb-can1 imx91-11x11-evk-mqs
            imx91-11x11-evk-i3c imx91-11x11-evk-8mic-reve
            imx91-11x11-evk-lpuart imx91-11x11-evk-flexspi-nand-m2
            imx91-11x11-evk-tianma-wvga-panel imx91-11x11-evk-usbwifi)
fi
need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" "qemu-system-aarch64"; need KERNEL "$KERNEL" "kernel Image"
need ROOTFS_TAR "$ROOTFS_TAR" "rootfs tar.zst"
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
printf '#!/bin/sh\nmount -t proc proc /proc 2>/dev/null\nmount -t sysfs sys /sys 2>/dev/null\nmount -t devtmpfs dev /dev 2>/dev/null\necho "MATRIX-USERSPACE-OK model=[$(cat /sys/firmware/devicetree/base/model 2>/dev/null)] soc=[$(cat /sys/devices/soc0/soc_id 2>/dev/null)]"\necho MATRIX-DONE\n' > "$TMP/minit"
fakeroot bash -c "
  cd '$TMP' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  cp '$TMP/minit' minit && chmod 755 minit
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$TMP/initrd.cpio.gz'
"

# Synthetic 'custom DTB' with a device at an unmodeled address (catch-all proof).
if [ -x "$DTC" ] && [ -e "$DEPLOY/imx91-11x11-evk.dtb" ]; then
    "$DTC" -I dtb -O dts -o "$TMP/stock.dts" "$DEPLOY/imx91-11x11-evk.dtb" 2>/dev/null
    sed -e 's/watchdog@424a0000/watchdog@40010000/' \
        -e 's/reg = <0x424a0000 0x10000>/reg = <0x40010000 0x10000>/' \
        "$TMP/stock.dts" > "$TMP/custom.dts"
    # Enable that node (flip its status to okay).
    awk '/watchdog@40010000/{f=1} f&&/status = "disabled"/{sub(/disabled/,"okay");f=0} {print}' \
        "$TMP/custom.dts" > "$TMP/custom2.dts"
    if "$DTC" -f -I dts -O dtb -o "$TMP/custom.dtb" "$TMP/custom2.dts" 2>/dev/null; then
        MATRIX+=("@custom-unmodeled-addr")
    fi
fi

boot_one() {
    local name="$1" dtb="$2" log="$TMP/$1.log"
    timeout "$TIMEOUT" "$QEMU" -M imx91-11x11-evk -audio driver=none -m 4G -display none \
        -kernel "$KERNEL" -dtb "$dtb" -initrd "$TMP/initrd.cpio.gz" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/minit ignore_loglevel" \
        -serial mon:stdio -serial null >"$log" 2>/dev/null
    local us ab
    us=$(grep -c "MATRIX-USERSPACE-OK" "$log")
    ab=$(grep -ciE "Unhandled fault|external abort|Internal error|Oops|Synchronous Abort" "$log")
    if [ "$us" -ge 1 ] && [ "$ab" -eq 0 ]; then
        echo "PASS  $name  $(grep -m1 'MATRIX-USERSPACE-OK' "$log" | sed 's/MATRIX-USERSPACE-OK //')"
        return 0
    fi
    echo "FAIL  $name  userspace=$us aborts=$ab"
    grep -iE "panic|abort|fault|Oops" "$log" | tail -2 | sed 's/^/        /'
    return 1
}

fails=0
for m in "${MATRIX[@]}"; do
    if [ "$m" = "@custom-unmodeled-addr" ]; then
        boot_one "custom-unmodeled-addr" "$TMP/custom.dtb" || fails=$((fails + 1))
    else
        dtb="$DEPLOY/$m.dtb"
        [ -e "$dtb" ] || { echo "SKIP  $m (dtb missing)"; continue; }
        boot_one "$m" "$dtb" || fails=$((fails + 1))
    fi
done
echo "----------------------------------------"
echo "DTB matrix: $((${#MATRIX[@]} - fails))/${#MATRIX[@]} passed"
exit $((fails > 0 ? 1 : 0))
