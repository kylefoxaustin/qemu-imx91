#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Interconnect readiness (i.MX 91), ETHERNET link: prove two QEMU i.MX 91
# instances pass real data over their FEC ethernet, bridged by a QEMU socket
# netdev - the shape Holobench wires as an "ethernet segment" in a lab. A known
# payload travels guest A -> FEC -> socket bridge -> FEC -> guest B and is echoed
# back byte-exact (the linktool oracle), so this is real data movement, not just
# link-up.
#
# Topology: server instance binds `-nic socket,listen=`; client instance
# `-nic socket,connect=`. Each board's first -nic = FEC (eth0, the link); the
# second -nic user = EQOS (eth1, unused, just keeps it happy). Static IPs on
# eth0 (server .1 / client .2); no DHCP needed.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
INITRD_SRC=${INITRD_SRC:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
CROSS=${CROSS:-aarch64-linux-gnu-gcc}
PORT=${PORT:-12421}
SUBNET=${SUBNET:-192.168.7}
PAYLOAD=${PAYLOAD:-IMX91-ETH-LINK-payload-0123456789-abcdef}
MEM=${MEM:-2G}            # < ~1G starves the FEC DMA coherent pool -> abort
TMO=${TMO:-180}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD_SRC"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"

SPID=; CPID=
WORK=$(mktemp -d); trap 'rm -rf "$WORK"; kill ${SPID:-} ${CPID:-} 2>/dev/null' EXIT

# ---- build the linktool oracle (static aarch64) -----------------------------
"$CROSS" -O2 -static -o "$WORK/linktool" "$HERE/linktool.c" || die "linktool build failed"

# ---- stage a role-based initramfs (values injected via /linkenv) ------------
build_initrd() {            # $1=role  $2=our-ip  $3=peer-ip  -> echoes path
    local role=$1 myip=$2 peer=$3
    local stage="$WORK/$role"
    mkdir -p "$stage"
    zcat "$INITRD_SRC" | (cd "$stage" && cpio -idmu 2>/dev/null)
    install -m755 "$WORK/linktool" "$stage/linktool"
    printf 'ROLE=%s\nMYIP=%s\nPEER=%s\nPORT=%s\nPAYLOAD=%s\n' \
        "$role" "$myip" "$peer" "$PORT" "$PAYLOAD" > "$stage/linkenv"
    cat > "$stage/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
exec > /dev/console 2>&1
. /linkenv
ifconfig lo 127.0.0.1 up
ifconfig eth0 "$MYIP" netmask 255.255.255.0 up
echo "=== INTERCONNECT eth ($ROLE $MYIP -> peer $PEER) ==="
if [ "$ROLE" = server ]; then
    /linktool server "$PORT"
else
    /linktool client "$PEER" "$PORT" "$PAYLOAD"
fi
echo "=== INTERCONNECT-DONE ==="
/bin/busybox poweroff -f
INIT
    chmod +x "$stage/init"
    ( cd "$stage" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/$role.gz"
    echo "$WORK/$role.gz"
}

SRV_IRD=$(build_initrd server "$SUBNET.1" "$SUBNET.2")
CLI_IRD=$(build_initrd client "$SUBNET.2" "$SUBNET.1")

boot() {                    # $1=initrd  $2=nic-arg  $3=logfile
    timeout "$TMO" "$QEMU" -M imx91-11x11-evk -audio driver=none -smp 1 -m "$MEM" -display none \
        -kernel "$IMAGE" -dtb "$DTB" -initrd "$1" \
        -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
        -nic "$2" -nic user \
        -serial file:"$3" -serial null >/dev/null 2>&1 &
}

# ---- launch server (listener) first, then client (connector) ----------------
SLOG="$WORK/server.log"; CLOG="$WORK/client.log"
echo "== booting i.MX 91 SERVER (socket listen :$PORT) =="
boot "$SRV_IRD" "socket,listen=127.0.0.1:$PORT" "$SLOG"; SPID=$!
sleep 2   # let the listener bind before the connector dials
echo "== booting i.MX 91 CLIENT (socket connect :$PORT) =="
boot "$CLI_IRD" "socket,connect=127.0.0.1:$PORT" "$CLOG"; CPID=$!

wait $SPID $CPID 2>/dev/null

echo "================== ETHERNET LINK =================="
grep -aE 'INTERCONNECT|LINK:' "$SLOG" "$CLOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]' | sed 's#.*/##'
spass=$(grep -ac 'LINK:PASS:server' "$SLOG"); cpass=$(grep -ac 'LINK:PASS:client' "$CLOG")
if [ "$spass" -ge 1 ] && [ "$cpass" -ge 1 ]; then
    echo "PASS: payload crossed FEC<->socket<->FEC byte-exact between two i.MX 91 guests"
    exit 0
fi
echo "--- server tail ---"; grep -aE 'LINK:|INTERCONNECT' "$SLOG" | tail -3
echo "--- client tail ---"; grep -aE 'LINK:|INTERCONNECT' "$CLOG" | tail -3
die "ethernet link did not complete (server-pass=$spass client-pass=$cpass)"
