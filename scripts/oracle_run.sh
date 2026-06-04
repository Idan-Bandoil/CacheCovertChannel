#!/usr/bin/env bash
# scripts/oracle_run.sh — one-shot harness for slice_oracle.
#
# slice_oracle needs REAL root (virt_to_phys via /proc/self/pagemap, MAP_HUGETLB,
# /dev/cpu_dma_latency).  This wrapper, run as root, prepares the quiet regime the
# feasibility measurement needs and restores it on exit:
#   - performance governor on every core (powersave drifts the cycle threshold),
#   - the 6 P-core SMT siblings (1,3,5,7,9,11) idled (a busy sibling shares the
#     agent's L1/L2 and reads flat L2 hits, polluting the staged measurement),
#   - the huge-page pool grown for the anchor + wash buffers.
# Governor + siblings are snapshotted and restored even on Ctrl-C / kill.
#
# Usage:  sudo ./scripts/oracle_run.sh [agent_csv]
#   e.g.  sudo ./scripts/oracle_run.sh                       # default 14 agents
#         sudo ./scripts/oracle_run.sh 12,13,14,15,16,17,18,19   # E-cores only
set -uo pipefail
cd "$(dirname "$0")/.."

if [ "$(id -u)" -ne 0 ]; then echo "[-] Run as root: sudo $0" >&2; exit 1; fi
if [ ! -x ./slice_oracle ]; then echo "[-] ./slice_oracle not built. Run: make slice_oracle" >&2; exit 1; fi

AGENTS="${1:-0,2,4,6,8,10,12,13,14,15,16,17,18,19}"
SIBS=(1 3 5 7 9 11)

declare -A ORIG_GOV
for d in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do ORIG_GOV["$d"]="$(cat "$d")"; done

restore() {
  for c in "${SIBS[@]}"; do echo 1 > "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null || true; done
  for d in "${!ORIG_GOV[@]}"; do echo "${ORIG_GOV[$d]}" > "$d" 2>/dev/null || true; done
  echo "[*] restored governor + SMT siblings."
}
trap restore EXIT INT TERM

echo "[*] performance governor on all cores..."
for d in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do echo performance > "$d" 2>/dev/null || true; done
echo "[*] idling P-core SMT siblings: ${SIBS[*]}"
for c in "${SIBS[@]}"; do echo 0 > "/sys/devices/system/cpu/cpu$c/online" 2>/dev/null || true; done
echo "[*] growing huge-page pool to 12..."
echo 12 > /proc/sys/vm/nr_hugepages 2>/dev/null || true
grep -iE 'HugePages_(Total|Free)' /proc/meminfo

echo "[*] launching slice_oracle (agents=$AGENTS)"
echo "------------------------------------------------------------------------"
./slice_oracle "$AGENTS"
