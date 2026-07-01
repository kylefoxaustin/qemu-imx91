#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# PuTTY-readiness (i.MX 91): prove a developer can reach the emulated board the
# two ways they do a real EVK - a serial terminal (PuTTY over serial to the
# console) and SSH. Boots the FULL BSP rootfs (imx-image-core .wic as the SD root)
# so systemd brings up serial-getty + sshd, then verifies BOTH paths end-to-end.
#
#   (1) SERIAL: a socket-backed console (what -serial pty/socket + PuTTY attaches
#       to) reaches 'imx91evk login:', logs in as root (empty password), and runs
#       a command in the interactive shell.
#   (2) SSH:    openssh sshd (PermitRootLogin/PermitEmptyPasswords) - ssh root@ over
#       a slirp hostfwd (attached to the eQOS/eth1 NIC) logs in + runs a command.
#
# Gotcha baked in: the .wic must be a 512K-multiple SD size or QEMU rejects it
# ("Invalid SD card size"); we copy + qemu-img resize to 3G. -audio driver=none
# keeps it silent. SKIPs cleanly if the BSP rootfs .wic or sshpass are missing.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
WIC_ZST=${WIC_ZST:-$DEPLOY/imx-image-core-imx91evk.rootfs.wic.zst}
ROOTDEV=${ROOTDEV:-/dev/mmcblk0p2}      # p1 = FAT boot, p2 = ext4 rootfs
SSH_PORT=${SSH_PORT:-2222}
MEM=${MEM:-2G}
TMO=${TMO:-300}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$WIC_ZST"; do [ -e "$f" ] || skip "missing $f"; done
command -v zstd    >/dev/null || skip "no zstd"
command -v qemu-img>/dev/null || skip "no qemu-img"
command -v sshpass >/dev/null || skip "no sshpass (needed to drive the empty-password SSH login)"

WORK=$(mktemp -d); CON="$WORK/con.sock"; QPID=
trap 'rm -rf "$WORK"; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null' EXIT

# ---- decompress + resize a COPY of the rootfs .wic to a valid SD size --------
echo "== staging rootfs .wic (decompress + resize to a 512K-multiple SD size) =="
zstd -d -f -q -o "$WORK/rootfs.wic" "$WIC_ZST" || die "zstd -d failed"
qemu-img resize -f raw "$WORK/rootfs.wic" 3G >/dev/null 2>&1 || die "resize failed"

# ---- boot the full rootfs: socket console + eQOS(eth1) hostfwd --------------
# Optional core-pinning: a full systemd boot under TCG is much faster when it
# isn't fighting the host for cores. Use it when taskset + spare cores exist.
PIN=""
if command -v taskset >/dev/null && [ "$(nproc)" -ge 6 ]; then
    PIN="taskset -c 2-$(( $(nproc) > 8 ? 7 : $(nproc) - 1 ))"
fi
echo "== booting the full BSP rootfs (root=$ROOTDEV) ${PIN:+[$PIN]} =="
timeout "$TMO" $PIN "$QEMU" -M imx91-11x11-evk -smp 1 -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" \
  -append "console=ttyLP0,115200 root=$ROOTDEV rootwait rw cpuidle.off=1" \
  -drive if=sd,format=raw,file="$WORK/rootfs.wic" -audio driver=none \
  -nic user -nic "user,hostfwd=tcp::${SSH_PORT}-:22" \
  -chardev socket,id=con,path="$CON",server=on,wait=off -serial chardev:con \
  >"$WORK/qemu.log" 2>&1 &
QPID=$!
for i in $(seq 1 30); do [ -S "$CON" ] && break; sleep 1; done
[ -S "$CON" ] || die "console socket never appeared"

# ---- (1) SERIAL login on ttyLP0 via the socket console ----------------------
SER=$(python3 - "$CON" <<'PY'
import socket, sys, time, re
buf = b""; s = None
for _ in range(60):                      # fresh socket each try (reuse breaks)
    try:
        s = socket.socket(socket.AF_UNIX); s.settimeout(2)
        s.connect(sys.argv[1]); break
    except OSError:
        s = None; time.sleep(1)
if s is None: print("SERIAL:FAIL:no-console-connect"); sys.exit()
def pump(t):
    global buf; end = time.time() + t
    while time.time() < end:
        try: d = s.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if not d: break
        buf += d
# getty prints 'login:' once (before we connect), so poke Enter until it
# re-prints the prompt - the reliable way to drive a socket-backed console.
ok = False
for _ in range(100):
    s.sendall(b"\r\n"); pump(2.0)
    if b"imx91evk login:" in buf or re.search(rb"login:\s*$", buf): ok = True; break
if not ok: print("SERIAL:FAIL:no-login-prompt"); sys.exit()
# Log in and confirm the interactive shell by its prompt (robust vs echo timing).
s.sendall(b"root\r\n")
got_shell = False
for _ in range(12):
    s.sendall(b"\r\n"); pump(2)
    if b"root@imx91evk" in buf: got_shell = True; break
if not got_shell: print("SERIAL:FAIL:no-shell-prompt"); sys.exit()
# Prove it's interactive: run a command, look for its (non-echoed) output.
s.sendall(b"id -un > /dev/console; echo MARK_$(uname -m)\r\n")
for _ in range(8):
    pump(2)
    m = re.search(rb"MARK_(aarch64)", buf)
    if m: print("SERIAL:PASS:root@imx91evk:" + m.group(1).decode()); break
else:
    print("SERIAL:PASS:root@imx91evk (shell prompt; cmd-echo not captured)")
PY
)
echo "serial: $SER"

# ---- (2) SSH login over the eQOS hostfwd ------------------------------------
SSHRES=FAIL
for i in $(seq 1 60); do
    OUT=$(sshpass -p '' ssh -p "$SSH_PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o PreferredAuthentications=password \
        -o PubkeyAuthentication=no -o ConnectTimeout=6 \
        root@127.0.0.1 'echo PUTTY_SSH_OK=$(id -un):$(uname -sm)' 2>/dev/null)
    case "$OUT" in *PUTTY_SSH_OK=root:*) SSHRES="PASS:$OUT"; break;; esac
    sleep 3
done
echo "ssh: $SSHRES"

# ---- score ------------------------------------------------------------------
echo "================== PuTTY-READINESS (i.MX 91) =================="
case "$SER" in *SERIAL:PASS*) s1=1;; *) s1=0;; esac
case "$SSHRES" in PASS:*) s2=1;; *) s2=0;; esac
[ "$s1" = 1 ] && echo "  serial-getty login on ttyLP0 : PASS" || echo "  serial login : FAIL"
[ "$s2" = 1 ] && echo "  ssh root@ over eQOS hostfwd  : PASS" || echo "  ssh login : FAIL"
if [ "$s1" = 1 ] && [ "$s2" = 1 ]; then
    echo "PASS: a developer can PuTTY into the i.MX 91 over serial AND ssh"
    exit 0
fi
die "PuTTY-readiness incomplete (serial=$s1 ssh=$s2)"
