#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/util.h>

int main() {
    void *shared_mem = map_shared_rw(SHARED_FILE);
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + FLAG_OFFSET);
    void *data_line = shared_mem + DATA_OFFSET;

    printf("[SENDER] Waiting for Receiver...\n");
    while (*sync_flag != 1) asm volatile("nop");

    printf("[SENDER] Syncing to Global Grid...\n");
    uint64_t current_slot_start = wait_for_next_slot();

    const char *msg = "HELLO";
    printf("[SENDER] Transmitting: %s\n", msg);

    for (int i = 0; i < strlen(msg); i++) {
        char c = msg[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (c >> b) & 1;
            uint64_t end_of_slot = current_slot_start + SLOT_DURATION;

            // TRANSMIT LOOP
            while (rdtsc() < end_of_slot) {
                if (bit == 1) {
                    // Constant re-access to keep it in cache
                    maccess(data_line);
                } else {
                    // Busy wait for 0
                    asm volatile("nop");
                }
            }
            current_slot_start = end_of_slot;
        }
    }
    
    *sync_flag = 0; // Tell receiver we are done
    printf("\n[SENDER] Done.\n");
    return 0;
}