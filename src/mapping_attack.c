#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "utils.h"

#define NUM_PAGES 100
#define BOOTSTRAP_SET 0x5A
#define TARGET_EVICTION_COUNT (LLC_WAYS * 2) 
#define MISS_THRESHOLD 150
#define L2_WASH_LINES 40000 // 40,000 lines * 64 bytes = ~2.5MB (flushes 1.25MB L2 easily)

void *l2_wash_pool[L2_WASH_LINES];

// Initialize the L2 Wash Buffer
void init_l2_wash() {
    // Allocate 2 huge pages (4MB) just for wash data
    uint8_t *wash_memory = (uint8_t*)allocate_huge_pages(2);
    int ptr = 0;
    
    for (uint64_t off = 0; off < 2 * HUGE_PAGE_SIZE && ptr < L2_WASH_LINES; off += CACHE_LINE_SIZE) {
        int set = (off >> SET_INDEX_SHIFT) & SET_INDEX_MASK;
        // CRITICAL: We dodge our target L3 set! 
        if (set != BOOTSTRAP_SET) {
            l2_wash_pool[ptr++] = wash_memory + off;
        }
    }
    printf("[+] Initialized L2 Wash Buffer with %d set-dodging lines.\n", ptr);
}

// Function to push everything out of L2 and down into L3
void wash_l2() {
    for (int i = 0; i < L2_WASH_LINES; i++) {
        maccess(l2_wash_pool[i]);
    }
}

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
    // 1. Ensure firmly cached in L1/L2
    maccess(victim);
    maccess(victim);

    // 2. Wash L2 to push the victim down to the L3
    wash_l2();

    // 3. Thrash the set
    for (int sweep = 0; sweep < 3; sweep++) {
        // Bring candidates into L1/L2
        for (int i = 0; i < size; i++) maccess(group[i]);
        
        // Push candidates down to L3 so they conflict with the victim!
        wash_l2();
    }

    WARM_TLB(victim);
    return measure_access_time(victim) >= MISS_THRESHOLD;
}

bool test_group_robust(void *victim, void **group, int size) {
    int misses = 0;
    int tests = 5; 
    for(int t = 0; t < tests; t++) {
        if (test_group(victim, group, size)) misses++;
    }
    // Return true only if it consistently evicts (majority vote)
    return misses >= 3; 
}

// Phase 1: Robust Pruning Algorithm
bool find_eviction_set(void *victim, void **pool, int pool_size, void **eviction_set_out, int *out_size) {
    // 1. Verify the whole pool works as a baseline
    if (!test_group_robust(victim, pool, pool_size)) {
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

        if (test_group_robust(victim, working_set, w_size)) {
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

    init_l2_wash();
    
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

    printf("[*] Phase 1 & 2: Iterative Bootstrapping and Alignment...\n");
    
    int delta[NUM_PAGES];
    bool page_mapped[NUM_PAGES];
    for (int i = 0; i < NUM_PAGES; i++) page_mapped[i] = false;
    
    int mapped_count = 0;
    int victim_index = 0;

    // Seed random for the shuffler
    srand(1337); 

    // Keep trying new victims until all pages are mapped
    while (mapped_count < NUM_PAGES && victim_index < total_candidates) {
        void *victim = candidate_pool[victim_index];
        int victim_page = ((uintptr_t)victim - (uintptr_t)pages) / HUGE_PAGE_SIZE;
        
        // If the victim's page is already mapped, and we aren't on the very first run, skip it
        // to force the algorithm to explore different physical slices.
        if (mapped_count > 0 && page_mapped[victim_page]) {
            victim_index++;
            continue;
        }

        // Build a pool of candidates EXCLUDING the current victim
        void *current_pool[total_candidates];
        int current_pool_size = 0;
        for (int i = 0; i < total_candidates; i++) {
            if (candidate_pool[i] != victim) {
                current_pool[current_pool_size++] = candidate_pool[i];
            }
        }

        // SHUFFLE the pool to prevent the top-down linear bias
        for (int i = current_pool_size - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            void *temp = current_pool[i];
            current_pool[i] = current_pool[j];
            current_pool[j] = temp;
        }

        void *eviction_set[total_candidates];
        int ev_size = 0;
        
        printf("[*] Testing Victim %d (Page %d)... ", victim_index, victim_page);
        
        if (find_eviction_set(victim, current_pool, current_pool_size, eviction_set, &ev_size)) {
            printf("Found minimal set of %d lines!\n", ev_size);
            
            // If this is the absolute first success, anchor Page 0 to Delta 0
            if (mapped_count == 0) {
                delta[0] = 0;
                page_mapped[0] = true;
                mapped_count = 1;
            }

            // We need a known reference point to calculate deltas.
            // Find a line in this eviction set that belongs to an ALREADY MAPPED page.
            int ref_hash = -1;
            for (int i = 0; i < ev_size; i++) {
                int p = ((uintptr_t)eviction_set[i] - (uintptr_t)pages) / HUGE_PAGE_SIZE;
                if (page_mapped[p]) {
                    // We found a bridge! The actual physical slice of this eviction set 
                    // is represented by: Hash_Known(Bridge_Line) ^ Delta[p]
                    ref_hash = get_known_hash(eviction_set[i]) ^ delta[p];
                    break;
                }
            }

            // If we found a bridge to our existing mapped pages, calculate the new deltas
            if (ref_hash != -1) {
                for (int i = 0; i < ev_size; i++) {
                    int p = ((uintptr_t)eviction_set[i] - (uintptr_t)pages) / HUGE_PAGE_SIZE;
                    if (!page_mapped[p]) {
                        delta[p] = ref_hash ^ get_known_hash(eviction_set[i]);
                        page_mapped[p] = true;
                        mapped_count++;
                    }
                }
            } else {
                printf("    [!] Valid set, but no bridge to previously mapped pages. Skipping.\n");
            }
        } else {
            printf("Failed to find set.\n");
        }
        
        victim_index++;
    }

    // Print Results
    printf("\n=========================================\n");
    printf("        ALGEBRAIC PAGE ALIGNMENT         \n");
    printf("=========================================\n");
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_mapped[i]) {
            printf("Page %02d: Relative Slice Offset (Delta) = %d\n", i, delta[i]);
        }
    }
    printf("-----------------------------------------\n");
    printf("Successfully aligned %d/%d pages.\n", mapped_count, NUM_PAGES);
    printf("=========================================\n");

    free(candidate_pool);
    return 0;
}
