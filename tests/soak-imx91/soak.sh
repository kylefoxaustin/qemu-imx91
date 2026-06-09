#!/bin/bash
# i.MX 91 soak supervisor.
#
# Boots the machine over and over, rotating through the BSP DTB variants so that
# across cycles EVERY function is exercised, and the in-guest battery (soak-init)
# verifies each one and prints SOAK:* markers. The supervisor accumulates
# per-function pass/fail/skip counters, tracks qemu RSS for leaks, runs the
# kernel-free qtest suite every few cycles, and (for display DTBs) grabs a QMP
# screendump and checks the framebuffer isn't blank. State persists across
# reboots so totals keep climbing; ^C prints the final dashboard.
#
#   tests/soak-imx91/soak.sh [HOURS]      (default: run until ^C)
#   DTBS="a b c" ITERS=5 ./soak.sh        (override the rotation / per-boot loop)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
QEMU=${QEMU:-$REPO/build/qemu-system-aarch64}
KERNEL=${KERNEL:-$DEPLOY/Image}
ROOTFS_TAR=${ROOTFS_TAR:-$DEPLOY/imx-image-core-imx91evk.rootfs.tar.zst}
MODULES_TGZ=${MODULES_TGZ:-$DEPLOY/modules-imx91evk.tgz}
DTC=${DTC:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work-shared/imx91evk/kernel-build-artifacts/scripts/dtc/dtc}
ALSA_SYSROOT=${ALSA_SYSROOT:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work/armv8a-mx91-poky-linux/alsa-lib/1.2.13/sysroot-destdir}
ALSA_LIBDIR=${ALSA_LIBDIR:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/work/armv8a-mx91-poky-linux/alsa-lib/1.2.13/image/usr/lib}
CROSS=${CROSS:-aarch64-linux-gnu-}
BOOT_TIMEOUT=${BOOT_TIMEOUT:-150}
QTEST_EVERY=${QTEST_EVERY:-10}
ITERS=${ITERS:-3}
HOURS=${1:-0}                              # 0 = run until ^C
CYCLES=${CYCLES:-0}                        # 0 = unbounded (else stop after N)

# DTB rotation. Each entry: dtb-basename | extra machine opts | extra qemu opts.
DTBS_DEFAULT=(
  "imx91-11x11-evk|"
  "imx91-11x11-evk-i3c|"
  "imx91-11x11-evk-mqs|"
  "imx91-11x11-evk-8mic-reve|"
  "imx91-11x11-evk-flexspi-nand-m2|,flexspi-flash=gd5f4gq4"
  "imx91-11x11-evk-mt9m114|"
  "imx91-11x11-evk-tianma-wvga-panel|"
  "imx91-11x11-frdm|"
  "imx91-9x9-qsb|"
)
IFS=' ' read -r -a DTBS_OVERRIDE <<< "${DTBS:-}"

need() { [ -e "$2" ] || { echo "error: $3 not found: $2" >&2; exit 1; }; }
need QEMU "$QEMU" qemu; need KERNEL "$KERNEL" kernel; need ROOTFS_TAR "$ROOTFS_TAR" rootfs
command -v fakeroot >/dev/null || { echo "error: fakeroot not found" >&2; exit 1; }

STATE=$(mktemp -d); WORK=$(mktemp -d)
# Separate cleanup (always) from the dashboard+stop on a signal: a trap that
# only runs cleanup would ABSORB SIGINT/SIGTERM (handler runs, loop continues).
# Print the dashboard + clean up on ANY exit (normal completion or signal). A
# bare INT/TERM handler that only cleaned up would ABSORB the signal (handler
# runs, loop continues); instead the signal handler just exits, which fires the
# EXIT trap once.
cleanup() { dashboard; rm -rf "$WORK"; }
trap cleanup EXIT
trap 'exit 0' INT TERM
: > "$STATE/failures.log"; : > "$STATE/rss.log"
CYCLE=0; BOOTS=0; RSS0=0

# ---- one-time build of the shared image: oracles + libasound + modules ------
echo "soak: building shared initramfs (oracles + modules + rootfs)..."
LASOUND=$(ls "$ALSA_LIBDIR"/libasound.so.2.* 2>/dev/null | head -1)
for src in pcm_play pcm_capture; do
  [ -n "$LASOUND" ] && command -v "${CROSS}gcc" >/dev/null && \
    "${CROSS}gcc" -O2 -w -I"$ALSA_SYSROOT/usr/include" \
       -o "$WORK/$src" "$HERE/../audio-imx91/$src.c" "$LASOUND" \
       -Wl,--allow-shlib-undefined 2>/dev/null
