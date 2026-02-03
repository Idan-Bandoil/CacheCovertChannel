#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/util.h>

int main() {
    // 1. Setup
    void *shared_mem = map_shared_rw(SHARED_FILE);
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + FLAG_OFFSET);
    void *data_line = shared_mem + DATA_OFFSET;

    printf("[SENDER] Waiting for Receiver to raise READY flag...\n");

    // 2. HANDSHAKE: Wait until receiver writes 1 to offset 64
    while (*sync_flag != 1) {
        // Relax cpu while waiting
        asm volatile("nop"); 
    }
    printf("[SENDER] Receiver Ready! Aligning to Global Grid...\n");

    // 3. SYNC: Wait for the exact next slot boundary
    uint64_t current_slot_start = wait_for_next_slot();

    const char *msg = "HELLO";
    printf("[SENDER] Sending: %s\n", msg);

    // 4. Transmission Loop
    for (int i = 0; i < strlen(msg); i++) {
        char c = msg[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (c >> b) & 1;

            // We are currently at the start of a slot.
            // Transmit for exactly one slot duration.
            uint64_t end_of_slot = current_slot_start + SLOT_DURATION;

            while (rdtsc() < end_of_slot) {
                if (bit) maccess(data_line);
            }
            
            // Update our logical time for the next loop iteration
            current_slot_start = end_of_slot;
        }
    }

    // Reset flag so we can run again
    *sync_flag = 0;
    printf("\n[SENDER] Done.\n");
    return 0;
}