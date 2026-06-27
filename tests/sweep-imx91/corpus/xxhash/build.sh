#!/bin/bash
# xxHash — oracle: TIER-3 differential. Build xxhsum for BOTH host and target;
# the host binary hashes a fixed corpus to produce the golden, the target binary
# must reproduce it in-guest. Catches width/endian/UB-codegen divergence the
# library's own tests can miss.
#
# Contract: CROSS, STAGE (=.../corpus/xxhash), DL.
set -euo pipefail
VER=0.8.2
URL=https://github.com/Cyan4973/xxHash/archive/refs/tags/v${VER}.tar.gz
TAR="$DL/xxhash-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
SRC="xxHash-${VER}"

# Target (static) xxhsum -> the binary under test.
make -C "$SRC" CC="${CROSS}gcc" LDFLAGS="-static" xxhsum >/dev/null
cp "$SRC/xxhsum" "$STAGE/xxhsum"

# Host xxhsum -> the oracle. Hash a fixed, content-rich corpus file.
make -C "$SRC" clean >/dev/null 2>&1 || true
make -C "$SRC" CC=gcc xxhsum >/dev/null
seq 1 100000 | "$SRC/xxhsum" -H0 >/dev/null   # warm; ensure binary runs
seq 1 100000 > "$STAGE/corpus.dat"
# Record golden for XXH32(H0), XXH64(H1), XXH3-64(H2), XXH128(H3).
for h in 0 1 2 3; do
  "$SRC/xxhsum" -H$h "$STAGE/corpus.dat" | awk '{print $1}' > "$STAGE/golden.H$h"
done
