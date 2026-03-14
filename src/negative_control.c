#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "utils.h"

// --- Run Configuration ---
#define NUM_HUGE_PAGES 100
#define TARGET_SLICE 2
#define TARGET_SET 0x5A
#define DUMMY_SET  0x5B // Intentionally picking a DIFFERENT set for the dummy lines

#define CACHE_MISS_THRESHOLD 150 
#define CACHE_HIT_THRESHOLD 100
#define EVICTION_SET_SIZE (LLC_WAYS * 3) // 36 lines, as requested

#define TEST_ITERATIONS 100000

#define WARM_TLB(addr) do { \
    volatile uint8_t dummy = *((volatile uint8_t*)(((uintptr_t)(addr)) ^ 0x800)); \
    (void)dummy; \
} while(0)

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: sudo %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);
    
    printf("[*] Applying system optimizations on Core %d...\n", core_id);
    pin_cpu(core_id);
    set_realtime_priority();
    set_realtime_latency();

    printf("[*] Allocating memory and searching for negative control set...\n");
    uint8_t *base_addr = (uint8_t*)allocate_huge_pages(NUM_HUGE_PAGES);

    void *victim = NULL; 
    void *dummy_set[EVICTION_SET_SIZE];
    int dummy_idx = 0;

    // 1. Find 1 Victim Line and 36 Dummy Lines
    for (size_t i = 0; i < NUM_HUGE_PAGES; i++) {
        uint8_t *page_base = base_addr + (i * HUGE_PAGE_SIZE);
        uint64_t phys_base = virt_to_phys(page_base);
        
        if (phys_base == 0) continue;

        for (size_t offset = 0; offset < HUGE_PAGE_SIZE; offset += CACHE_LINE_SIZE) {
            void *virt_line = page_base + offset;
            uint64_t phys_line = phys_base + offset;

            uint64_t current_set = (phys_line >> SET_INDEX_SHIFT) & SET_INDEX_MASK;
            int current_slice = get_cache_slice(phys_line);
            
            // Look for our specific victim
            if (current_set == TARGET_SET && current_slice == TARGET_SLICE && victim == NULL) {
                victim = virt_line;
            }
            
            // Look for our dummy lines in a completely different set
            if (current_set == DUMMY_SET && current_slice == TARGET_SLICE && dummy_idx < EVICTION_SET_SIZE) {
                dummy_set[dummy_idx++] = virt_line;
            }

            // Stop searching once we have both
            if (victim != NULL && dummy_idx >= EVICTION_SET_SIZE) {
                goto search_done;
            }
        }
    }

search_done:
    if (victim == NULL || dummy_idx < EVICTION_SET_SIZE) {
        printf("[-] Could not find enough lines. Victim Found: %s, Dummies: %d/%d\n", 
               victim ? "Yes" : "No", dummy_idx, EVICTION_SET_SIZE);
        return 1;
    }

    printf("[+] Found Victim (Set 0x%x) and Dummy Set (Set 0x%x, %d lines)\n", 
           TARGET_SET, DUMMY_SET, EVICTION_SET_SIZE);
    printf("[*] Running %d test iterations...\n", TEST_ITERATIONS);
    
    int valid_hits = 0;
    int false_misses = 0; // We call these false misses because they shouldn't happen!
    uint64_t sum_hit_time = 0;
    uint64_t sum_post_dummy_time = 0;

    // 2. High-Speed Verification Loop
    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        maccess(victim);
        uint64_t hit_time = measure_access_time(victim);
        
        maccess(victim);

        // Sweep the dummy set (thrashing Set 0x5B, which should leave Set 0x5A completely alone)
        for (int sweep = 0; sweep < 3; sweep++) {
            for (int i = 0; i < EVICTION_SET_SIZE; i++) maccess(dummy_set[i]);
            for (int i = EVICTION_SET_SIZE - 1; i >= 0; i--) maccess(dummy_set[i]);
        }

        WARM_TLB(victim);
        uint64_t post_dummy_time = measure_access_time(victim);

        sum_hit_time += hit_time;
        sum_post_dummy_time += post_dummy_time;
        
        if (hit_time <= CACHE_HIT_THRESHOLD) valid_hits++;
        if (post_dummy_time >= CACHE_MISS_THRESHOLD) false_misses++;
    }

    // 3. Print Final Statistics
    double hit_pct = ((double)valid_hits / TEST_ITERATIONS) * 100.0;
    double false_miss_pct = ((double)false_misses / TEST_ITERATIONS) * 100.0;
    double survived_pct = 100.0 - false_miss_pct; // The metric we care about here

    printf("\n=========================================\n");
    printf("        NEGATIVE CONTROL STATISTICS      \n");
    printf("=========================================\n");
    printf("Baseline Hit Times (Target: <= %d)\n", CACHE_HIT_THRESHOLD);
    printf("  Mean Time:             %.2f cycles\n", (double)sum_hit_time / TEST_ITERATIONS);
    printf("  Within Threshold:      %d / %d (%.2f%%)\n", valid_hits, TEST_ITERATIONS, hit_pct);
    printf("-----------------------------------------\n");
    printf("Post-Dummy Times (Target: STILL <= %d)\n", CACHE_HIT_THRESHOLD);
    printf("  Mean Time:             %.2f cycles\n", (double)sum_post_dummy_time / TEST_ITERATIONS);
    printf("  Victim Survived:       %.2f%% of the time\n", survived_pct);
    printf("=========================================\n");

    return 0;
}