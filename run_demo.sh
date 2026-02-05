#!/bin/bash

# Default values
SENDER_CORE=""
RECEIVER_CORE=""

# 1. Parse Arguments
while getopts "s:r:" opt; do
  case $opt in
    s) SENDER_CORE="$OPTARG" ;;
    r) RECEIVER_CORE="$OPTARG" ;;
    *) 
       echo "Usage: $0 -s <sender_core_id> -r <receiver_core_id>"
       exit 1
       ;;
  esac
done

if [[ -z "$SENDER_CORE" || -z "$RECEIVER_CORE" ]]; then
    echo "Error: You must specify both core IDs."
    echo "Usage: $0 -s <sender_core_id> -r <receiver_core_id>"
    exit 1
fi

# ---------------------------------------------------------
# Robust Core Type Detection (P-Core vs E-Core)
# ---------------------------------------------------------
get_core_type() {
    local target_core=$1
    local topo_file="/sys/devices/system/cpu/cpu$target_core/topology/core_type"
    local freq_file="/sys/devices/system/cpu/cpu$target_core/cpufreq/cpuinfo_max_freq"
    
    # METHOD 1: Direct Kernel Support (Linux 5.18+)
    if [ -f "$topo_file" ]; then
        local type_str=$(cat "$topo_file")
        # Clean up string
        if [[ "$type_str" == *"atom"* ]]; then
             echo "E-Core"
             return
        elif [[ "$type_str" == *"core"* ]]; then
             echo "P-Core"
             return
        fi
    fi

    # METHOD 2: Frequency Heuristic (Fallback)
    # If the file doesn't exist, we compare frequencies.
    # P-Cores run faster. E-Cores run slower.
    
    if [ ! -f "$freq_file" ]; then
        # If we can't read freq, assume P-Core (Standard)
        echo "P-Core (Assumed)"
        return
    fi

    local my_max_freq=$(cat "$freq_file")
    local system_max_freq=0

    # Scan all CPUs to find the absolute highest frequency on the system
    for f in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq; do
        if [ -f "$f" ]; then
            local current_freq=$(cat "$f")
            if [ "$current_freq" -gt "$system_max_freq" ]; then
                system_max_freq=$current_freq
            fi
        fi
    done

    # Threshold: If this core is slower than the system max by > 5%, it's an E-Core
    # (We use a small buffer because frequencies can vary slightly)
    local threshold=$((system_max_freq - 200000)) # 200MHz buffer

    if [ "$my_max_freq" -lt "$threshold" ]; then
        echo "E-Core"
    else
        echo "P-Core"
    fi
}
# ---------------------------------------------------------

# 2. Print Configuration Info
S_TYPE=$(get_core_type $SENDER_CORE)
R_TYPE=$(get_core_type $RECEIVER_CORE)

echo "------------------------------------------------"
echo "[INFO] Configuration Selected:"
echo "  -> Sender on Core $SENDER_CORE   [$S_TYPE]"
echo "  -> Receiver on Core $RECEIVER_CORE [$R_TYPE]"
echo "------------------------------------------------"

# 3. Clean and Compile
echo "[*] Compiling..."
make clean > /dev/null 2>&1
make
if [ $? -ne 0 ]; then
    echo "[!] Compilation failed. Aborting."
    exit 1
fi

# 4. Prepare the shared file
echo "[*] Preparing shared memory file..."
rm -f shared_data.bin
touch shared_data.bin
truncate -s 4k shared_data.bin

# 5. Launch SENDER
echo "[*] Launching Sender..."
gnome-terminal --title="SENDER (Core $SENDER_CORE)" -- bash -c "taskset -c $SENDER_CORE ./sender; echo; echo 'Sender finished.'; read -p 'Press Enter to exit...'"

# 6. Wait 1 Second
echo "[*] Waiting 1 second..."
sleep 1

# 7. Launch RECEIVER
echo "[*] Launching Receiver..."
gnome-terminal --title="RECEIVER (Core $RECEIVER_CORE)" -- bash -c "taskset -c $RECEIVER_CORE ./receiver; echo; echo 'Receiver finished.'; read -p 'Press Enter to exit...'"

echo "[*] Demo running."