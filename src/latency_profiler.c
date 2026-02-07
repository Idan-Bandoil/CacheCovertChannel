#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// Use MASTIK's low-level timing primitives
// These provide the correct serialization (lfence) automatically
#include <mastik/low.h> 

#define SAMPLES 10000

// Define Cache Sizes to Thrash (Adjust for your specific CPU if needed)
// L1 is usually 32KB -> We use 64KB to be safe
#define L1_SIZE (100 * 1024)
#define L1_THRASH_SIZE (2 * L1_SIZE)

// L2 is usually 256KB -> We use 512KB to be safe
#define L2_SIZE (2 * 1024 * 1024)
#define L2_THRASH_SIZE (2 * L2_SIZE)

// Buffers for eviction
uint8_t *l1_garbage;
uint8_t *l2_garbage;

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

// Helper: Clear L1 by reading a buffer larger than L1
void flush_l1() {
    for (int i = 0; i < L1_THRASH_SIZE; i += 64) {
        maccess(l1_garbage + i);
    }
}

// Helper: Clear L2 by reading a buffer larger than L2
// Note: This also clears L1 automatically
void flush_l2() {
    for (int i = 0; i < L2_THRASH_SIZE; i += 64) {
        maccess(l2_garbage + i);
    }
}

int main(int argc, char **argv) {
    // 1. Setup Data
    uint8_t *target = (uint8_t *)malloc(4096);
    *target = 1; // dummy write
    
    // Allocate garbage buffers
    l1_garbage = (uint8_t *)malloc(L1_THRASH_SIZE);
    l2_garbage = (uint8_t *)malloc(L2_THRASH_SIZE);
    memset(l1_garbage, 1, L1_THRASH_SIZE);
    memset(l2_garbage, 1, L2_THRASH_SIZE);

    printf("[*] Profiling Memory Hierarchy Latency...\n");
    printf("[*] Samples: %d\n\n", SAMPLES);

    uint64_t start, end;
    uint64_t l1_total = 0, l2_total = 0, l3_total = 0, dram_total = 0;

    for (int i = 0; i < SAMPLES; i++) {
        
        // --- Measure L1 ---
        maccess(target); // Ensure it's in L1
        start = rdtscp();
        maccess(target);
        end = rdtscp();
        l1_total += (end - start);

        // --- Measure L2 ---
        maccess(target); // Bring to L1
        flush_l1();      // Evict from L1 -> Pushed to L2
        
        start = rdtscp();
        maccess(target); // Miss L1 -> Hit L2
        end = rdtscp();
        l2_total += (end - start);

        // --- Measure L3 ---
        maccess(target); // Bring to L1
        flush_l2();      // Evict from L1 & L2 -> Pushed to L3
        
        start = rdtscp();
        maccess(target); // Miss L1 -> Miss L2 -> Hit L3
        end = rdtscp();
        l3_total += (end - start);

        // --- Measure DRAM ---
        clflush(target); // Flush from ALL caches
        
        start = rdtscp();
        maccess(target); // Miss L1/L2/L3 -> Fetch from DRAM
        end = rdtscp();
        dram_total += (end - start);
    }

    printf("Results (Average Cycles):\n");
    printf("-------------------------\n");
    printf("L1 Cache : %.2f cycles\n", (double)l1_total / SAMPLES);
    printf("L2 Cache : %.2f cycles\n", (double)l2_total / SAMPLES);
    printf("L3 Cache : %.2f cycles\n", (double)l3_total / SAMPLES);
    printf("DRAM     : %.2f cycles\n", (double)dram_total / SAMPLES);
    printf("-------------------------\n");

    return 0;
}