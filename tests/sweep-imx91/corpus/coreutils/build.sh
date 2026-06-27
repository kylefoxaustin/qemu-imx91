#!/bin/bash
# GNU coreutils — oracle: differential. Cross-build the coreutils binaries
# static; for each case in cases.tsv, run it through the HOST's coreutils to
# capture a golden, then in-guest run the same case through the cross-built
# coreutils and diff. Only version-stable transforms are used (see cases.tsv).
set -euo pipefail
VER=9.5
URL=https://ftp.gnu.org/gnu/coreutils/coreutils-${VER}.tar.xz
TAR="$DL/coreutils-${VER}.tar.xz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
SRC="$DL/coreutils-${VER}"
if [ ! -x "$SRC/src/sort" ]; then
  rm -rf "$SRC"; tar xf "$TAR" -C "$DL"
  ( cd "$SRC"
    # Build DYNAMIC: a static link with the Ubuntu aarch64 cross toolchain fails
    # to resolve __preinit_array/__fini_array on this autotools project (same
    # static-glibc fragility that hit sqlite/LTP). Dynamic links the guest's own
    # glibc at runtime and is the reliable path. FORCE_UNSAFE_CONFIGURE lets
    # configure proceed under restricted build envs.
    FORCE_UNSAFE_CONFIGURE=1 ./configure --host=aarch64-linux-gnu CC="${CROSS}gcc" \
      CFLAGS="-O2" >/dev/null 2>&1
    make -j"$(nproc)" >/dev/null 2>&1 || true )
fi

# stage the cross-built binaries that our cases reference (+ a few deps)
mkdir -p "$STAGE/bin"
CASES="$(dirname "$0")/cases.tsv"
cp "$CASES" "$STAGE/cases.tsv"
need=$(awk -F'\t' '!/^#/ && NF>=2 {print $2}' "$CASES" \
       | grep -oE '\b(seq|factor|sort|wc|head|tail|tac|cut|tr|nl|paste|uniq|comm|join|od|base64|sha256sum|sha512sum|sha1sum|md5sum|cksum|sum|expr|printf|fmt|fold|rev|numfmt|pr|stat|truncate|date)\b' \
       | sort -u)
staged=0
for t in $need; do
  if [ -x "$SRC/src/$t" ]; then cp "$SRC/src/$t" "$STAGE/bin/$t"; staged=$((staged+1)); fi
done
[ "$staged" -gt 0 ] || { echo "no coreutils binaries staged" >&2; exit 1; }

# generate the golden for each case using the HOST coreutils (system PATH)
mkdir -p "$STAGE/golden"
while IFS=$'\t' read -r name cmd; do
  case "$name" in ''|\#*) continue ;; esac
  [ -n "${cmd:-}" ] || continue
  sh -c "$cmd" > "$STAGE/golden/$name" 2>&1 || true
done < "$CASES"
echo "staged $staged coreutils binaries, $(ls "$STAGE/golden" | wc -l) goldens"
