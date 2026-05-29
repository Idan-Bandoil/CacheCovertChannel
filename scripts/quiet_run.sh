#!/usr/bin/env bash
# Run a mapping_attack-class binary on a quiesced measurement core.
#
#   - pins the `performance` governor on the target core (a `powersave` core
#     scales frequency mid-run, drifting the cycle threshold)
#   - idles the target core's SMT sibling(s) (removes intra-core contention)
#   - warns about heavy shared-L3 apps, the DOMINANT noise source
#   - restores the governor and re-onlines the sibling(s) on exit
#
# Usage: ./scripts/quiet_run.sh <binary> <core_id> [args...]
#   e.g. ./scripts/quiet_run.sh ./mapping_attack 2
#
# IMPORTANT: the 30 MB LLC is shared by ALL cores, so this CANNOT isolate the
# L3. Background browsers/IDEs/Electron apps (firefox, chrome, code, gitkraken,
# ...) thrash the shared LLC and evict the victim regardless of the test set,
# which is what makes mapping_attack's accuracy swing run-to-run. Close them
# before measuring. For full reliability, measure on a headless / isolcpus'd box.
#
# Requires sudoers NOPASSWD for `tee` on the governor and cpu*/online paths
# (same grant pattern as scripts/run_with_env.sh).

set -uo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <binary> <core_id> [args...]" >&2
  exit 2
fi
BIN="$1"; CORE="$2"; shift 2

GOV="/sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor"
ORIG_GOV=$(cat "$GOV" 2>/dev/null || echo "")
SIBS=$(tr ',-' ' ' < "/sys/devices/system/cpu/cpu${CORE}/topology/thread_siblings_list" 2>/dev/null || echo "")
OFFLINED=""

restore() {
  [ -n "$ORIG_GOV" ] && echo "$ORIG_GOV" | sudo -n tee "$GOV" >/dev/null 2>&1 || true
  for s in $OFFLINED; do
    echo 1 | sudo -n tee "/sys/devices/system/cpu/cpu$s/online" >/dev/null 2>&1 || true
  done
  [ -n "$OFFLINED" ] && echo "[*] restored: governor=$ORIG_GOV, re-onlined sibling(s):$OFFLINED"
}
trap restore EXIT INT TERM

# Contention check (advisory).
LOAD=$(cut -d' ' -f1 /proc/loadavg)
HEAVY=$(ps -eo comm,%cpu --sort=-%cpu 2>/dev/null \
        | awk 'NR>1 && $2>2 {print $1}' \
        | grep -iE 'firefox|chrome|chromium|^code|gitkraken|electron|slack|teams|spotify' \
        | sort -u | head -6 | tr '\n' ' ')
echo "[*] load(1m)=$LOAD"
if [ -n "$HEAVY" ]; then
  echo "[!] Heavy shared-L3 apps running: $HEAVY"
  echo "[!] The LLC is shared by ALL cores -- close these for reliable cache timing."
fi

# Pin performance governor.
if ! echo performance | sudo -n tee "$GOV" >/dev/null 2>&1; then
  echo "[!] could not set 'performance' governor (sudoers grant for tee on $GOV?)" >&2
fi

# Idle the SMT sibling(s) of the target core.
for s in $SIBS; do
  [ "$s" = "$CORE" ] && continue
  if echo 0 | sudo -n tee "/sys/devices/system/cpu/cpu$s/online" >/dev/null 2>&1; then
    OFFLINED="$OFFLINED $s"
  fi
done
echo "[*] core $CORE: governor=$(cat "$GOV" 2>/dev/null), idled sibling(s):${OFFLINED:- none}"

sudo -n "$BIN" "$CORE" "$@"
