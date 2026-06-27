#!/bin/sh
# In-guest: run each crypto KAT binary, parse its SUCCEEDED/FAILED self-report.
for bin in *; do
  case "$bin" in run.sh|*.txt|.*) continue ;; esac
  [ -f "$bin" ] && [ -x "$bin" ] || continue
  out=$(./"$bin" 2>&1)
  # B-Con reports "tests: SUCCEEDED" for most algos, "tests: PASSED" for base64
  if echo "$out" | grep -qiE "SUCCEEDED|PASSED" && ! echo "$out" | grep -qi "FAILED"; then
    echo "SOAK:PASS:crypto-$bin:KAT ok"
  else
    echo "SOAK:FAIL:crypto-$bin:$(echo "$out" | tr '\n' ' ' | cut -c1-60)"
  fi
done
