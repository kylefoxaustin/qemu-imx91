#!/bin/bash
# zstd — oracle: -t integrity check + compress/decompress round-trip across
# levels. Cross-built static CLI.
set -euo pipefail
VER=1.5.6
URL=https://github.com/facebook/zstd/releases/download/v${VER}/zstd-${VER}.tar.gz
TAR="$DL/zstd-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
cd "zstd-${VER}"
# build just the CLI, static, no extra backends needed for round-trip
make -C programs zstd \
  CC="${CROSS}gcc" LDFLAGS="-static" \
  HAVE_ZLIB=0 HAVE_LZMA=0 HAVE_LZ4=0 >/dev/null 2>&1
cp programs/zstd "$STAGE/zstd"
