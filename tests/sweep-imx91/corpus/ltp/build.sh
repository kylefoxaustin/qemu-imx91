#!/bin/bash
# LTP syscall subset — oracle: each test binary self-reports via the LTP exit
# BITMASK (0=pass, 1=TFAIL, 2=TBROK, 4=TWARN, 32=TCONF). run.sh maps TBROK/TCONF
# to SKIP (broken-env / unsupported, NOT a real failure) per the fleet's
# SKIP-first-class rule. Cross-built static from the release tarball (ships a
# pre-generated configure, so no autoconf needed). Staged via the safe
# allowlist in allow.txt — destructive/global-state tests are excluded.
set -euo pipefail
VER=20240524
TB="ltp-full-${VER}.tar.xz"
URL="https://github.com/linux-test-project/ltp/releases/download/${VER}/${TB}"
[ -f "$DL/$TB" ] || curl -fsSL -o "$DL/$TB" "$URL"
SRC="$DL/ltp-full-${VER}"
# Build DYNAMIC (links the guest's own glibc at runtime), deliberately: a STATIC
# glibc link makes every tiny syscall test ~1MB, and 500+ of them bloat the
# initramfs past what the kernel can load -> boot panics at mount_root. Dynamic
# drops each test to ~20KB. The guest rootfs ships ld-linux-aarch64 + glibc;
# requires guest glibc >= build glibc (same condition that made sqlite work).
if [ ! -f "$SRC/.built-dyn" ]; then
  rm -rf "$SRC"; tar xf "$DL/$TB" -C "$DL"
  ( cd "$SRC"
    ./configure --host=aarch64-linux-gnu CC="${CROSS}gcc" CFLAGS="-O2" >/dev/null 2>&1
    make -C include >/dev/null 2>&1
    make -C lib -j"$(nproc)" >/dev/null 2>&1
    make -C testcases/kernel/syscalls -j"$(nproc)" >/dev/null 2>&1 || true
    touch .built-dyn )
fi

# Build the set of allowed stems from allow.txt (strip comments).
ALLOW="$(dirname "$0")/allow.txt"
mkdir -p "$STAGE/bin"
staged=0
# For each allowed stem (allow.txt lists many per line; strip comment lines first)
# stage every built binary named <stem><digits> — the numbered variants of that
# syscall — plus an exact-name match.
for stem in $(grep -vE '^[[:space:]]*#' "$ALLOW"); do
  for bin in $(find "$SRC/testcases/kernel/syscalls" -maxdepth 2 -type f -perm -u+x \
                 \( -name "${stem}" -o -name "${stem}[0-9]" -o -name "${stem}[0-9][0-9]" \) 2>/dev/null); do
    file "$bin" | grep -q 'ARM aarch64' || continue
    b=$(basename "$bin")
    [ -e "$STAGE/bin/$b" ] && continue
    cp "$bin" "$STAGE/bin/$b"; staged=$((staged+1))
  done
done

[ "$staged" -gt 0 ] || { echo "no LTP binaries staged" >&2; exit 1; }
ls "$STAGE/bin" | sort > "$STAGE/manifest.txt"
echo "staged $staged LTP syscall test binaries"
