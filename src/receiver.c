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
    volatile uint64_t *start_ts = (uint64_t *)(shared_mem + OFFSET_START_TIME);
    void *data_line = shared_mem + OFFSET_DATA;

    fr_monitor(fr, data_line);

    // 1. Get Length
    int length_to_receive = *msg_len;
    if (length_to_receive == 0) {
        printf("[RECEIVER] Waiting for metadata...\n");
        while(*msg_len == 0) asm volatile("nop");
        length_to_receive = *msg_len;
    }
    printf("[RECEIVER] Length: %d bytes.\n", length_to_receive);

    char *final_message = malloc(length_to_receive + 1);
    memset(final_message, 0, length_to_receive + 1);

    // 2. Handshake: Tell sender we are here
    *sync_flag = 1; 
    
    // 3. Wait for Sender to schedule the time (Flag becomes 2)
    printf("[RECEIVER] Waiting for schedule...\n");
    while (*sync_flag != 2) asm volatile("nop");

    // 4. Read the scheduled start time
    uint64_t start_time = *start_ts;
    printf("[RECEIVER] Start Time Locked: %lu\n", start_time);

    // 5. Spin exactly until that time
    while (rdtsc() < start_time) asm volatile("nop");

    // --- Transmission Started ---
    printf("[RECEIVER] Receiving...\n");
    uint64_t current_slot_start = start_time;
    
    uint8_t current_byte = 0;
    int bit_index = 0;
    int char_index = 0;

    int total_bits = length_to_receive * 8;

    for (int i = 0; i < total_bits; i++) {
        
        int hits = 0;
        
        // Wait 10% into slot
        while (rdtsc() < current_slot_start + (SLOT_DURATION/10)) asm volatile("nop");
        
        uint64_t sampling_end = current_slot_start + SLOT_DURATION - (SLOT_DURATION/10);

        while (rdtsc() < sampling_end) {
            uint16_t res[1];
            fr_probe(fr, res); // THIS ALSO FLUSHES
            
            if (res[0] < CACHE_THRESHOLD) hits++;
            
            for(volatile int k = 0; k < 2000; k++); 
        }

        // Decision
        int bit = (hits > 20) ? 1 : 0;

        printf("%d", bit);
        fflush(stdout);

        current_byte = (current_byte << 1) | bit;
        bit_index++;

        if (bit_index == 8) {
            if ('\0' == current_byte)
                final_message[char_index++] = '-';
            else
                final_message[char_index++] = current_byte;
            printf("(%c) ", current_byte);
            fflush(stdout);
            bit_index = 0;
            current_byte = 0;
        }

        while (rdtsc() < (current_slot_start + SLOT_DURATION)) asm volatile("nop");
        current_slot_start += SLOT_DURATION;
    }

    printf("\n\n[RECEIVER] Result: \"%s\"\n", final_message);
    
    free(final_message);
    fr_release(fr);
    return 0;
}