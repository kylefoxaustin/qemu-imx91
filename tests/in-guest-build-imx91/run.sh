#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# In-guest build tier (i.MX 91): prove the emulated A55 can HOST a C compiler and
# RUN the binaries it produces - a developer can build their code ON the board,
# not just cross-compile it on a host. Strongest self-hosting signal for the
# board farm (tests/sweep-imx91 cross-compiles on the host then runs in-guest;
# this compiles *and* runs in-guest).
#
# Adapted from the i.MX 95 in-guest-build harness. The 91 delta: NO System
# Manager / M33 (single Cortex-A55), so there is NO `-device loader,...m33.elf`;
# the boot is the plain imx91-11x11-evk tuple used by tests/boot-imx91.
#
# MVP toolchain = TinyCC (native aarch64, runs in the guest) + musl libc (static),
# built FROM SOURCE (no binaries committed). Gotchas (each cost a boot), baked in:
#   - cross-building native-aarch64 tcc: the c2str build-helper must be built with
#     the HOST cc (it runs during the build), not the target cc.
#   - libtcc1.a (tcc's runtime) is normally built by running tcc; since our tcc is
#     an aarch64 binary we compile the arm64 runtime objects with the cross cc.
#   - BOTH musl and libtcc1.a must be -fno-stack-protector (Ubuntu gcc defaults to
#     -fstack-protector-strong; tcc links libtcc1 after libc, so __stack_chk_* go
#     unresolved otherwise).
#   - tcc's crt prefix is the TRIPLET libdir, so musl's crt1.o/crti.o/crtn.o +
#     libc.a are staged in /usr/lib/<triplet>; headers in /usr/include.
#   - the busybox initramfs is static-only (no loader), so we link -static.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
QEMU=${QEMU:-$ROOT/build/qemu-system-aarch64}
DEPLOY=${DEPLOY:-$HOME/Documents/nxp/linux/imx-yocto-bsp/build-imx91/tmp/deploy/images/imx91evk}
IMAGE=${IMAGE:-${KERNEL:-$DEPLOY/Image}}
DTB=${DTB:-$DEPLOY/imx91-11x11-evk.dtb}
INITRD=${INITRD:-$ROOT/tests/busybox-imx91/busybox-imx91.cpio.gz}
CROSS=${CROSS:-${CC:-aarch64-linux-gnu-gcc}}
HOSTCC=${HOSTCC:-cc}
AR=${AR:-aarch64-linux-gnu-ar}
SMP=${SMP:-1}                              # i.MX 91 is single-core (1x A55)
MEM=${MEM:-2G}
TMO=${TMO:-180}
MUSL_VER=${MUSL_VER:-1.2.5}
TCC_GIT=${TCC_GIT:-https://repo.or.cz/tinycc.git}
CACHE=${CACHE:-$ROOT/build/in-guest-build}

skip() { echo "SKIP: $*"; exit 0; }
die()  { echo "FAIL: $*"; exit 1; }
for f in "$QEMU" "$IMAGE" "$DTB" "$INITRD"; do [ -e "$f" ] || skip "missing $f"; done
command -v "$CROSS" >/dev/null || skip "no cross compiler ($CROSS)"
command -v "$HOSTCC" >/dev/null || skip "no host cc ($HOSTCC)"
command -v git >/dev/null || skip "no git"; command -v curl >/dev/null || skip "no curl"

TRIPLET=arm64-linux-gnu
mkdir -p "$CACHE"

# ---- build native-aarch64 tcc (runs in the guest, emits arm64) --------------
TCC=$CACHE/tinycc
if [ ! -x "$TCC/tcc" ]; then
    echo "== building native-aarch64 tcc =="
    [ -d "$TCC" ] || git clone --depth 1 "$TCC_GIT" "$TCC" >/dev/null 2>&1 || skip "tcc clone failed"
    ( cd "$TCC" && ./configure --cpu=arm64 --cc="$CROSS" --ar="$AR" \
        --extra-cflags="-static -O2" --extra-ldflags="-static" --prefix=/usr >/dev/null 2>&1 \
      && "$HOSTCC" -DC2STR conftest.c -o c2str.exe \
      && ./c2str.exe include/tccdefs.h tccdefs_.h \
      && make tcc >/dev/null 2>&1 ) || die "tcc build failed"
fi
[ -x "$TCC/tcc" ] || die "no tcc binary"
# tcc runtime: compile the arm64 runtime objects with the cross cc (no stack-protector)
if [ ! -f "$TCC/libtcc1.a" ]; then
    objs=""
    for src in lib/lib-arm64.c lib/stdatomic.c lib/atomic.S lib/builtin.c \
               lib/alloca.S lib/alloca-bt.S lib/armflush.c lib/dsohandle.c; do
        o="$TCC/${src%.*}.tcc1.o"
        "$CROSS" -c -O2 -fno-stack-protector -I"$TCC" -I"$TCC/include" "$TCC/$src" -o "$o" 2>/dev/null \
            && objs="$objs $o"
    done
    "$AR" rcs "$TCC/libtcc1.a" $objs || die "libtcc1.a build failed"
fi

# ---- build musl static for aarch64 (no stack protector) ---------------------
MUSL=$CACHE/musl
if [ ! -f "$MUSL/muslroot/lib/libc.a" ]; then
    echo "== building musl $MUSL_VER (static) =="
    if [ ! -d "$MUSL" ]; then
        curl -sL "https://musl.libc.org/releases/musl-$MUSL_VER.tar.gz" -o "$CACHE/musl.tgz" \
            && tar -C "$CACHE" -xf "$CACHE/musl.tgz" && mv "$CACHE/musl-$MUSL_VER" "$MUSL" || skip "musl fetch failed"
    fi
    ( cd "$MUSL" && ./configure --target=aarch64-linux-gnu CC="$CROSS" --disable-shared \
        CFLAGS="-O2 -fno-stack-protector" --prefix="$MUSL/muslroot" >/dev/null 2>&1 \
      && make -j"$(nproc)" >/dev/null 2>&1 && make install >/dev/null 2>&1 ) || die "musl build failed"
fi
[ -f "$MUSL/muslroot/lib/libc.a" ] || die "no musl libc.a"

# ---- stage the guest rootfs (busybox + toolchain + sysroot + tests) ---------
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/root"; mkdir -p "$STAGE/opt/tcc" "$STAGE/usr/lib/$TRIPLET" "$STAGE/usr/include" "$STAGE/work"
zcat "$INITRD" | (cd "$STAGE" && cpio -idmu 2>/dev/null)
cp "$TCC/tcc" "$STAGE/opt/tcc/"; cp "$TCC/libtcc1.a" "$STAGE/opt/tcc/"; cp -r "$TCC/include" "$STAGE/opt/tcc/include"
cp "$MUSL"/muslroot/lib/*.o "$STAGE/usr/lib/$TRIPLET/"; cp "$MUSL"/muslroot/lib/*.a "$STAGE/usr/lib/$TRIPLET/"
cp -r "$MUSL"/muslroot/include/. "$STAGE/usr/include/"
cp "$HERE"/tests/*.c "$STAGE/work/" 2>/dev/null || true

# guest runner: compile each test in-guest, run it, compare to its expected line
cat > "$STAGE/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc; /bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null
mkdir -p /tmp 2>/dev/null
exec > /dev/console 2>&1
cd /work
echo "=== IN-GUEST-BUILD: tcc $( /opt/tcc/tcc -v 2>&1 | head -1 ) ==="
pass=0; fail=0
for c in *.c; do
    exp=$(sed -n 's/^\/\/ EXPECT: //p' "$c")
    /opt/tcc/tcc -B/opt/tcc -static "$c" -o "${c%.c}" 2>/tmp/cc.err
    if [ $? -ne 0 ]; then echo "BUILD-FAIL $c"; head -3 /tmp/cc.err; fail=$((fail+1)); continue; fi
    out=$(./"${c%.c}" 2>&1)
    if [ "$out" = "$exp" ]; then echo "PASS $c -> $out"; pass=$((pass+1));
    else echo "RUN-FAIL $c: got [$out] want [$exp]"; fail=$((fail+1)); fi
done
echo "=== IG-RESULT pass=$pass fail=$fail ==="
/bin/busybox poweroff -f
INIT
chmod +x "$STAGE/init"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip ) > "$WORK/initrd.gz"

# ---- boot + score (plain imx91 tuple; NO System Manager) --------------------
LOG="$WORK/serial.log"
echo "== booting i.MX 91; building + running in-guest =="
timeout "$TMO" "$QEMU" -M imx91-11x11-evk -smp "$SMP" -m "$MEM" -display none \
  -kernel "$IMAGE" -dtb "$DTB" -initrd "$WORK/initrd.gz" \
  -append "console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
  -serial file:"$LOG" -serial null >/dev/null 2>&1 || true

echo "================== IN-GUEST BUILD (i.MX 91) =================="
sed -n '/IN-GUEST-BUILD:/,/IG-RESULT/p' "$LOG" | grep -avE '^\[ *[0-9]+\.[0-9]+\]'
res=$(grep -aoE 'IG-RESULT pass=[0-9]+ fail=[0-9]+' "$LOG" | tail -1)
[ -n "$res" ] || die "no result marker (boot/build did not complete)"
echo "$res"
echo "$res" | grep -q 'fail=0' && { echo "PASS: A55 built + ran all in-guest tests"; exit 0; } || die "$res"
