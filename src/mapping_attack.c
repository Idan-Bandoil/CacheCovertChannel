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
bool test_group(uint8_t *victim, uint8_t **group, int size) {
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

bool test_group_robust(uint8_t *victim, uint8_t **group, int size) {
    int misses = 0;
    int tests = 5; 
    for(int t = 0; t < tests; t++) {
        if (test_group(victim, group, size)) misses++;
    }
    // Return true only if it consistently evicts (majority vote)
    return misses >= 3; 
}

// Phase 1: Robust Pruning Algorithm
bool find_eviction_set(uint8_t *victim, uint8_t **pool, int pool_size, uint8_t **eviction_set_out, int *out_size) {
    // 1. Verify the whole pool works as a baseline
    if (!test_group_robust(victim, pool, pool_size)) {
        return false; 
    }

    // 2. Setup a working array we can shrink
    uint8_t *working_set[pool_size];
    for (int i = 0; i < pool_size; i++) working_set[i] = pool[i];
    int w_size = pool_size;

    // 3. Prune elements one by one
    for (int i = 0; i < w_size; ) {
        uint8_t *candidate = working_set[i];
        
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

int create_candidate_pool(int set_idx, int required_candidates, uint8_t* huge_pages_base, 
    uint8_t*** candidate_pool_out)
{
    uint8_t **candidate_pool = malloc(sizeof(uint8_t*) * required_candidates);
    int pool_index = 0;

    for (int p = 0; (p < NUM_PAGES) && (pool_index < required_candidates); p++) {
        uint8_t *page_base = huge_pages_base + (p * HUGE_PAGE_SIZE);
        
        // Loop through the 8 combinations of bits 18, 19, 20
        for (uint64_t variation = 0; variation < 8; variation++) {
            uint64_t offset = (variation << 18) | (set_idx << SET_INDEX_SHIFT);
            candidate_pool[pool_index] = page_base + offset;
            pool_index++;
        }
    }

    *candidate_pool_out = candidate_pool;

    return (pool_index < required_candidates) ? 0 : 1;
}

// Helper: Finds a bridge to an already mapped page and calculates Deltas for new pages
bool calculate_page_deltas(uint8_t **eviction_set, int ev_size, uint8_t *pages, bool *page_mapped, int *delta, int *mapped_count) {
    int ref_hash = -1;

    // 1. Find a line in this eviction set that belongs to an ALREADY MAPPED page.
    for (int i = 0; i < ev_size; i++) {
        int p = ((uintptr_t)eviction_set[i] - (uintptr_t)pages) / HUGE_PAGE_SIZE;
        if (page_mapped[p]) {
            // We found a bridge! The actual physical slice of this eviction set 
            // is represented by: Hash_Known(Bridge_Line) ^ Delta[p]
            ref_hash = get_known_hash(eviction_set[i]) ^ delta[p];
            break;
        }
    }

    // 2. If we found a bridge to our existing mapped pages, calculate the new deltas
    if (ref_hash != -1) {
        for (int i = 0; i < ev_size; i++) {
            int p = ((uintptr_t)eviction_set[i] - (uintptr_t)pages) / HUGE_PAGE_SIZE;
            if (!page_mapped[p]) {
                delta[p] = ref_hash ^ get_known_hash(eviction_set[i]);
                page_mapped[p] = true;
                (*mapped_count)++; // Increment the tracked count via pointer
            }
        }
        return true; 
    }

    return false; // No bridge to previously mapped pages
}

// Phase 1 & 2: Iterative Bootstrapping and Algebraic Alignment
int bootstrap_page_alignment(uint8_t **candidate_pool, int total_candidates, uint8_t *pages, bool *page_mapped, int *delta) {
    int mapped_count = 0;
    int victim_index = 0;

    // Keep trying new victims until all pages are mapped
    while (mapped_count < NUM_PAGES && victim_index < total_candidates) {
        uint8_t *victim = candidate_pool[victim_index];
        int victim_page = ((uintptr_t)victim - (uintptr_t)pages) / HUGE_PAGE_SIZE;
        
        // If the victim's page is already mapped, and we aren't on the very first run, skip it
        // to force the algorithm to explore different physical slices.
        if (mapped_count > 0 && page_mapped[victim_page]) {
            victim_index++;
            continue;
        }

        // Build a pool of candidates EXCLUDING the current victim
        uint8_t *current_pool[total_candidates];
        int current_pool_size = 0;
        for (int i = 0; i < total_candidates; i++) {
            if (candidate_pool[i] != victim) {
                current_pool[current_pool_size++] = candidate_pool[i];
            }
        }

        shuffle_addresses_randomly(current_pool, current_pool_size);

        uint8_t *eviction_set[total_candidates];
        int ev_size = 0;
        
        printf("[*] Testing Victim %d (Page %d)... ", victim_index, victim_page);
        if (!find_eviction_set(victim, current_pool, current_pool_size, eviction_set, &ev_size)) {
            victim_index++;
            continue;
        }
        printf("Found minimal set of %d lines!\n", ev_size);
        
        // If this is the absolute first success, anchor Page 0 to Delta 0
        if (mapped_count == 0) {
            delta[0] = 0;
            page_mapped[0] = true;
            mapped_count = 1;
        }

        // Attempt to bridge and calculate new deltas
        if (!calculate_page_deltas(eviction_set, ev_size, pages, page_mapped, delta, &mapped_count)) {
            printf("    [!] Valid set, but no bridge to previously mapped pages. Skipping.\n");
        }
        
        victim_index++;
    }

    return mapped_count;
}

void print_page_alignments(bool* page_mapped, int* delta, int mapped_count)
{
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
}

// Phase 3: O(1) Mathematical Bucketing
// Instantly generates a perfect eviction set for any target set and slice using the recovered deltas.
int build_target_eviction_set(int target_set, int target_slice, uint8_t *pages, int *delta, uint8_t **eviction_set_out) {
    int found_perfect = 0;

    // Scan through our aligned pages
    for (int p = 0; p < NUM_PAGES && found_perfect < TARGET_EVICTION_COUNT; p++) {
        uint8_t *page_base = pages + (p * HUGE_PAGE_SIZE);
        
        // Check all 8 lines in this page that map to the target_set
        for (uint64_t variation = 0; variation < 8; variation++) {
            uint64_t offset = (variation << 18) | (target_set << SET_INDEX_SHIFT);
            uint8_t *candidate = page_base + offset;
            
            // Calculate the true physical slice using our recovered Delta!
            int relative_slice = get_known_hash(candidate) ^ delta[p];
            
            if (relative_slice == target_slice) {
                eviction_set_out[found_perfect++] = candidate;
                
                // Break out of the variation loop!
                // We only want ONE line per huge page to prevent L2 cache clustering
                break; 
            }
        }
    }
    
    // Return the number of matching lines found
    return found_perfect;
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
    
    int total_candidates = NUM_PAGES * 8;
    uint8_t **candidate_pool = malloc(sizeof(uint8_t*) * total_candidates);
    if (!create_candidate_pool(BOOTSTRAP_SET, total_candidates, pages, &candidate_pool))
    {
        printf("create_candidate_pool() failed to find enough candidates!\n");
        return 1;
    }
    
    int delta[NUM_PAGES];
    bool page_mapped[NUM_PAGES];
    for (int i = 0; i < NUM_PAGES; i++) page_mapped[i] = false;
    
    int mapped_count = bootstrap_page_alignment(candidate_pool, total_candidates, pages, page_mapped, delta);

    print_page_alignments(page_mapped, delta, mapped_count);

    // =========================================
    // PHASE 3: O(1) MATHEMATICAL BUCKETING
    // =========================================
    printf("[*] Phase 3: Instantly generating a perfect eviction set via math...\n");

    int target_set = 0x8;   // Pick ANY set (0 to 4095)
    int target_slice = 2;     // Pick ANY slice (0 to 7)
    
    uint8_t *perfect_eviction_set[TARGET_EVICTION_COUNT];
    
    int found_perfect = build_target_eviction_set(target_set, target_slice, pages, delta, perfect_eviction_set);

    if (found_perfect == TARGET_EVICTION_COUNT) {
        printf("[+] Successfully generated %d lines for Set 0x%03X, Slice %d!\n", 
               found_perfect, target_set, target_slice);
               
        uint8_t *test_victim = perfect_eviction_set[0];
        
        // We use the remaining 23 lines as the thrashing group
        if (test_group_robust(test_victim, perfect_eviction_set + 1, found_perfect - 1)) {
            printf("[+] VERIFIED: The mathematical eviction set triggers L3 misses!\n");
        } else {
            printf("[-] The set was mathematically generated, but hardware absorbed the thrash.\n");
        }
    } else {
        printf("[-] Could not find enough lines. (Found %d, need %d). Allocate more NUM_PAGES.\n", 
               found_perfect, TARGET_EVICTION_COUNT);
    }

    free(candidate_pool);
    return 0;
}
