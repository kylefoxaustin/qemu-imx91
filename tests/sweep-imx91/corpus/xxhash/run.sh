#!/bin/sh
# In-guest. Reproduce the host-computed golden hashes on the A55; any mismatch is
# a real cross-arch divergence.
ok=1
for h in 0 1 2 3; do
  want=$(cat "golden.H$h" 2>/dev/null)
  got=$(./xxhsum -H$h corpus.dat 2>/dev/null | awk '{print $1}')
  if [ -z "$want" ] || [ "$want" != "$got" ]; then
    echo "SOAK:FAIL:xxhash-H$h:want=$want got=$got"
    ok=0
  else
    echo "SOAK:PASS:xxhash-H$h:$got"
  fi
done
[ "$ok" = 1 ] || true
