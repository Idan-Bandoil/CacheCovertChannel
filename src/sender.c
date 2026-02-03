#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/util.h>

// --- CHANGE YOUR MESSAGE HERE ---
#define SECRET_MESSAGE "This is a dynamic test message!" 
// --------------------------------

int main() {
    void *shared_mem = map_shared_rw(SHARED_FILE);
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + OFFSET_FLAG);
    volatile uint8_t *msg_len   = (uint8_t *)(shared_mem + OFFSET_LEN);
    void *data_line = shared_mem + OFFSET_DATA;

    const char *msg = SECRET_MESSAGE;
    int length = strlen(msg);

    // 1. Write the length to shared memory so receiver knows how much to read
    printf("[SENDER] Message: \"%s\"\n", msg);
    printf("[SENDER] Length: %d bytes. Posting metadata...\n", length);
    *msg_len = (uint8_t)length;

    // 2. Wait for Receiver to acknowledge and be ready
    printf("[SENDER] Waiting for Receiver...\n");
    while (*sync_flag != 1) asm volatile("nop");

    // 3. Sync and Start
    printf("[SENDER] Receiver ready. Transmitting...\n");
    uint64_t current_slot_start = wait_for_next_slot();

    // Loop through every byte of the dynamic message
    for (int i = 0; i < length; i++) {
        char c = msg[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (c >> b) & 1;
            uint64_t end_of_slot = current_slot_start + SLOT_DURATION;

            while (rdtsc() < end_of_slot) {
                if (bit == 1) {
                    maccess(data_line);
                } else {
                    asm volatile("nop");
                }
            }
            current_slot_start = end_of_slot;
        }
    }
    
    *sync_flag = 0; // Reset flag
    printf("\n[SENDER] Finished.\n");
    return 0;
}