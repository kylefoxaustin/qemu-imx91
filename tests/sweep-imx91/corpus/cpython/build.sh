#!/bin/bash
# CPython 3.10 — oracle: the stdlib's own regression suite (`python -m test`),
# each module self-reporting PASS/FAIL. Cross-built DYNAMIC + --disable-shared
# (one interpreter binary, no shared .so dance — setup.py CANNOT cross-build
# shared extension modules with this toolchain). The dependency-free CPU-pure
# stdlib C modules are forced builtin via Modules/Setup.local (the trick
# 95emulator flagged); modules needing external libs (_ssl/_hashlib->openssl,
# _ctypes->libffi, _decimal->libmpdec, pyexpat->expat) are omitted and their
# tests SKIP in-guest. Build-python = host 3.10.x (same bytecode magic).
set -euo pipefail
VER=3.10.14
TB="Python-${VER}.tar.xz"
[ -f "$DL/$TB" ] || curl -fsSL -o "$DL/$TB" "https://www.python.org/ftp/python/${VER}/${TB}"
SRC="$DL/Python-${VER}"
HOSTPY=$(command -v python3)

if [ ! -f "$SRC/.built-py" ]; then
  rm -rf "$SRC"; tar xf "$DL/$TB" -C "$DL"
  cp "$(dirname "$0")/Setup.local" "$SRC/Modules/Setup.local"
  ( cd "$SRC"
    ./configure --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu \
      --disable-shared --without-ensurepip --disable-ipv6 --disable-test-modules \
      ac_cv_file__dev_ptmx=no ac_cv_file__dev_ptc=no \
      CC="${CROSS}gcc" PYTHON_FOR_BUILD="$HOSTPY" >/dev/null 2>&1
    make python -j"$(nproc)" >/dev/null 2>&1
    touch .built-py )
fi
[ -x "$SRC/python" ] || { echo "cpython interpreter did not build" >&2; exit 1; }

# stage: interpreter + the pure-python stdlib (incl Lib/test for `-m test`)
mkdir -p "$STAGE/py/bin" "$STAGE/py/lib/python3.10"
cp "$SRC/python" "$STAGE/py/bin/python3"
cp -a "$SRC/Lib/." "$STAGE/py/lib/python3.10/"
# drop precompiled host pyc caches (wrong path/arch noise) — guest recompiles
find "$STAGE/py/lib" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
echo "staged cpython $VER + stdlib ($(du -sh "$STAGE/py/lib" | cut -f1))"
