#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "utils.h"

// --- Run Configuration ---
#define NUM_HUGE_PAGES 100
#define TARGET_SLICE 6
#define TARGET_SET 0x5A

// --- Precise Range Configuration ---
// Hits should be fast L1/L2/L3 accesses
#define HIT_MIN_CYCLES 0
#define HIT_MAX_CYCLES 110

// Misses should be true DRAM fetches, but not absurdly high
#define MISS_MIN_CYCLES HIT_MAX_CYCLES
#define MISS_MAX_CYCLES 800

// Anything above this is considered system noise (OS interrupt, TLB miss, etc.)
// and will be excluded from the mean calculations.
#define OUTLIER_THRESHOLD MISS_MAX_CYCLES 

#define EVICTION_SET_SIZE (LLC_WAYS * 3)

// --- Testing Configuration ---
#define TEST_ITERATIONS 100000

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
    ensure_huge_pages_available(NUM_HUGE_PAGES);
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

    // Statistics tracking variables
    int target_hits = 0;
    int target_misses = 0;
    int hit_outliers = 0;
    int miss_outliers = 0;
    
    uint64_t sum_hit_time = 0;
    uint64_t sum_miss_time = 0;
    
    int valid_hit_measurements = 0;
    int valid_miss_measurements = 0;

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
        warm_tlb(victim);
        uint64_t miss_time = measure_access_time(victim);

        // Record Statistics for Hit Time
        if (hit_time > OUTLIER_THRESHOLD) {
            hit_outliers++;
        } else {
            sum_hit_time += hit_time;
            valid_hit_measurements++;
            if (hit_time <= HIT_MAX_CYCLES) {
                target_hits++;
            }
        }

        // Record Statistics for Miss Time
        if (miss_time > OUTLIER_THRESHOLD) {
            miss_outliers++;
        } else {
            sum_miss_time += miss_time;
            valid_miss_measurements++;
            if (miss_time >= MISS_MIN_CYCLES && miss_time <= MISS_MAX_CYCLES) {
                target_misses++;
            }
        }
    }

    // 3. Print Final Statistics
    // Calculate percentages strictly out of non-outlier measurements
    double hit_pct = valid_hit_measurements > 0 ? 
                     ((double)target_hits / valid_hit_measurements) * 100.0 : 0.0;
    double miss_pct = valid_miss_measurements > 0 ? 
                      ((double)target_misses / valid_miss_measurements) * 100.0 : 0.0;
                      
    double mean_hit = valid_hit_measurements > 0 ? 
                      (double)sum_hit_time / valid_hit_measurements : 0.0;
    double mean_miss = valid_miss_measurements > 0 ? 
                       (double)sum_miss_time / valid_miss_measurements : 0.0;

    printf("\n=======================================================\n");
    printf("                  TEST STATISTICS                      \n");
    printf("=======================================================\n");
    printf("Total Iterations Executed: %d\n", TEST_ITERATIONS);
    printf("-------------------------------------------------------\n");
    printf("Baseline Hit Times (Target Range: %d - %d cycles)\n", HIT_MIN_CYCLES, HIT_MAX_CYCLES);
    printf("  Mean Time (sans outliers): %.2f cycles\n", mean_hit);
    printf("  Within Target Range:       %d / %d (%.2f%%)\n", target_hits, valid_hit_measurements, hit_pct);
    printf("  Outliers Ignored (>%d):  %d\n", OUTLIER_THRESHOLD, hit_outliers);
    printf("-------------------------------------------------------\n");
    printf("Post-Eviction Times (Target Range: %d - %d cycles)\n", MISS_MIN_CYCLES, MISS_MAX_CYCLES);
    printf("  Mean Time (sans outliers): %.2f cycles\n", mean_miss);
    printf("  Within Target Range:       %d / %d (%.2f%%)\n", target_misses, valid_miss_measurements, miss_pct);
    printf("  Outliers Ignored (>%d):  %d\n", OUTLIER_THRESHOLD, miss_outliers);
    printf("=======================================================\n");

    return 0;
}