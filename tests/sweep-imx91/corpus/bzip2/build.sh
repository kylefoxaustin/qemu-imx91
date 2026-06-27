#!/bin/bash
# bzip2 — oracle: compress/decompress round-trip plus the upstream sample
# reference files (sample1/2/3.ref) when present. Cross-built static.
set -euo pipefail
VER=1.0.8
URL=https://sourceware.org/pub/bzip2/bzip2-${VER}.tar.gz
TAR="$DL/bzip2-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
cd "bzip2-${VER}"
make bzip2 CC="${CROSS}gcc" CFLAGS="-O2 -static -D_FILE_OFFSET_BITS=64" >/dev/null
cp bzip2 "$STAGE/bzip2"
