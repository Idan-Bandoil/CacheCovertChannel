#!/bin/bash

# 1. Clean and Compile
echo "[*] Compiling..."
make clean > /dev/null
make
if [ $? -ne 0 ]; then
    echo "[!] Compilation failed. Aborting."
    exit 1
fi

# 2. Prepare the shared file
echo "[*] Preparing shared memory file..."
rm -f shared_data.bin
touch shared_data.bin
truncate -s 4k shared_data.bin

# 3. Launch SENDER (Left Side)
# --geometry=80x24+100+300: 
#   80 columns wide, 24 rows high
#   +100 pixels from left, +300 pixels from top
echo "[*] Launching Sender on Core 2..."
gnome-terminal --geometry=80x24+100+300 --title="SENDER (Core 2)" -- bash -c "taskset -c 2 ./sender; echo; echo 'Sender finished.'; read -p 'Press Enter to exit...'"

# 4. Wait 1 Second (Requested Delay)
echo "[*] Waiting 1 second..."
sleep 1

# 5. Launch RECEIVER (Right Side)
# --geometry=80x24+900+300:
#   Shifted +900 pixels to the right to place it next to the sender
echo "[*] Launching Receiver on Core 0..."
gnome-terminal --geometry=80x24+900+300 --title="RECEIVER (Core 0)" -- bash -c "taskset -c 0 ./receiver; echo; echo 'Receiver finished.'; read -p 'Press Enter to exit...'"

echo "[*] Demo running."