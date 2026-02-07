#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <x86intrin.h>
#include <mastik/util.h>

// Derived from your lscpu --cache output
#define WAYS 12

#define PAGE_SIZE 4096
#define STRIDE PAGE_SIZE
#define SAMPLES 20000

// Thresholds defined based on timing experiment
#define PCORE_L1_THRESHOLD 15
#define ECORE_L1_THRESHOLD 25

static inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <core_id>\n", argv[0]);
        return 1;
    }

    int core_id = atoi(argv[1]);
    uint64_t threshold = 0;

    if (core_id >= 0 && core_id <= 11) {
        threshold = PCORE_L1_THRESHOLD;
        printf("[Config] Core %d is a P-Core.\n", core_id);
    } else if (core_id >= 12 && core_id <= 19) {
        threshold = ECORE_L1_THRESHOLD;
        printf("[Config] Core %d is an E-Core.\n", core_id);
    } else {
        // Fallback for unexpected core IDs
        threshold = PCORE_L1_THRESHOLD;
        printf("[Warning] Core %d is outside known ranges (0-19). Defaulting to P-Core threshold.\n", core_id);
    }

    printf("[*] Testing Cache Policy on L1 (Fixed: %d Ways) | Threshold: %lu cycles | Samples %u\n", WAYS, threshold, SAMPLES);

    // 2. Allocate buffer for WAYS + 1 lines (The Set + The Intruder)
    size_t size = (WAYS + 1) * STRIDE;
    uint8_t *mem = (uint8_t *)malloc(size);
    if (!mem) {
        perror("malloc");
        return 1;
    }
    memset(mem, 0xAA, size);

    void *lines[WAYS + 1];
    for (int i = 0; i <= WAYS; i++) {
        lines[i] = mem + (i * STRIDE);
    }

    int evictions[WAYS + 1]; // Histogram of who gets kicked out
    memset(evictions, 0, sizeof(evictions));

    // 3. Experiment Loop
    for (int s = 0; s < SAMPLES; s++) {
        
        // A. Flush everything to start clean
        for (int i = 0; i <= WAYS; i++) _mm_clflush(lines[i]);
        _mm_mfence();
        
        // B. FILL the set sequentially (0 -> 11)
        for (int i = 0; i < WAYS; i++) {
            volatile uint32_t val = *(volatile uint32_t *)lines[i];
            (void)val;
        }
        // If LRU: '0' is now the Oldest (Least Recently Used)
        // If MRU: '11' is the Newest (Most Recently Used)

        // C. Access the "intruder" (Index 12)
        // The cache is full (size 12). One of 0..11 MUST be evicted.
        volatile uint32_t val = *(volatile uint32_t *)lines[WAYS];
        (void)val;

        // D. Measure who is missing
        for (int i = 0; i < WAYS; i++) {
            _mm_lfence();
            uint64_t t1 = rdtsc();
            volatile uint32_t v = *(volatile uint32_t *)lines[i];
            (void)v; // Prevent compiler from optimizing out the read
            uint64_t t2 = rdtsc();
            _mm_lfence();
            
            // Use the core-specific threshold
            if ((t2 - t1) > threshold) {
                evictions[i]++;
                // We break here because after the first miss we bring that value into the cache
                // and that evicts another cache line making the next misses irrelevant.
                break; 
            }
        }
    }

    // 4. Results Analysis
    printf("\n--- Results (Eviction Probability) ---\n");
    printf("%-8s %-12s %-10s\n", "Index", "Evictions", "Prob");
    
    for (int i = 0; i < WAYS; i++) {
        double prob = (double)evictions[i] * 100.0 / SAMPLES;
        char note[32] = "";
        
        if (i == 0) strcpy(note, "<- LRU Candidate (Oldest)");
        if (i == WAYS-1) strcpy(note, "<- MRU Candidate (Newest)");

        printf("Line %-3d %-12d %5.1f%% %s\n", i, evictions[i], prob, note);
    }

    free(mem);
    return 0;
}