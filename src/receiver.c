#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/fr.h>
#include <mastik/util.h>

int main() {
    fr_t fr = fr_prepare();
    void *shared_mem = map_shared_rw(SHARED_FILE);
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + FLAG_OFFSET);
    void *data_line = shared_mem + DATA_OFFSET;

    fr_monitor(fr, data_line);

    printf("[RECEIVER] Setting READY flag...\n");
    *sync_flag = 1;

    uint64_t current_slot_start = wait_for_next_slot();
    
    printf("[RECEIVER] Listening...\n");
    printf("Raw: ");

    char final_message[20] = {0};
    uint8_t current_byte = 0;
    int bit_index = 0;
    int char_index = 0;

    // Loop for 40 bits
    for (int i = 0; i < 40; i++) {
        
        int hits = 0;
        int total_probes = 0;

        // Start sampling 10% into the slot (skip edge noise)
        while (rdtsc() < current_slot_start + (SLOT_DURATION/10)) asm volatile("nop");

        // Stop sampling 10% before the end
        uint64_t sampling_end = current_slot_start + SLOT_DURATION - (SLOT_DURATION/10);

        // --- OVERSAMPLING LOOP ---
        while (rdtsc() < sampling_end) {
            uint16_t res[1];
            fr_probe(fr, res); // This measures AND flushes
            
            if (res[0] < CACHE_THRESHOLD) {
                hits++;
            }
            total_probes++;

            // CRITICAL: Wait a tiny bit to let Sender put it back in!
            // If we probe too fast, we flush faster than sender can write.
            for(volatile int k=0; k<2000; k++); 
        }

        // --- DECISION LOGIC ---
        // If the Sender was active, we should see MANY hits.
        // If the Sender was idle, we might see 1 or 2 hits (noise), but not many.
        
        int bit = 0;
        // If > 10 hits detected in this window, it's definitely a 1.
        if (hits > 100) { 
            bit = 1;
        }

        // Debug output (Shows Hit Count vs Total Probes)
        // Uncomment this if you still have issues to see the "signal strength"
        // printf("[%d/%d]", hits, total_probes); 

        printf("%d", bit);
        fflush(stdout);

        current_byte = (current_byte << 1) | bit;
        bit_index++;

        if (bit_index == 8) {
            final_message[char_index++] = current_byte;
            printf("(%d) ", current_byte);
            fflush(stdout);
            bit_index = 0;
            current_byte = 0;
        }

        // Wait for strict slot end
        while (rdtsc() < (current_slot_start + SLOT_DURATION)) asm volatile("nop");
        current_slot_start += SLOT_DURATION;
    }

    printf("\n[RECEIVER] Result: %s\n", final_message);
    fr_release(fr);
    return 0;
}