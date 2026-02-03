#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include <mastik/fr.h>
#include <mastik/util.h>

int main() {
    fr_t fr = fr_prepare();
    void *shared_mem = map_shared_rw(SHARED_FILE);
    
    volatile uint8_t *sync_flag = (uint8_t *)(shared_mem + OFFSET_FLAG);
    volatile uint8_t *msg_len   = (uint8_t *)(shared_mem + OFFSET_LEN);
    void *data_line = shared_mem + OFFSET_DATA;

    fr_monitor(fr, data_line);

    // 1. Read the length metadata FIRST
    // (In a real attack, we'd assume a fixed size or a header, 
    // but here we just read the memory location directly)
    int length_to_receive = *msg_len;
    
    // Safety check in case sender hasn't started yet (defaults to 0)
    if (length_to_receive == 0) {
        printf("[RECEIVER] Waiting for Sender metadata...\n");
        // Simple spin until sender writes a non-zero length
        while(*msg_len == 0) asm volatile("nop");
        length_to_receive = *msg_len;
    }

    printf("[RECEIVER] Metadata received. Expecting %d bytes.\n", length_to_receive);

    // 2. Allocate buffer dynamically
    char *final_message = malloc(length_to_receive + 1);
    memset(final_message, 0, length_to_receive + 1);

    // 3. Handshake
    printf("[RECEIVER] Setting READY flag...\n");
    *sync_flag = 1;

    uint64_t current_slot_start = wait_for_next_slot();
    
    printf("[RECEIVER] Listening...\n");
    printf("Raw: ");

    uint8_t current_byte = 0;
    int bit_index = 0;
    int char_index = 0;

    // Loop exactly as many times as needed (Length * 8 bits)
    int total_bits = length_to_receive * 8;

    for (int i = 0; i < total_bits; i++) {
        
        int hits = 0;
        
        // Skip first 10% of slot (alignment guard)
        while (rdtsc() < current_slot_start + (SLOT_DURATION/10)) asm volatile("nop");
        
        // Sample until 90% of slot
        uint64_t sampling_end = current_slot_start + SLOT_DURATION - (SLOT_DURATION/10);

        while (rdtsc() < sampling_end) {
            uint16_t res[1];
            fr_probe(fr, res);
            
            if (res[0] < CACHE_THRESHOLD) hits++;
            
            // Slow down probe slightly to allow sender refill
            for(volatile int k=0; k<2000; k++); 
        }

        // Decision (Oversampling Threshold)
        int bit = (hits > 10) ? 1 : 0;

        printf("%d", bit);
        fflush(stdout);

        current_byte = (current_byte << 1) | bit;
        bit_index++;

        if (bit_index == 8) {
            final_message[char_index++] = current_byte;
            printf("(%c) ", current_byte); // Print char immediately
            fflush(stdout);
            bit_index = 0;
            current_byte = 0;
        }

        while (rdtsc() < (current_slot_start + SLOT_DURATION)) asm volatile("nop");
        current_slot_start += SLOT_DURATION;
    }

    printf("\n\n[RECEIVER] Final Decoded String: \"%s\"\n", final_message);
    
    free(final_message);
    fr_release(fr);
    return 0;
}