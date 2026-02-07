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
#define THRESHOLD 15

// Helper for cycle counting
static inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char **argv) {
    printf("[*] Testing Cache Policy on L1 (Fixed: %d Ways)\n", WAYS);

    // 1. Allocate buffer for WAYS + 1 lines (The Set + The Intruder)
    size_t size = (WAYS + 2) * STRIDE;
    uint8_t *mem = (uint8_t *)malloc(size);
    memset(mem, 0xAA, size);

    void *lines[WAYS + 1];
    for (int i = 0; i <= WAYS; i++) {
        lines[i] = mem + (i * STRIDE);
    }

    int evictions[WAYS + 1]; // Histogram of who gets kicked out
    memset(evictions, 0, sizeof(evictions));

    // 2. Experiment Loop
    for (int s = 0; s < SAMPLES; s++) {
        
        // A. Flush everything to start clean
        for (int i = 0; i <= WAYS; i++) _mm_clflush(lines[i]);
        _mm_mfence();
        
        // B. FILL the set sequentially (0 -> 11)
        // If LRU: '0' is now the Oldest (Least Recently Used)
        // If MRU: '11' is the Newest (Most Recently Used)
        for (int i = 0; i < WAYS; i++) {
            volatile uint32_t val = *(volatile uint32_t *)lines[i];
            (void)val;
        }

        // C. ACCESS the INTRUDER (Index 12)
        // The cache is full (size 12). One of 0..11 MUST be evicted.
        volatile uint32_t val = *(volatile uint32_t *)lines[WAYS];
        (void)val;

        // D. MEASURE who is missing
        for (int i = 0; i < WAYS; i++) {
            _mm_lfence();
            uint64_t t1 = rdtsc();
            volatile uint32_t v = *(volatile uint32_t *)lines[i];
            uint64_t t2 = rdtsc();
            _mm_lfence();
            
            // If latency is high, this index was evicted
            if ((t2 - t1) > THRESHOLD) {
                evictions[i]++;
                // We break here because after the first miss we bring that value into the cache
                // and that evicts another cache line making the next misses irrelevant, as they
                // all evict the next one and we only want to measure which one was evicted first
                break; 
            }
        }
    }

    // 3. Results Analysis
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
