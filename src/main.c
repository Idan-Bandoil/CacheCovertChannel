#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

// MASTIK headers
#include <mastik/fr.h>
#include <mastik/util.h>

// Configuration for i7-12700 (25MB L3)
#define L3_SIZE_MB 25
// We use a buffer 3x larger than L3 to guarantee eviction even with replacement policies
#define BUFFER_SIZE_MB (L3_SIZE_MB * 3)
#define BUFFER_SIZE_BYTES (L3_SIZE_MB * 3 * 1024 * 1024)
#define CACHE_LINE 64
#define MINIMAL_RAM_ACCESS_CYCLES_THRESHOLD 180

void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

int main() {
    printf("[*] specific Check: Cache Inclusion Policy (Using MASTIK)\n");
    printf("[*] Target CPU: Intel 12th Gen\n");

    // 1. Setup MASTIK FR handle
    fr_t fr = fr_prepare();
    if (!fr) {
        fprintf(stderr, "Error initializing MASTIK\n");
        return 1;
    }

    // 2. Allocate the massive buffer
    printf("[*] Allocating %d MB buffer...\n", BUFFER_SIZE_MB);
    uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE_BYTES);
    if (!buffer) return 1;

    // Initialize buffer to ensure pages are mapped (COW protection)
    for (int i = 0; i < BUFFER_SIZE_BYTES; i += 4096) {
        buffer[i] = 1;
    }

    // 3. Pick a target address in the middle of the buffer
    // We treat this pointer as the "victim" line we are testing
    void *target = (void *)&buffer[BUFFER_SIZE_BYTES / 2];
    
    // Monitor this address so fr_probe can time it
    fr_monitor(fr, target);

    // 4. Run the Experiment multiple times
    int inclusive_count = 0;
    int non_inclusive_count = 0;
    int iterations = 100; // Run enough times to average out noise

    printf("[*] Running %d iterations...\n", iterations);
    
    for (int k = 0; k < iterations; k++) {
        // A. Load Target into L1/L2/L3
        maccess(target);

        // B. Thrash L3
        // We scan the large buffer to force the L3 to fill with new data.
        for (int i = 0; i < BUFFER_SIZE_BYTES; i += CACHE_LINE) {
            // SKIP the target itself so we don't reload it
            if ((uintptr_t)&buffer[i] == (uintptr_t)target) continue;
            
            // Access garbage line
            maccess(&buffer[i]);
        }

        // C. Probe (Time the reload of Target)
        // fr_probe measures time to access the monitored addresses.
        uint16_t results[1];
        fr_probe(fr, results);
        uint16_t latency = results[0];

        // D. Classify
        if (latency > MINIMAL_RAM_ACCESS_CYCLES_THRESHOLD) {
            inclusive_count++;
        } else {
            non_inclusive_count++;
        }
    }

    // 5. Results
    printf("\n[*] Results:\n");
    printf("    Slow Accesses (Evicted):  %d\n", inclusive_count);
    printf("    Fast Accesses (Cached):   %d\n", non_inclusive_count);
    
    printf("\n[*] Conclusion:\n");
    if (inclusive_count > non_inclusive_count) {
        printf("    CACHE IS INCLUSIVE.\n");
        printf("    (Evicting L3 also evicted the data in L1/L2)\n");
    } else {
        printf("    CACHE IS NON-INCLUSIVE.\n");
        printf("    (Data survived in L1/L2 after L3 eviction)\n");
    }

    // Cleanup
    free(buffer);
    fr_release(fr);
    return 0;
}
