#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#define SAMPLES 1000000
#define HISTOGRAM_SIZE 500

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

static inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(int argc, char **argv) {
    // 1. Allocate a target variable
    // Align to 64 bytes to fit perfectly in a cache line
    uint8_t *data = (uint8_t *)malloc(4096);
    uint8_t *target = data + 64; 
    memset(data, 1, 4096);

    // Histogram to store cycle counts (0 to 499 cycles)
    uint64_t histogram[HISTOGRAM_SIZE] = {0};
    uint64_t total_cycles = 0;
    uint64_t min_cycles = 999999;
    uint64_t max_cycles = 0;

    // 2. Warm Up
    maccess(target);

    // 3. Measurement Loop
    for (int i = 0; i < SAMPLES; i++) {
        // Ensure data is in cache (Reload)
        maccess(target);
        
        // Serialize execution (prevent out-of-order execution affecting timing)
        _mm_lfence();
        
        uint64_t start = rdtsc();
        maccess(target); // The Access we want to measure
        uint64_t end = rdtsc();
        
        _mm_lfence();

        uint64_t latency = end - start;

        // Filter out noise (context switches)
        if (latency < HISTOGRAM_SIZE) {
            histogram[latency]++;
            total_cycles += latency;
            if (latency < min_cycles) min_cycles = latency;
            if (latency > max_cycles) max_cycles = latency;
        }
    }

    // 4. Calculate Stats
    double avg_latency = (double)total_cycles / SAMPLES;

    // Find the "Mode" (Most frequent latency)
    uint64_t mode_cycles = 0;
    uint64_t mode_count = 0;
    for (int i = 0; i < HISTOGRAM_SIZE; i++) {
        if (histogram[i] > mode_count) {
            mode_count = histogram[i];
            mode_cycles = i;
        }
    }

    // 5. Output Results
    printf("AVG:%.2f  MODE:%lu  MIN:%lu  MAX:%lu\n", 
            avg_latency, mode_cycles, min_cycles, max_cycles);

    // Optional: Print a mini ASCII histogram for visual verification
    // printf("\nDistribution (Cycles : Count):\n");
    // for (int i = 0; i < 150; i++) { // Only print first 150 buckets
    //    if (histogram[i] > SAMPLES / 1000) { // Only print significant buckets
    //        printf("%3d: %lu\n", i, histogram[i]);
    //    }
    // }

    free(data);
    return 0;
}