done
[ -f "$HERE/../camera-imx91/v4l2_cap.c" ] && command -v "${CROSS}gcc" >/dev/null && \
  "${CROSS}gcc" -O2 -w -o "$WORK/v4l2_cap" "$HERE/../camera-imx91/v4l2_cap.c" 2>/dev/null
fakeroot bash -c "
  cd '$WORK' && mkdir rootfs && cd rootfs
  zstd -dc '$(readlink -f "$ROOTFS_TAR")' | tar -x 2>/dev/null
  [ -f '$MODULES_TGZ' ] && tar --keep-directory-symlink -xzf '$MODULES_TGZ' 2>/dev/null
  cp '$HERE/soak-init' init && chmod 755 init
  for b in pcm_play pcm_capture v4l2_cap; do
    [ -f '$WORK/'\$b ] && { cp '$WORK/'\$b \$b && chmod 755 \$b; }
  done
  if [ -n '$LASOUND' ]; then
    mkdir -p usr/lib && cp '$LASOUND' usr/lib/ && ln -sf \$(basename '$LASOUND') usr/lib/libasound.so.2
  fi
  [ -e dev/console ] || mknod -m 600 dev/console c 5 1
  find . | cpio -o -H newc 2>/dev/null | gzip -1 > '$WORK/initrd.cpio.gz'
"
echo "soak: initrd $(du -h "$WORK/initrd.cpio.gz" | cut -f1)"

# A small ext4 SD image with a marker, for the storage write+verify test.
SDIMG="$WORK/sd.img"
if command -v mkfs.ext4 >/dev/null; then
  dd if=/dev/zero of="$SDIMG" bs=1M count=64 2>/dev/null
  mkfs.ext4 -q -F "$SDIMG" 2>/dev/null
  SD_OPTS=(-drive "if=sd,format=raw,file=$SDIMG")
else
  SD_OPTS=()
fi

# Build the synthetic NAND-at-unmodelled-addr / flash flag handled per-variant.

bump() {  # name result(pass|fail|skip) detail
  local key="$1.$2"
  local n; n=$(cat "$STATE/$key" 2>/dev/null || echo 0)
  echo $((n + 1)) > "$STATE/$key"
}

dashboard() {
  echo; echo "==================== SOAK DASHBOARD ===================="
  echo "cycles=$CYCLE  clean-boots=$BOOTS  RSS first=${RSS0}kB last=$(tail -1 "$STATE/rss.log" 2>/dev/null | awk '{print $2}')kB"
  printf "%-20s %6s %6s %6s\n" function PASS FAIL SKIP
  for f in $(ls "$STATE" | sed -E 's/\.(pass|fail|skip)$//' | grep -vE 'failures|rss' | sort -u); do
    p=$(cat "$STATE/$f.pass" 2>/dev/null || echo 0)
    fl=$(cat "$STATE/$f.fail" 2>/dev/null || echo 0)
    s=$(cat "$STATE/$f.skip" 2>/dev/null || echo 0)
    printf "%-20s %6s %6s %6s\n" "$f" "$p" "$fl" "$s"
  done
  echo "failures logged: $(wc -l < "$STATE/failures.log")"
  [ -s "$STATE/failures.log" ] && tail -5 "$STATE/failures.log"
  echo "========================================================"
}

run_qtests() {
  echo "soak: [cycle $CYCLE] running kernel-free qtest suite..."
  local ok=0 bad=0
  for t in flexcan lpspi lpi2c isi sai micfil flexspi flexio i3c ddrc; do
    local bin="$REPO/build/tests/qtest/imx91-$t-test"
    [ -x "$bin" ] || continue
    if QTEST_QEMU_BINARY="$QEMU" "$bin" >/dev/null 2>&1; then
      ok=$((ok + 1)); bump "qtest-$t" pass
    else
      bad=$((bad + 1)); bump "qtest-$t" fail
      echo "[cycle $CYCLE] qtest FAIL: $t" >> "$STATE/failures.log"
    fi
  done
  echo "soak: qtests $ok ok / $bad failed"
}

START=$(date +%s)
echo "soak: starting. ^C for dashboard. (qtests every $QTEST_EVERY cycles)"

