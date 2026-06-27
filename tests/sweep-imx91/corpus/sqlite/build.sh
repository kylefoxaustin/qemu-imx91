#!/bin/bash
# SQLite — oracle: TIER-3 differential. Run a deterministic SQL workload through
# the cross-built A55 sqlite3 in-guest and diff against the host-built golden.
#
# Built DYNAMIC (against the guest's own glibc), deliberately: a fully *static*
# glibc link of the large sqlite3 shell SIGSEGVs at startup on the guest (a known
# glibc static-IFUNC/NSS fragility in the Ubuntu cross toolchain — NOT a model
# bug; the dynamic build against the guest's matched glibc runs clean). This
# mirrors how the BSP itself ships sqlite3. Requires guest glibc >= build glibc.
set -euo pipefail
VER=3460100
URL=https://www.sqlite.org/2024/sqlite-autoconf-${VER}.tar.gz
TAR="$DL/sqlite-autoconf-${VER}.tar.gz"
[ -f "$TAR" ] || curl -fsSL -o "$TAR" "$URL"
tar xzf "$TAR"
cd "sqlite-autoconf-${VER}"

CFLAGS="-O2 -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1"
"${CROSS}gcc" $CFLAGS -o "$STAGE/sqlite3" shell.c sqlite3.c -lm -ldl

# host (oracle) -> golden output for the shipped workload.sql
gcc $CFLAGS -o ./sqlite3_host shell.c sqlite3.c -lm -lpthread -ldl
./sqlite3_host :memory: < "$(dirname "$0")/workload.sql" > "$STAGE/golden.txt" 2>&1
