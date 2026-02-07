#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>
#include <mastik/low.h> 

#define SAMPLES 10000

// --- Configuration ---
#define L1_SIZE (100 * 1024)
#define L1_THRASH_SIZE (2 * L1_SIZE)

#define L2_SIZE (2 * 1024 * 1024)
#define L2_THRASH_SIZE (2 * L2_SIZE)

uint8_t *l1_garbage;
uint8_t *l2_garbage;

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

void flush_l1() {
    for (int i = 0; i < L1_THRASH_SIZE; i += 64) maccess(l1_garbage + i);
}

void flush_l2() {
    for (int i = 0; i < L2_THRASH_SIZE; i += 64) maccess(l2_garbage + i);
}

int main(int argc, char **argv) {
    uint8_t *target = (uint8_t *)malloc(4096);
    *target = 1; 
    
    l1_garbage = (uint8_t *)malloc(L1_THRASH_SIZE);
    l2_garbage = (uint8_t *)malloc(L2_THRASH_SIZE);
    memset(l1_garbage, 1, L1_THRASH_SIZE);
    memset(l2_garbage, 1, L2_THRASH_SIZE);

    printf("[*] Profiling Memory Hierarchy Latency...\n");
    printf("[*] Samples: %d\n\n", SAMPLES);

    uint64_t start, end, lat;
    
    // 1. Totals
    uint64_t l1_total = 0, l2_total = 0, l3_total = 0, dram_total = 0;
    
    // 2. Counters (to track how many valid samples we actually kept)
    int l1_cnt = 0, l2_cnt = 0, l3_cnt = 0, dram_cnt = 0;

    for (int i = 0; i < SAMPLES; i++) {
        
        // --- Measure L1 ---
        maccess(target); 
        start = rdtscp();
        maccess(target);
        end = rdtscp();
        
        lat = end - start;
        if (lat < 40) { // Ignore extreme noise
            l1_total += lat;
            l1_cnt++;
        }

        // --- Measure L2 ---
        maccess(target); 
        flush_l1();
        
        start = rdtscp();
        maccess(target); 
        end = rdtscp();
        
        lat = end - start;
        if (lat < 50 && lat > 20) { // Ignore extreme noise
            l2_total += lat;
            l2_cnt++;
        }

        // --- Measure L3 ---
        maccess(target); 
        flush_l2();      
        
        start = rdtscp();
        maccess(target);
        end = rdtscp();
        
        lat = end - start;
        if (lat > 30 && lat < 120) { // Ignore extreme noise
            l3_total += lat;
            l3_cnt++;
        }

        // --- Measure DRAM ---
        clflush(target);
        _mm_mfence(); // Serialize
        
        start = rdtscp();
        maccess(target); 
        end = rdtscp();
        
        lat = end - start;
        if (lat > 180) { // Ignore extreme noise
            dram_total += lat;
            dram_cnt++;
        }
    }

    // Prevent divide by zero if counts are 0
    double l1_avg = l1_cnt ? (double)l1_total / l1_cnt : 0;
    double l2_avg = l2_cnt ? (double)l2_total / l2_cnt : 0;
    double l3_avg = l3_cnt ? (double)l3_total / l3_cnt : 0;
    double dram_avg = dram_cnt ? (double)dram_total / dram_cnt : 0;

    printf("Results (Average Cycles):\n");
    printf("-------------------------\n");
    printf("L1 Cache : %.2f cycles (Samples: %d)\n", l1_avg, l1_cnt);
    printf("L2 Cache : %.2f cycles (Samples: %d)\n", l2_avg, l2_cnt);
    printf("L3 Cache : %.2f cycles (Samples: %d)\n", l3_avg, l3_cnt);
    printf("DRAM     : %.2f cycles (Samples: %d)\n", dram_avg, dram_cnt);
    printf("-------------------------\n");

    return 0;
}
