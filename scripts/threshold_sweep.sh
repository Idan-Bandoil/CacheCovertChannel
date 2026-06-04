#!/usr/bin/env bash
# Sweep (ROBUST_TESTS, ROBUST_THRESHOLD) on mapping_attack.
# Builds with -DROBUST_TESTS=N -DROBUST_THRESHOLD=K per cell, runs N=$RUNS
# trials, prints a TSV summary.
#
# Usage: ./scripts/threshold_sweep.sh [runs] [core]
set -euo pipefail

RUNS=${1:-30}
CORE=${2:-4}

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/src/mapping_attack.c"
UTILS="$REPO/src/utils.c"
BIN="$REPO/mapping_attack"
CFLAGS="-Wall -Wextra -O2 -march=native -std=c11 -pedantic"

# (tests, threshold) cells, low-->high conservatism for tests/2 ratio
CELLS=(
  "3 2"
  "5 2"
  "5 3"   # control / current default
  "5 4"
  "7 3"
  "7 4"
  "9 4"
  "9 5"
)

printf "tests\tthresh\tverified/N\taligned100/N\tmean_runtime_s\n"

for cell in "${CELLS[@]}"; do
  read -r T TH <<< "$cell"
  # Build for this cell
  gcc $CFLAGS -DROBUST_TESTS=$T -DROBUST_THRESHOLD=$TH -c "$SRC" -o /tmp/mapping_attack_$$.o
  gcc $CFLAGS -c "$UTILS" -o /tmp/utils_$$.o
  gcc $CFLAGS -o "$BIN" /tmp/mapping_attack_$$.o /tmp/utils_$$.o
  rm -f /tmp/mapping_attack_$$.o /tmp/utils_$$.o

  pass=0; aligned100=0; total_time_ms=0
  for ((i=1; i<=RUNS; i++)); do
    t0=$(date +%s%N)
    out=$(sudo -n "$BIN" "$CORE" 2>&1)
    t1=$(date +%s%N)
    total_time_ms=$(( total_time_ms + (t1 - t0) / 1000000 ))

    if grep -q VERIFIED <<< "$out"; then pass=$((pass+1)); fi
    al=$(grep -oP "aligned \K[0-9]+" <<< "$out" | head -1)
    if [ "$al" = "100" ]; then aligned100=$((aligned100+1)); fi
  done

  mean_s=$(awk -v ms="$total_time_ms" -v n="$RUNS" 'BEGIN{printf "%.2f", (ms/1000.0)/n}')
  printf "%d\t%d\t%d/%d\t%d/%d\t%s\n" "$T" "$TH" "$pass" "$RUNS" "$aligned100" "$RUNS" "$mean_s"
done