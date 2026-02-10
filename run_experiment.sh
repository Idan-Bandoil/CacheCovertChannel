#!/bin/bash

# --- Helper: Core Detection ---
get_core_type() {
    local target_core=$1
    local topo="/sys/devices/system/cpu/cpu$target_core/topology/core_type"
    local freq="/sys/devices/system/cpu/cpu$target_core/cpufreq/cpuinfo_max_freq"

    if [ -f "$topo" ]; then
        if grep -q "atom" "$topo"; then echo "E"; return; fi
        if grep -q "core" "$topo"; then echo "P"; return; fi
    fi

    # Fallback to frequency check
    local my_freq=$(cat "$freq" 2>/dev/null)
    local max_freq=$(cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -nr | head -n1)
    
    if [ -z "$max_freq" ] || [ -z "$my_freq" ]; then echo "P"; return; fi
    
    if [ "$my_freq" -lt $((max_freq - 200000)) ]; then echo "E"; else echo "P"; fi
}

# --- Compile ---
# Ensure the path to src/latency_profiler.c is correct relative to where you run this script
gcc src/latency_profiler.c -o latency_profiler -O3
if [ $? -ne 0 ]; then echo "Compile failed."; exit 1; fi

# --- Init Stats ---
p_n=0; p_l1=0; p_l2=0; p_l3=0; p_mem=0
e_n=0; e_l1=0; e_l2=0; e_l3=0; e_mem=0

echo "Running Profiler..."

# FIX: Use 'ls' combined with 'sort -V' to get numerical ordering (0, 1, 2... 10)
for cpu_dir in $(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V); do
    id=$(basename "$cpu_dir" | sed 's/cpu//')
    if ! [[ "$id" =~ ^[0-9]+$ ]]; then continue; fi

    type=$(get_core_type $id)

    # -f : Use SCHED_FIFO (First In, First Out)
    # 99 : Priority 99 (Highest real-time priority)
    out=$(chrt -f 99 taskset -c $id ./latency_profiler)
    
    # Extract values
    v1=$(echo "$out" | grep "L1 Cache" | awk '{printf "%.0f", $4}')
    v2=$(echo "$out" | grep "L2 Cache" | awk '{printf "%.0f", $4}')
    v3=$(echo "$out" | grep "L3 Cache" | awk '{printf "%.0f", $4}')
    vm=$(echo "$out" | grep "DRAM"     | awk '{printf "%.0f", $3}')

    # Simple Print
    echo "Core $id [$type] -> L1:$v1  L2:$v2  L3:$v3  RAM:$vm"

    # Accumulate
    if [ "$type" == "P" ]; then
        ((p_n++)); ((p_l1+=v1)); ((p_l2+=v2)); ((p_l3+=v3)); ((p_mem+=vm))
    else
        ((e_n++)); ((e_l1+=v1)); ((e_l2+=v2)); ((e_l3+=v3)); ((e_mem+=vm))
    fi
done

# --- Summary ---
echo ""
echo "=== Averages ==="

if [ $p_n -gt 0 ]; then
    echo "P-Cores: L1:$((p_l1/p_n))  L2:$((p_l2/p_n))  L3:$((p_l3/p_n))  RAM:$((p_mem/p_n))"
fi

if [ $e_n -gt 0 ]; then
    echo "E-Cores: L1:$((e_l1/e_n))  L2:$((e_l2/e_n))  L3:$((e_l3/e_n))  RAM:$((e_mem/e_n))"
fi