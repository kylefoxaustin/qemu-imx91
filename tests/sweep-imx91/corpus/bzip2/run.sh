#!/bin/sh
# In-guest bzip2 round-trip at a few sizes.
ok=1
for n in 100 5000 50000; do
  i=0; : > rt.in
  while [ $i -lt $n ]; do printf 'bzip2 line %d the quick brown fox\n' $i; i=$((i+1)); done > rt.in
  if ./bzip2 -c < rt.in > rt.bz2 2>/dev/null && ./bzip2 -dc < rt.bz2 > rt.out 2>/dev/null \
     && cmp -s rt.in rt.out; then :; else ok=0; fi
done
if [ "$ok" = 1 ]; then echo "SOAK:PASS:bzip2-roundtrip:3 sizes ok"; else echo "SOAK:FAIL:bzip2-roundtrip:mismatch"; fi
