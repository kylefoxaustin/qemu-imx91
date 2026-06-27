# Shared SOAK:-marker scoreboard engine (sourced by sweep.sh and soak.sh).
#
# This is the host-side scorer the fleet (91/93/95) agreed to align on so the
# dashboards are literally diff-able. The in-guest battery prints one line per
# case to the serial console:
#   SOAK:PASS:<name>:<detail>     a routine ran and matched its oracle
#   SOAK:FAIL:<name>:<reason>     ran but the oracle rejected it
#   SOAK:SKIP:<name>:<reason>     not applicable / dep absent (FIRST-CLASS, not a fail)
#   SOAK:BOOT:<model>:<soc>       one clean boot reached userspace
#   SOAK:BATTERY:DONE             the battery finished (absence => boot hang)
# The host greps "^SOAK:" out of the boot log and accumulates counter files in
# $STATE/<name>.<pass|fail|skip>. Stateless, survives reboots, totals climb.
#
# Callers must set: STATE (a writable dir) before sourcing.

bump() {                        # bump <name> <pass|fail|skip>
  local key="$1.$2"
  local n; n=$(cat "$STATE/$key" 2>/dev/null || echo 0)
  echo $((n + 1)) > "$STATE/$key"
}

# Parse one boot log into the counters. $1=log, $2=tag (for failures.log context).
score_log() {
  local log="$1" tag="${2:-}"
  while IFS= read -r line; do
    case "$line" in
      SOAK:PASS:*) n="${line#SOAK:PASS:}"; bump "${n%%:*}" pass ;;
      SOAK:FAIL:*) n="${line#SOAK:FAIL:}"; bump "${n%%:*}" fail
                   echo "[$tag] $line" >> "$STATE/failures.log" ;;
      SOAK:SKIP:*) n="${line#SOAK:SKIP:}"; bump "${n%%:*}" skip ;;
    esac
  done < <(grep "^SOAK:" "$log" 2>/dev/null)
}

dashboard() {
  echo; echo "==================== SWEEP DASHBOARD ===================="
  printf "%-28s %6s %6s %6s\n" routine PASS FAIL SKIP
  local tp=0 tf=0 ts=0
  for f in $(ls "$STATE" 2>/dev/null | sed -E 's/\.(pass|fail|skip)$//' | grep -vE 'failures|rss' | sort -u); do
    p=$(cat "$STATE/$f.pass" 2>/dev/null || echo 0)
    fl=$(cat "$STATE/$f.fail" 2>/dev/null || echo 0)
    s=$(cat "$STATE/$f.skip" 2>/dev/null || echo 0)
    printf "%-28s %6s %6s %6s\n" "$f" "$p" "$fl" "$s"
    tp=$((tp+p)); tf=$((tf+fl)); ts=$((ts+s))
  done
  printf "%-28s %6s %6s %6s\n" "TOTAL" "$tp" "$tf" "$ts"
  if [ -s "$STATE/failures.log" ]; then
    echo "--- failures ---"; tail -20 "$STATE/failures.log"
  fi
  echo "========================================================"
}
