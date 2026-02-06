#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

// Standard Intel L1 is 8-way associative
#define WAYS 30
#define TRIALS 10000
#define CACHE_HIT_THRESHOLD 80 // Cycles (Adjust based on previous experiment)

// Striding by Page Size (4KB) ensures we hit the same Set Index 
// in VIPT (Virtually Indexed Physically Tagged) L1 caches.
#define STRIDE 4096 

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

static inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

// Flush a line from cache
static inline void clflush(void *p) {
    _mm_clflush(p);
}

int main(int argc, char **argv) {
    // 1. Prepare Memory
    // We need (WAYS + 1) lines. 
    // We allocate a large buffer to jump by 4KB strides.
    size_t buffer_size = (WAYS + 1) * STRIDE;
    uint8_t *buffer = (uint8_t *)malloc(buffer_size);
    memset(buffer, 1, buffer_size);

    // Array to hold our conflicting pointers
    uint8_t *lines[WAYS + 1];
    for (int i = 0; i <= WAYS; i++) {
        lines[i] = buffer + (i * STRIDE);
    }

    // Statistics: Count how many times each index was evicted
    int evictions[WAYS] = {0}; 

    printf("[*] Testing Cache Replacement Policy (L1 - 8 Ways)\n");
    printf("[*] Hypothesis: If LRU, Index 0 (Oldest) should be evicted most.\n\n");

    // 2. Experiment Loop
    for (int t = 0; t < TRIALS; t++) {
        
        // A. Flush everything to start clean
        for (int i = 0; i <= WAYS; i++) {
            clflush(lines[i]);
        }
        _mm_mfence();

        // B. FILL the Set (Access 0 to 7)
        // Order: 0 -> 1 -> ... -> 7
        // Result: 0 is LRU (Oldest), 7 is MRU (Newest)
        for (int i = 0; i < WAYS; i++) {
            maccess(lines[i]);
        }
        _mm_mfence();

        // C. EVICT (Access the 9th line, Index 8)
        // This forces *someone* out.
        maccess(lines[WAYS]);
        _mm_mfence();

        // D. PROBE (Who is missing?)
        // We check 0 to 7.
        for (int i = 0; i < WAYS; i++) {
            // Measure latency
            _mm_lfence();
            uint64_t start = rdtsc();
            maccess(lines[i]);
            uint64_t end = rdtsc();
            _mm_lfence();
            
            uint64_t lat = end - start;

            // If latency is high, it was evicted (Miss)
            if (lat > CACHE_HIT_THRESHOLD) {
                evictions[i]++;
                // it was a miss, so we brought it into the cache and evicted someone.
                // next misses are therefore irrelevant so we break here
                break; 
            }
        }
    }

    // 3. Print Results (Histogram)
    printf("%-10s %-15s %-15s\n", "Index", "Eviction Count", "Probability");
    printf("--------------------------------------------\n");
    
    for (int i = 0; i < WAYS; i++) {
        double prob = (double)evictions[i] * 100.0 / TRIALS;
        
        // Highlight the LRU candidate
        const char *note = "";
        if (i == 0) note = "(LRU Candidate)";
        if (i == WAYS - 1) note = "(MRU Candidate)";

        printf("%-10d %-15d %5.1f%%  %s\n", i, evictions[i], prob, note);
    }

    printf("--------------------------------------------\n");

    // 4. Basic Analysis
    if (evictions[0] > (TRIALS * 0.9)) {
        printf("[RESULT] High match for LRU Policy.\n");
    } else if (evictions[0] < (TRIALS * 0.1)) {
        printf("[RESULT] Does NOT look like LRU.\n");
    } else {
        printf("[RESULT] Mixed behavior (Pseudo-LRU or Noise).\n");
    }

    free(buffer);
    return 0;
}
