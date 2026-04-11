#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "utils.h"

#define NUM_PAGES 100
#define BOOTSTRAP_SET 0x0
#define TARGET_EVICTION_COUNT (LLC_WAYS * 2) 
#define MISS_THRESHOLD 110

// Macro for TLB warmup
#define WARM_TLB(addr) do { \
    volatile uint8_t dummy = *((volatile uint8_t*)(((uintptr_t)(addr)) ^ 0x800)); \
    (void)dummy; \
} while(0)

// Helper: Calculate the slice hash using ONLY bits 0-20.
// Because it's an XOR function, the zeroed upper bits have no effect on the known bits' sum.
int get_known_hash(void *vaddr) {
    uint64_t addr = (uintptr_t)vaddr;
    uint64_t masked_addr = addr & ((1ULL << 21) - 1);
    return get_cache_slice(masked_addr);
}

// Helper: Tests if a group of addresses successfully evicts the victim
bool test_group(void *victim, void **group, int size) {
    // Ensure firmly cached
    maccess(victim);
    maccess(victim);
    maccess(victim);

    // Thrash the set to overwhelm RRIP
    for (int sweep = 0; sweep < 3; sweep++) {
        for (int i = 0; i < size; i++) maccess(group[i]);
        for (int i = size - 1; i >= 0; i--) maccess(group[i]);
    }

    return measure_access_time(victim) >= MISS_THRESHOLD;
}

// Phase 1: Robust Pruning Algorithm
bool find_eviction_set(void *victim, void **pool, int pool_size, void **eviction_set_out, int *out_size) {
    // 1. Verify the whole pool works as a baseline
    if (!test_group(victim, pool, pool_size)) {
        return false; 
    }

    // 2. Setup a working array we can shrink
    void *working_set[pool_size];
    for (int i = 0; i < pool_size; i++) working_set[i] = pool[i];
    int w_size = pool_size;

    // 3. Prune elements one by one
    for (int i = 0; i < w_size; ) {
        void *candidate = working_set[i];
        
        // Temporarily remove candidate by shifting left
        for (int j = i; j < w_size - 1; j++) working_set[j] = working_set[j + 1];
        w_size--;

        if (test_group(victim, working_set, w_size)) {
            // Still evicts! The candidate is unnecessary. Permanently discard.
            // Do NOT increment i, because a new element shifted into index i.
            if (w_size <= TARGET_EVICTION_COUNT) break; // Reached our target optimized size
        } else {
            // Eviction failed! The candidate is essential.
            // Put it back at index i.
            for (int j = w_size; j > i; j--) working_set[j] = working_set[j - 1];
            working_set[i] = candidate;
            w_size++;
            i++; // Move on to test the next element
        }
    }
    
    for (int i = 0; i < w_size; i++) eviction_set_out[i] = working_set[i];
    *out_size = w_size;
    return w_size >= LLC_WAYS;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);
    pin_cpu(core_id);
    set_realtime_priority();
    set_realtime_latency();

    uint8_t *pages = (uint8_t*)allocate_huge_pages(NUM_PAGES);
    
    // Efficient Candidate Construction (Direct Bitwise Mapping)
    int total_candidates = NUM_PAGES * 8;
    void **candidate_pool = malloc(sizeof(void*) * total_candidates);
    int pool_ptr = 0;

    for (int p = 0; p < NUM_PAGES; p++) {
        uint8_t *page_base = pages + (p * HUGE_PAGE_SIZE);
        
        // Loop through the 8 combinations of bits 18, 19, 20
        for (uint64_t variation = 0; variation < 8; variation++) {
            uint64_t offset = (variation << 18) | (BOOTSTRAP_SET << SET_INDEX_SHIFT);
            candidate_pool[pool_ptr++] = page_base + offset;
        }
    }

    printf("[*] Phase 1: Bootstrapping one eviction set from %d candidates...\n", pool_ptr);
    
    // We isolate one line as the victim, and use the rest as the pool
    void *victim = candidate_pool[0];
    void *eviction_set[total_candidates];
    int ev_size = 0;
    
    if (!find_eviction_set(victim, &candidate_pool[1], pool_ptr - 1, eviction_set, &ev_size)) {
        printf("[-] Failed to find initial eviction set. The pool itself does not evict the victim.\n");
        return 1;
    }
    printf("[+] Found working bootstrap eviction set of %d lines!\n", ev_size);

    // Phase 2: Algebraic Page Alignment
    int delta[NUM_PAGES];
    bool page_mapped[NUM_PAGES];
    for (int i = 0; i < NUM_PAGES; i++) page_mapped[i] = false;

    // Use the victim's page (Page 0) as our reference point
    int ref_hash = get_known_hash(victim);
    delta[0] = 0;
    page_mapped[0] = true;

    printf("[*] Phase 2: Aligning pages via XOR Delta logic...\n");
    for (int i = 0; i < ev_size; i++) {
        int p = ((uintptr_t)eviction_set[i] - (uintptr_t)pages) / HUGE_PAGE_SIZE;
        if (!page_mapped[p]) {
            // Delta_p = Hash_Known(Victim) ^ Hash_Known(Current_Eviction_Line)
            delta[p] = ref_hash ^ get_known_hash(eviction_set[i]);
            page_mapped[p] = true;
        }
    }

    // Print Results
    printf("\n=========================================\n");
    printf("        ALGEBRAIC PAGE ALIGNMENT         \n");
    printf("=========================================\n");
    int mapped_count = 0;
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_mapped[i]) {
            printf("Page %02d: Relative Slice Offset (Delta) = %d\n", i, delta[i]);
            mapped_count++;
        }
    }
    printf("-----------------------------------------\n");
    printf("Successfully aligned %d/%d pages.\n", mapped_count, NUM_PAGES);
    printf("=========================================\n");

    free(candidate_pool);
    return 0;
}
