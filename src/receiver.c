#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/fr.h>
#include <mastik/util.h>

int main() {
    // 1. Setup
    fr_t fr = fr_prepare();
    if (!fr) {
        fprintf(stderr, "Failed to init FR\n");
        return 1;
    }

    void *shared_mem = map_shared_rw(SHARED_FILE);
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + FLAG_OFFSET);
    void *data_line = shared_mem + DATA_OFFSET;

    fr_monitor(fr, data_line);

    // 2. HANDSHAKE
    printf("[RECEIVER] Setting READY flag and syncing...\n");
    *sync_flag = 1;

    // 3. SYNC
    uint64_t current_slot_start = wait_for_next_slot();
    uint64_t probe_offset = SLOT_DURATION / 2;
    
    printf("[RECEIVER] Receiving...\n");
    printf("Raw Data: "); 

    char final_message[6]; 
    memset(final_message, 0, sizeof(final_message));

    uint8_t current_byte = 0;
    int bit_index = 0;
    int char_index = 0;

    // Loop for 40 bits (5 characters)
    for (int i = 0; i < 40; i++) {
        
        // Wait for Middle of Slot
        while (rdtsc() < (current_slot_start + probe_offset)) {
            asm volatile("nop");
        }

        // Probe
        uint16_t res[1];
        fr_probe(fr, res);
        
        // Decide 1 or 0
        int bit = (res[0] < 120) ? 1 : 0;
        
        // Print the bit
        printf("%d", bit);
        fflush(stdout);

        // Accumulate bit into byte
        current_byte = (current_byte << 1) | bit;
        bit_index++;

        // End of Byte Reached?
        if (bit_index == 8) {
            final_message[char_index] = current_byte;
            
            // --- NEW: Print Decimal Value ---
            printf("(%d) ", current_byte); 
            fflush(stdout);
            // --------------------------------

            char_index++;
            
            // Reset counters
            bit_index = 0;
            current_byte = 0;
        }

        // Wait for End of Slot
        while (rdtsc() < (current_slot_start + SLOT_DURATION)) {
            asm volatile("nop");
        }
        
        current_slot_start += SLOT_DURATION;
    }
    
    printf("\n");
    printf("[RECEIVER] Decoded String: %s\n", final_message);

    fr_release(fr);
    return 0;
}