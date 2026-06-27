#!/bin/sh
# In-guest coreutils differential: run each case through the cross-built
# coreutils (PATH -> staged bin) and diff against the host golden.
export PATH="$PWD/bin:$PATH"
TAB=$(printf '\t')
while IFS="$TAB" read -r name cmd; do
  case "$name" in ''|\#*) continue ;; esac
  [ -n "$cmd" ] || continue
  [ -f "golden/$name" ] || { echo "SOAK:SKIP:coreutils-$name:no golden"; continue; }
  sh -c "$cmd" > "out.$name" 2>&1
  if cmp -s "out.$name" "golden/$name"; then
    echo "SOAK:PASS:coreutils-$name:matches golden"
  else
    echo "SOAK:FAIL:coreutils-$name:$(diff golden/$name out.$name 2>/dev/null | head -2 | tr '\n' ' ' | cut -c1-70)"
  fi
  rm -f "out.$name"
done < cases.tsv
