#!/usr/bin/env bash
#
# THE i.MX 91's ONLY TEMPERATURE SENSOR DID NOT WORK, AND NOTHING SAID SO.
#
# This machine mapped hw/misc/imx93_tmu.c -- the i.MX 93's TMR/TMSR/TIER register
# file -- at 4448_2000h. But the i.MX 91's device tree says "fsl,imx91-tmu", which
# binds drivers/thermal/imx91_thermal.c and speaks an entirely different block:
# CTRL0/STAT0/DATA0/CTRL1, and it ENABLES and STARTS the sensor exclusively through
# the SET/CLR aliases:
#
#     writel(CTRL1_EN,    base + CTRL1_SET);
#     writel(CTRL1_START, base + CTRL1_SET);
#
# The old model swallowed every one of those writes. DRDY never asserted, the 40 ms
# poll in get_temp() timed out, and the guest saw:
#
#     # cat /sys/class/thermal/thermal_zone0/temp
#     cat: read error: No data available
#
# The thermal zone registered. The driver bound. Nothing ever read a degree, and no
# test in this tree noticed -- because every test we own asks whether the model does
# what the model does.
#
#     A MODEL INHERITED FROM A NEIGHBOURING CHIP IS A MODEL OF THAT CHIP.
#
# So this test does not poke registers. It boots Linux, binds the real driver, and
# asks the guest for a temperature -- and it asserts a value the MODEL DOES NOT
# CHOOSE: the temperature is set from the command line, so a model that returns a
# plausible constant fails.
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

# An initrd that just asks the kernel what the die temperature is.
mkdir -p "$WORK/ird" && (cd "$WORK/ird" && zcat "$REPO/tests/busybox-imx91/busybox-imx91.cpio.gz" | cpio -idm 2>/dev/null)
cat > "$WORK/ird/init" <<'EOF'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
/bin/busybox --install -s /bin 2>/dev/null
echo "TMU-TYPE: $(cat /sys/class/thermal/thermal_zone0/type 2>&1)"
echo "TMU-TEMP: $(cat /sys/class/thermal/thermal_zone0/temp 2>&1)"
/bin/busybox poweroff -f
EOF
chmod +x "$WORK/ird/init"
(cd "$WORK/ird" && find . | cpio -o -H newc 2>/dev/null | gzip > "$WORK/ird.cpio.gz")

# ⭐ THE GOLDEN IS NOT THE MODEL'S OWN CONSTANT.
#
# (And the sweep earned its keep before the model did: the SHORT -global form,
#  `-global imx91.tmu.temperature=N`, mis-splits on the dots in the type name and
#  SILENTLY DOES NOTHING -- so every run quietly used the model's 25 C default. One
#  temperature would have looked like a pass. Three did not.)
# The die temperature is set on the command line, so a model that answers with some
# plausible built-in number -- or that "works" by always reporting ready -- fails.
probe() { # <millidegrees>
    timeout -s KILL 90 "$QEMU" -M imx91-11x11-evk -audio driver=none -smp 1 -m 1G -display none \
        -kernel "$KERNEL" -dtb "$DTB" -initrd "$WORK/ird.cpio.gz" \
        -global driver=imx91.tmu,property=temperature,value="$1" \
        -append "earlycon=lpuart32,mmio32,0x44380010 console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -serial mon:stdio -serial null 2>/dev/null \
        | grep -a '^TMU-TEMP:' | head -1 | sed 's/TMU-TEMP: *//' | tr -d '\r'
}

fail=0
for want in 25000 48500 -5000; do
    got=$(probe "$want")
    if [ "${got:-x}" = "$want" ]; then
        printf "  ok    die = %7s m°C  ->  guest reads %7s\n" "$want" "$got"
    else
        printf "  FAIL  die = %7s m°C  ->  guest reads '%s'\n" "$want" "${got:-<nothing>}"
        fail=1
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    echo "PASS: Linux binds fsl,imx91-tmu, enables the sensor through CTRL1_SET, polls"
    echo "      DRDY and reads back the die temperature we set -- including a negative"
    echo "      one, which a model that fakes a plausible constant cannot do."
else
    echo "FAIL: the guest cannot read the temperature we set."
    echo "      (Before hw/misc/imx91_tmu.c this read 'No data available' -- the"
    echo "       i.MX 91's only temperature sensor did not work at all.)"
fi
exit $fail
