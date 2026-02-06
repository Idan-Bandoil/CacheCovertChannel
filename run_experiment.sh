#!/bin/bash

# --- Helper Function: Core Detection ---
get_core_type() {
    local target_core=$1
    local topo_file="/sys/devices/system/cpu/cpu$target_core/topology/core_type"
    local freq_file="/sys/devices/system/cpu/cpu$target_core/cpufreq/cpuinfo_max_freq"

    # Method 1: Kernel Topology (Linux 5.18+)
    if [ -f "$topo_file" ]; then
        local type_str=$(cat "$topo_file")
        if [[ "$type_str" == *"atom"* ]]; then echo "E-Core"; return; fi
        if [[ "$type_str" == *"core"* ]]; then echo "P-Core"; return; fi
    fi

    # Method 2: Frequency Fallback
    if [ ! -f "$freq_file" ]; then echo "Unknown"; return; fi
    
    local my_freq=$(cat "$freq_file")
    # Get max freq across all cores
    local max_sys_freq=$(cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq 2>/dev/null | sort -nr | head -n1)
    
    if [ -z "$max_sys_freq" ]; then echo "P-Core"; return; fi

    local threshold=$((max_sys_freq - 200000))

    if [ "$my_freq" -lt "$threshold" ]; then echo "E-Core"; else echo "P-Core"; fi
}
# ---------------------------------------

echo "[*] Compiling profiler..."
make > /dev/null
if [ $? -ne 0 ]; then echo "Compilation failed"; exit 1; fi

# Initialize Statistics Variables
p_count=0; p_sum=0; p_min=99999; p_max=0
e_count=0; e_sum=0; e_min=99999; e_max=0

echo "[*] Running latency profile on all cores..."
echo "---------------------------------------------------------"
printf "%-10s %-10s %-15s\n" "Core ID" "Type" "Latency (Avg)"
echo "---------------------------------------------------------"

# Iterate over all CPU directories found in sysfs
for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
    # Extract just the number (e.g., "cpu0" -> "0")
    core_id=$(basename "$cpu_dir" | sed 's/cpu//')
    
    # Skip if not a valid integer (sanity check)
    if ! [[ "$core_id" =~ ^[0-9]+$ ]]; then continue; fi

    # Determine type
    core_type=$(get_core_type $core_id)

    # Run Profiler
    # Output format expected: AVG:36.00 MODE:36 MIN:34 MAX:48
    raw_output=$(taskset -c $core_id ./latency_profiler)
    
    # Extract the AVG value using awk (splitting by space and then colon)
    # $1 is "AVG:36.00", split by ':' gives "36.00"
    avg_latency=$(echo "$raw_output" | awk -F' ' '{print $1}' | awk -F':' '{print $2}')
    
    # Convert to integer for easier math (bash handles floats poorly without bc)
    # We round it: 36.00 -> 36
    avg_int=$(printf "%.0f" "$avg_latency")

    printf "%-10s %-10s %-15s\n" "$core_id" "$core_type" "${avg_latency} cycles"

    # Update Statistics
    if [ "$core_type" == "P-Core" ]; then
        p_count=$((p_count + 1))
        p_sum=$((p_sum + avg_int))
        if [ "$avg_int" -lt "$p_min" ]; then p_min=$avg_int; fi
        if [ "$avg_int" -gt "$p_max" ]; then p_max=$avg_int; fi
    elif [ "$core_type" == "E-Core" ]; then
        e_count=$((e_count + 1))
        e_sum=$((e_sum + avg_int))
        if [ "$avg_int" -lt "$e_min" ]; then e_min=$avg_int; fi
        if [ "$avg_int" -gt "$e_max" ]; then e_max=$avg_int; fi
    fi
done

echo "---------------------------------------------------------"
echo " FINAL STATISTICS"
echo "---------------------------------------------------------"

# Calculate Averages (avoid divide by zero)
if [ $p_count -gt 0 ]; then
    p_avg=$((p_sum / p_count))
    echo "P-Cores ($p_count found):"
    echo "  Average Latency : $p_avg cycles"
    echo "  Range           : $p_min - $p_max cycles"
else
    echo "P-Cores: None found."
fi

echo ""

if [ $e_count -gt 0 ]; then
    e_avg=$((e_sum / e_count))
    echo "E-Cores ($e_count found):"
    echo "  Average Latency : $e_avg cycles"
    echo "  Range           : $e_min - $e_max cycles"
else
    echo "E-Cores: None found."
fi

echo "---------------------------------------------------------"