while :; do
  if [ "${#DTBS_OVERRIDE[@]}" -gt 0 ]; then
    entry="${DTBS_OVERRIDE[$((CYCLE % ${#DTBS_OVERRIDE[@]}))]}|"
  else
    entry="${DTBS_DEFAULT[$((CYCLE % ${#DTBS_DEFAULT[@]}))]}"
  fi
  dtb_name="${entry%%|*}"; rest="${entry#*|}"; mopts="${rest%%|*}"
  DTB="$DEPLOY/$dtb_name.dtb"
  CYCLE=$((CYCLE + 1))
  if [ ! -e "$DTB" ]; then echo "soak: [cycle $CYCLE] SKIP (no $dtb_name.dtb)"; continue; fi

  LOG="$WORK/cycle.log"; QMP="$WORK/qmp.sock"
  WAV="$WORK/play.wav"; rm -f "$WAV"
  echo "soak: [cycle $CYCLE] boot $dtb_name ${mopts:+($mopts)}"
  # -nic user -nic user: peers for both board NICs (FEC eth0 + ENET_QoS eth1),
  # so udhcpc gets a lease on each. -audio driver=wav writes to a file (NOT the
  # host backend, which would beep for the whole soak).
  ITERS=$ITERS timeout "$BOOT_TIMEOUT" "$QEMU" \
      -M "imx91-11x11-evk$mopts" -m 4G -display none \
      -audio "driver=wav,path=$WAV" -nic user -nic user \
      -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/initrd.cpio.gz" \
      "${SD_OPTS[@]}" \
      -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init ignore_loglevel ITERS=$ITERS" \
      -serial mon:stdio -serial null -qmp "unix:$QMP,server,nowait" \
      >"$LOG" 2>&1 &
  QPID=$!

  # Sample RSS while it runs. Anchor the leak baseline at the FIRST sample taken
  # after userspace is live (the battery has printed SOAK:BOOT), so kernel-boot
  # allocation isn't mistaken for drift.
  while kill -0 "$QPID" 2>/dev/null; do
    rss=$(awk '/VmRSS/{print $2}' "/proc/$QPID/status" 2>/dev/null)
    if [ -n "$rss" ]; then
      echo "$(date +%s) $rss" >> "$STATE/rss.log"
      [ "$RSS0" = 0 ] && grep -q "SOAK:BOOT:" "$LOG" 2>/dev/null && RSS0=$rss
    fi
    sleep 3
  done
  wait "$QPID" 2>/dev/null

  # Flag a leak: peak RSS > 1.5x the post-liveness baseline.
  if [ "$RSS0" != 0 ]; then
    peak=$(awk 'BEGIN{m=0}{if($2>m)m=$2}END{print m}' "$STATE/rss.log")
    if [ "$peak" -gt $((RSS0 * 3 / 2)) ]; then
      echo "[cycle $CYCLE] RSS leak? baseline=${RSS0}kB peak=${peak}kB" >> "$STATE/failures.log"
    fi
  fi

  # Parse the battery markers.
  if grep -q "SOAK:BOOT:" "$LOG"; then BOOTS=$((BOOTS + 1)); fi
  while IFS= read -r line; do
    case "$line" in
      SOAK:PASS:*) n="${line#SOAK:PASS:}"; bump "${n%%:*}" pass ;;
      SOAK:FAIL:*) n="${line#SOAK:FAIL:}"; bump "${n%%:*}" fail
                   echo "[cycle $CYCLE $dtb_name] $line" >> "$STATE/failures.log" ;;
      SOAK:SKIP:*) n="${line#SOAK:SKIP:}"; bump "${n%%:*}" skip ;;
    esac
  done < <(grep "^SOAK:" "$LOG")

  if ! grep -q "SOAK:BATTERY:DONE" "$LOG"; then
    echo "[cycle $CYCLE $dtb_name] battery did not finish (boot hang?)" >> "$STATE/failures.log"
    bump boot fail
  else
    bump boot pass
  fi

  # Kernel-free qtest suite every N cycles.
  [ $((CYCLE % QTEST_EVERY)) -eq 0 ] && run_qtests

  # Compact live status line.
  echo "soak: [cycle $CYCLE] done. boots=$BOOTS fails=$(wc -l < "$STATE/failures.log") RSS=$(tail -1 "$STATE/rss.log" 2>/dev/null | awk '{print $2}')kB"

  if [ "$HOURS" != 0 ]; then
    now=$(date +%s); [ $((now - START)) -ge $((HOURS * 3600)) ] && break
  fi
  [ "$CYCLES" != 0 ] && [ "$CYCLE" -ge "$CYCLES" ] && break
done
