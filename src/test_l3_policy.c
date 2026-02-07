#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
#include <mastik/util.h>

// --- Configuration from lscpu --cache ---
#define L3_WAYS 12
#define L3_SIZE (24 * 1024 * 1024)
#define BUFFER_SIZE (L3_SIZE * 4) // 4x larger to ensure we find collisions

// L3 Hit ~40-60 cycles. DRAM ~200+ cycles.
// Threshold of 150 safely distinguishes L3 Hit vs DRAM (Miss)
#define THRESHOLD 25 
#define SAMPLES 10000

static inline uint64_t rdtsc_local() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

int is_cache_miss(void *p) {
    _mm_lfence();
    uint64_t t1 = rdtsc_local();
    maccess(p);
    uint64_t t2 = rdtsc_local();
    _mm_lfence();
    return (t2 - t1) > THRESHOLD;
}

// ---------------------------------------------------------
// ALGORITHM: Find Eviction Set (Expand & Prune)
// ---------------------------------------------------------
// This function finds 'n' addresses that conflict with 'victim'
int find_eviction_set(void **conflict_set, void *victim, void *buffer_start, size_t buffer_len, int required_ways) {
    int count = 0;
    void **pool = malloc(sizeof(void*) * 5000); // Temporary pool
    int pool_size = 0;
    
    // --- Step 1: Expansion (Fill the pool until victim is evicted) ---
    // We stride by 64 bytes (cache line) to find candidates
    uint8_t *ptr = (uint8_t *)buffer_start;
    
    // Prime the victim
    maccess(victim);
    
    int found_conflict = 0;
    for (size_t i = 0; i < buffer_len; i += 64) {
        void *candidate = ptr + i;
        if (candidate == victim) continue;

        // Add to pool
        pool[pool_size++] = candidate;
        maccess(candidate);

        // Check if victim is evicted
        if (is_cache_miss(victim)) {
            found_conflict = 1;
            break;
        }
        
        if (pool_size >= 4000) break; // Safety break
    }

    if (!found_conflict) {
        free(pool);
        return 0; 
    }

    // --- Step 2: Pruning (Remove useless lines) ---
    // We iterate backwards. If removing a line keeps the victim evicted, that line was useless.
    // If removing a line makes the victim a HIT, that line is part of the conflict set.
    
    int set_idx = 0;
    
    // We need to reduce 'pool' down to exactly 'required_ways'
    // This simple prune approach just finds the minimal set.
    for (int i = 0; i < pool_size; i++) {
        void *removed = pool[i];
        
        // Try accessing everyone EXCEPT 'removed'
        maccess(victim);
        for (int j = 0; j < pool_size; j++) {
            if (i == j) continue; // Skip the one we are testing removal of
            maccess(pool[j]);
        }
        
        // If victim is NOW a hit, it means 'removed' was critical. Keep it.
        if (!is_cache_miss(victim)) {
            conflict_set[set_idx++] = removed;
            if (set_idx >= required_ways) break;
        }
    }
    
    free(pool);
    return set_idx;
}

int main() {
    srand(time(NULL));
    printf("[*] Preparing L3 Experiment (Ways: %d, Size: 24MB)\n", L3_WAYS);
    printf("[*] Allocating Large Buffer (This may take a moment)...\n");

    // Allocate huge buffer to find conflicts
    uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
    memset(buffer, 1, BUFFER_SIZE);

    // We need WAYS + 1 (The Set + Intruder) = 13 lines
    int TARGET_SET_SIZE = L3_WAYS + 1;
    void *eviction_set[TARGET_SET_SIZE];
    void *victim = buffer + 4096; // Pick an arbitrary victim

    printf("[*] Searching for Conflict Set using Expand-and-Prune...\n");
    
    // Retry loop in case of noise
    int found = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        // Pick a random new victim each time to traverse slices
        victim = buffer + (rand() % (BUFFER_SIZE / 2)); 
        
        found = find_eviction_set(eviction_set, victim, buffer, BUFFER_SIZE, TARGET_SET_SIZE);
        if (found == TARGET_SET_SIZE) {
            printf("[SUCCESS] Found %d conflicting addresses for L3!\n", found);
            break;
        }
        printf("... Attempt %d yielded %d addresses (need %d). Retrying.\n", attempt+1, found, TARGET_SET_SIZE);
    }

    if (found != TARGET_SET_SIZE) {
        printf("[FAIL] Could not find a clean eviction set. System too noisy.\n");
        return 1;
    }

    int evictions[TARGET_SET_SIZE];
    memset(evictions, 0, sizeof(evictions));

    printf("[*] Running Replacement Policy Test (%d Samples)...\n", SAMPLES);

    for (int s = 0; s < SAMPLES; s++) {
        // 1. Flush All
        for (int i = 0; i < TARGET_SET_SIZE; i++) _mm_clflush(eviction_set[i]);
        _mm_mfence();

        // 2. Fill Set (0 to 11)
        for (int i = 0; i < L3_WAYS; i++) {
            maccess(eviction_set[i]);
        }
        _mm_mfence();

        // 3. Intruder (Index 12)
        maccess(eviction_set[L3_WAYS]);
        _mm_mfence();

        for (int target = 0; target < TARGET_SET_SIZE; target++) {
            if (is_cache_miss(eviction_set[target])) {
                evictions[target]++;
                break;
            }
        }

    }

    // --- Output ---
    printf("\n%-8s %-12s %-10s\n", "Index", "Evictions", "Prob");
    printf("------------------------------------\n");
    for (int i = 0; i < TARGET_SET_SIZE; i++) {
        double prob = (double)evictions[i] * 100.0 / SAMPLES;
        char note[50] = "";
        
        if (i == 0) strcpy(note, "<- Oldest (LRU?)");
        if (i == L3_WAYS - 1) strcpy(note, "<- Newest (MRU?)");
        if (i == L3_WAYS) strcpy(note, "<- Intruder (Most Recent)");

        printf("Line %-3d %-12d %5.1f%% %s\n", i, evictions[i], prob, note);
    }

    free(buffer);
    return 0;
}