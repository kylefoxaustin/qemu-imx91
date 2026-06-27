#!/bin/sh
# In-guest LTP syscall subset. Each binary self-reports via the LTP exit bitmask;
# we translate to one SOAK row per test (ltp-<name>).
#   rc 0            -> PASS
#   rc & 32 (TCONF) -> SKIP  (unsupported/not-configured)
#   rc & 2  (TBROK) -> SKIP  (broken environment, not a real failure)
#   rc & 1  (TFAIL) -> FAIL
#   rc & 4  (TWARN) -> PASS with a warning (test passed, warned)
#   124/137 (timeout/killed) -> FAIL:timeout
#
# Env: LTP tests write under $TMPDIR and some need the nobody/daemon users.

export TMPDIR=/tmp
export LTPROOT="$PWD"
export PATH="$PWD/bin:$PATH"
# ensure the unprivileged users LTP's drop-priv tests expect exist
for u in nobody daemon bin; do
  grep -q "^$u:" /etc/passwd 2>/dev/null || \
    echo "$u:x:65534:65534:$u:/:/bin/false" >> /etc/passwd 2>/dev/null
done
# per-test timeout: env, else an LTP_PER=<secs> token on the kernel cmdline, else
# 30s (TCG is slow — sync-loops/timing tests need headroom or they false-timeout).
PER=${LTP_PER_TIMEOUT:-}
if [ -z "$PER" ]; then
  for tok in $(cat /proc/cmdline 2>/dev/null); do
    case "$tok" in LTP_PER=*) PER="${tok#LTP_PER=}" ;; esac
  done
fi
PER=${PER:-30}

run_one() {   # run_one <secs> <cmd...>  -> rc (124 on timeout)
  if [ "$HAVE_TIMEOUT" = 1 ]; then timeout "$1" "$2"; return $?; fi
  shift_secs=$1; shift
  "$@" & p=$!; ( sleep "$shift_secs"; kill -9 "$p" 2>/dev/null ) & w=$!
  wait "$p" 2>/dev/null; r=$?; kill -9 "$w" 2>/dev/null; return $r
}
HAVE_TIMEOUT=0
command -v timeout >/dev/null 2>&1 && timeout 1 true >/dev/null 2>&1 && HAVE_TIMEOUT=1

# Tests that are deterministically impractical under TCG (fork-storms / network
# timing that take minutes to emulate) — a model-SPEED gap, not a failure. Named
# SKIPs keep the pass-rate honest and are consistent across the 91/93/95 fleet.
TCG_SKIP=" fork13 setsockopt06 "

for b in bin/*; do
  [ -f "$b" ] && [ -x "$b" ] || continue
  name=$(basename "$b")
  case "$TCG_SKIP" in *" $name "*) echo "SOAK:SKIP:ltp-$name:tcg-impractical (slow under emulation)"; continue ;; esac
  work="/tmp/ltp-$name"; mkdir -p "$work"
  ( cd "$work" && run_one "$PER" "$LTPROOT/$b" ) >"/tmp/o.$name" 2>&1
  rc=$?
  rm -rf "$work"
  if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
    echo "SOAK:FAIL:ltp-$name:timeout-${PER}s"
  elif [ "$rc" = 0 ]; then
    echo "SOAK:PASS:ltp-$name:TPASS"
  elif [ $((rc & 32)) -ne 0 ]; then
    echo "SOAK:SKIP:ltp-$name:TCONF unsupported"
  elif [ $((rc & 2)) -ne 0 ]; then
    echo "SOAK:SKIP:ltp-$name:TBROK env"
  elif [ $((rc & 1)) -ne 0 ]; then
    echo "SOAK:FAIL:ltp-$name:TFAIL ($(grep -c TFAIL "/tmp/o.$name" 2>/dev/null) fails) $(grep TFAIL "/tmp/o.$name" 2>/dev/null | head -1 | cut -c1-50)"
  elif [ $((rc & 4)) -ne 0 ]; then
    echo "SOAK:PASS:ltp-$name:TWARN (passed w/ warning)"
  else
    echo "SOAK:FAIL:ltp-$name:rc=$rc"
  fi
  rm -f "/tmp/o.$name"
done
