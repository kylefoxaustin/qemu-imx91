#!/bin/sh
# In-guest. Emits SOAK markers for two zlib routines.

# (1) example: zlib's own self-test. Exercises deflate/inflate, gzip files,
#     dictionaries, flush modes; exits 0 and prints "... OK" lines on success.
if ./example >ex.out 2>&1; then
  echo "SOAK:PASS:zlib-example:self-test ok"
else
  echo "SOAK:FAIL:zlib-example:rc=$? $(tail -1 ex.out)"
fi

# (2) minigzip round-trip: compress then decompress a payload, compare.
payload="The quick brown fox 0123456789 jumps over the lazy dog. zlib rt."
i=0; while [ $i -lt 1000 ]; do printf '%s\n' "$payload"; i=$((i+1)); done > rt.in
if ./minigzip < rt.in > rt.gz 2>/dev/null && ./minigzip -d < rt.gz > rt.out 2>/dev/null \
   && cmp -s rt.in rt.out; then
  echo "SOAK:PASS:zlib-minigzip:roundtrip $(wc -c < rt.in)B ok"
else
  echo "SOAK:FAIL:zlib-minigzip:roundtrip mismatch"
fi
