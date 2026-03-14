#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "utils.h"

// --- Run Configuration ---
#define NUM_HUGE_PAGES 100
#define TARGET_SLICE 2
#define TARGET_SET 0x5A
#define CACHE_MISS_THRESHOLD 150 
#define CACHE_HIT_THRESHOLD 100
#define EVICTION_SET_SIZE (LLC_WAYS * 3) 

// --- Testing Configuration ---
#define TEST_ITERATIONS 100000

// Helper macro to warm up the TLB without touching the victim cache line
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

    // 1. Allocate Memory & Find Set (Happens exactly ONCE)
    printf("[*] Allocating memory and searching for eviction set...\n");
    uint8_t *base_addr = (uint8_t*)allocate_huge_pages(NUM_HUGE_PAGES);

    int required_lines = EVICTION_SET_SIZE + 1;
    void* candidate_set[required_lines];
    int ev_idx = 0;

    for (size_t i = 0; i < NUM_HUGE_PAGES; i++) {
        uint8_t *page_base = base_addr + (i * HUGE_PAGE_SIZE);
        uint64_t phys_base = virt_to_phys(page_base);
        
        if (phys_base == 0) continue;

        for (size_t offset = 0; offset < HUGE_PAGE_SIZE; offset += CACHE_LINE_SIZE) {
            void *virt_line = page_base + offset;
            uint64_t phys_line = phys_base + offset;

            uint64_t current_set = (phys_line >> SET_INDEX_SHIFT) & SET_INDEX_MASK;
            
            if (current_set == TARGET_SET && get_cache_slice(phys_line) == TARGET_SLICE) {
                candidate_set[ev_idx++] = virt_line;
                if (ev_idx >= required_lines) goto search_done;
            }
        }
    }

search_done:
    if (ev_idx < required_lines) {
        printf("[-] Could not find enough lines. Found %d/%d.\n", ev_idx, required_lines);
        return 1;
    }

    // 2. High-Speed Verification Loop
    printf("[+] Found candidate set. Running %d test iterations...\n", TEST_ITERATIONS);
    
    void *victim = candidate_set[EVICTION_SET_SIZE]; 
    void **eviction_set = candidate_set; 

    int valid_hits = 0;
    int valid_misses = 0;
    uint64_t sum_hit_time = 0;
    uint64_t sum_miss_time = 0;

    for (int iter = 0; iter < TEST_ITERATIONS; iter++) {
        // Step A: Bring victim into cache and measure baseline
        maccess(victim);
        uint64_t hit_time = measure_access_time(victim);
        
        // Step B: Ensure victim is firmly cached
        maccess(victim);

        // Step C: Overwhelm RRIP by sweeping the eviction set
        for (int sweep = 0; sweep < 3; sweep++) {
            for (int i = 0; i < EVICTION_SET_SIZE; i++) maccess(eviction_set[i]);
            for (int i = EVICTION_SET_SIZE - 1; i >= 0; i--) maccess(eviction_set[i]);
        }

        // Step D: Warm TLB and measure the victim again
        WARM_TLB(victim);
        uint64_t miss_time = measure_access_time(victim);

        // Record Statistics
        sum_hit_time += hit_time;
        sum_miss_time += miss_time;
        if (hit_time <= CACHE_HIT_THRESHOLD) valid_hits++;
        if (miss_time >= CACHE_MISS_THRESHOLD) valid_misses++;
    }

    // 3. Print Final Statistics
    double hit_pct = ((double)valid_hits / TEST_ITERATIONS) * 100.0;
    double miss_pct = ((double)valid_misses / TEST_ITERATIONS) * 100.0;

    printf("\n=========================================\n");
    printf("             TEST STATISTICS             \n");
    printf("=========================================\n");
    printf("Total Iterations:        %d\n", TEST_ITERATIONS);
    printf("-----------------------------------------\n");
    printf("Baseline Hit Times (Target: <= %d)\n", CACHE_HIT_THRESHOLD);
    printf("  Mean Time:             %.2f cycles\n", (double)sum_hit_time / TEST_ITERATIONS);
    printf("  Within Threshold:      %d / %d (%.2f%%)\n", valid_hits, TEST_ITERATIONS, hit_pct);
    printf("-----------------------------------------\n");
    printf("Post-Eviction Times (Target: >= %d)\n", CACHE_MISS_THRESHOLD);
    printf("  Mean Time:             %.2f cycles\n", (double)sum_miss_time / TEST_ITERATIONS);
    printf("  Within Threshold:      %d / %d (%.2f%%)\n", valid_misses, TEST_ITERATIONS, miss_pct);
    printf("=========================================\n");

    return 0;
}