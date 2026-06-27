#!/bin/sh
# In-guest zstd: round-trip at several levels + frame integrity check.
i=0; : > rt.in
while [ $i -lt 20000 ]; do printf 'zstd %d the quick brown fox jumps\n' $i; i=$((i+1)); done > rt.in
ok=1
for lvl in 1 3 9 19; do
  if ./zstd -q -$lvl -c rt.in > rt.zst 2>/dev/null \
     && ./zstd -q -t rt.zst 2>/dev/null \
     && ./zstd -q -d -c rt.zst > rt.out 2>/dev/null \
     && cmp -s rt.in rt.out; then :; else ok=0; fi
done
if [ "$ok" = 1 ]; then echo "SOAK:PASS:zstd-roundtrip:levels 1/3/9/19 + -t ok"; else echo "SOAK:FAIL:zstd-roundtrip:mismatch or -t fail"; fi
