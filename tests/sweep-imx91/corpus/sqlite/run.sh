#!/bin/sh
# In-guest SQLite: reproduce the host golden for the same workload.
rm -f wk.db
./sqlite3 :memory: < workload.sql > out.txt 2>err.txt; rc=$?
if [ "$rc" = 0 ] && cmp -s out.txt golden.txt; then
  echo "SOAK:PASS:sqlite-workload:$(wc -l < out.txt) lines match golden"
else
  echo "SOAK:FAIL:sqlite-workload:rc=$rc out=$(wc -c <out.txt)B err=[$(tr '\n' ' ' <err.txt|cut -c1-80)]"
  diff golden.txt out.txt 2>/dev/null | head -4 | while IFS= read -r l; do echo "SOAK:FAIL:sqlite-workload:  $l"; done
fi
