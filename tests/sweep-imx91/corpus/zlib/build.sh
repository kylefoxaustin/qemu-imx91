#!/bin/bash
# zlib — oracle: the upstream `example` self-test program (returns 0 + prints OK)
# plus a minigzip round-trip. Cross-built static for the A55 guest.
#
# Contract (set by build-corpus.sh): CROSS, STAGE (=.../corpus/zlib), DL (cache).
set -euo pipefail
VER=1.3.1
URL=https://github.com/madler/zlib/releases/download/v${VER}/zlib-${VER}.tar.gz
TAR="$DL/zlib-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
cd "zlib-${VER}"

# zlib's configure honours CC/CFLAGS; build the static lib + the test programs.
CC="${CROSS}gcc" CFLAGS="-O2 -static" ./configure --static >/dev/null
make -j"$(nproc)" libz.a example minigzip >/dev/null

cp example minigzip "$STAGE/"
