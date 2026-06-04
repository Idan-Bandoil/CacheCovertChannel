#!/usr/bin/env bash
# Wrapper that snapshots the CPU governor for a target core, sets `performance`
# for the duration of the run, runs the binary, and restores the original
# governor on exit (even on Ctrl-C / kill -TERM).
#
# Usage: ./scripts/run_with_env.sh <binary> <core_id> [args...]
#
# Requires sudoers grant for:
#   /usr/bin/tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

set -euo pipefail

if [ $# -lt 2 ]; then
  echo "Usage: $0 <binary> <core_id> [args...]" >&2
  exit 2
fi

BIN="$1"; shift
CORE="$1"; shift

GOV_PATH="/sys/devices/system/cpu/cpu${CORE}/cpufreq/scaling_governor"
ORIG_GOV=$(cat "$GOV_PATH")

restore_gov() {
  echo "$ORIG_GOV" | sudo -n tee "$GOV_PATH" >/dev/null || true
}
trap restore_gov EXIT INT TERM

echo "performance" | sudo -n tee "$GOV_PATH" >/dev/null

sudo -n "$BIN" "$CORE" "$@"