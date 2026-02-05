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
    volatile uint64_t *start_ts = (uint64_t *)(shared_mem + OFFSET_START_TIME);
    void *data_line = shared_mem + OFFSET_DATA;

    const char *msg = SECRET_MESSAGE;
    int length = strlen(msg);

    // 1. Post Length
    printf("[SENDER] Message: \"%s\"\n", msg);
    *msg_len = (uint8_t)length;

    // 2. Wait for Receiver (Handshake)
    printf("[SENDER] Waiting for Receiver...\n");
    while (*sync_flag != 1) asm volatile("nop");

    // 3. SCHEDULE START TIME
    // We set the start time to be NOW + 20 Slots (approx 200ms)
    // This huge buffer gives both CPUs plenty of time to sync up.
    uint64_t future_start = rdtsc() + (20 * SLOT_DURATION);
    
    // Write this time to shared memory so receiver knows it
    *start_ts = future_start;
    
    // Signal that schedule is set (Reuse flag: 2 means "Time is set")
    *sync_flag = 2;

    printf("[SENDER] Scheduled Start: %lu. Waiting...\n", future_start);

    // 4. Spin until the exact start second
    while (rdtsc() < future_start) asm volatile("nop");

    printf("[SENDER] Transmitting...\n");

    uint64_t current_slot_start = future_start;

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
    
    *sync_flag = 0; 
    printf("\n[SENDER] Finished.\n");
    return 0;
}