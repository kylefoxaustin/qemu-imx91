#!/bin/bash
# Lua — oracle: a self-test script that asserts deterministic results across
# string/table/math/coroutine/closure paths (exit 0 + "LUA SELFTEST OK").
# Cross-built static interpreter.
set -euo pipefail
VER=5.4.7
URL=https://www.lua.org/ftp/lua-${VER}.tar.gz
TAR="$DL/lua-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
cd "lua-${VER}/src"
# generic POSIX build, static, no readline dependency
make all CC="${CROSS}gcc" \
  MYCFLAGS="-O2 -DLUA_USE_POSIX" MYLDFLAGS="-static" SYSLIBS="-lm" >/dev/null 2>&1
cp lua "$STAGE/lua"
# selftest.lua is a static asset shipped from the corpus dir by build-corpus.sh
