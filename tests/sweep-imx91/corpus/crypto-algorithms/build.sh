#!/bin/bash
# crypto-algorithms (B-Con, public domain) — oracle: each algorithm ships a
# main() running known-answer tests against published vectors, printing
# "<ALG> tests: SUCCEEDED/FAILED". We cross-compile every self-testing .c into a
# static binary; run.sh runs each and greps SUCCEEDED. ~9 KAT routines.
set -euo pipefail
SRC="$DL/crypto-algorithms"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 https://github.com/B-Con/crypto-algorithms "$SRC" >/dev/null 2>&1
fi
built=0
for t in "$SRC"/*_test.c; do
  [ -f "$t" ] || continue
  base=$(basename "$t" _test.c)        # e.g. sha256_test.c -> sha256
  impl="$SRC/$base.c"
  # link the test main with its implementation unit (impl may be header-only)
  srcs="$t"; [ -f "$impl" ] && srcs="$impl $t"
  if "${CROSS}gcc" -O2 -static -w -I"$SRC" -o "$STAGE/$base" $srcs 2>/dev/null; then
    built=$((built+1))
  fi
done
[ "$built" -gt 0 ] || { echo "no crypto KAT binaries built" >&2; exit 1; }
echo "built $built crypto KAT binaries"
