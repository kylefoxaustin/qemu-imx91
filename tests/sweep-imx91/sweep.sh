#!/bin/bash
# i.MX 91 code-sweep supervisor.
#
# Cross-builds a corpus of real third-party code (each with source + a defined
# oracle), stages the static test binaries into a one-shot initramfs over the
# stock imx-image-core rootfs, boots the machine ONCE, and scores the SOAK:
# markers the in-guest battery prints. Same marker grammar + scorer as the soak
# harness, so the dashboards are diff-able across the 91/93/95 fleet.
#
#   tests/sweep-imx91/sweep.sh [name ...]     (no names => whole corpus)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$HERE/scoreboard.sh"

DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-1200}      # whole-sweep wall clock guard
ITEM_TIMEOUT=${ITEM_TIMEOUT:-120}       # per-routine guard inside the guest

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" qemu; need KERNEL "$KERNEL" kernel
need DTB "$DTB" dtb; need ROOTFS "$ROOTFS_TAR" rootfs
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

WORK=$(mktemp -d)
STATE="$WORK/state"; mkdir -p "$STATE"; : > "$STATE/failures.log"
trap 'dashboard; rm -rf "$WORK"' EXIT
trap 'exit 0' INT TERM

# 1) cross-build the corpus into a staging tree
STAGE="$WORK/stage"
echo "sweep: building corpus..."
bash "$HERE/build-corpus.sh" "$STAGE" "$@" || { echo "sweep: corpus build aborted"; exit 1; }

# 2) assemble the one-shot initramfs: stock rootfs + /sweep overlay + /init
echo "sweep: assembling initramfs..."
fakeroot bash -c "
  cd '$WORK' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  mkdir -p sweep
  cp -a '$STAGE'/. sweep/
  cp '$HERE/sweep-init' init && chmod 755 init
  find sweep -name run.sh -exec chmod 755 {} +
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$WORK/initrd.cpio.gz'
"
echo "sweep: initrd $(du -h "$WORK/initrd.cpio.gz" | cut -f1)"

# 3) boot once, capture the console (written live to .last-boot.log so a long
#    run can be monitored in-flight: tail -f tests/sweep-imx91/.last-boot.log)
LOG="$HERE/.last-boot.log"; : > "$LOG"
echo "sweep: booting (boot-timeout ${BOOT_TIMEOUT}s, per-item ${ITEM_TIMEOUT}s)..."
timeout "$BOOT_TIMEOUT" "$QEMU" \
    -M imx91-11x11-evk -m 4G -display none -audio driver=none \
    -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
    -nic user \
    -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init ignore_loglevel ITEM_TIMEOUT=$ITEM_TIMEOUT ${EXTRA_APPEND:-}" \
    -serial mon:stdio -serial null \
    >"$LOG" 2>&1
qrc=$?

# 4) score (console already preserved live in .last-boot.log)
score_log "$LOG" sweep
if ! grep -q "SOAK:BATTERY:DONE" "$LOG"; then
  echo "[sweep] battery did not finish (qemu rc=$qrc, boot hang or panic?)" >> "$STATE/failures.log"
  bump boot fail
  echo "sweep: WARNING battery did not finish — last console lines:"
  tail -15 "$LOG" | sed 's/^/  | /'
else
  bump boot pass
  echo "sweep: battery finished."
fi
# dashboard prints via the EXIT trap
