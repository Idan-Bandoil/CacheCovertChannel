#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "utils.h"

// --- Run Configuration ---
#define NUM_HUGE_PAGES 50
#define TARGET_SLICE 2
#define TARGET_SET 0x5A

int main(int argc, char** argv) {
    if (argc != 2)
    {
        printf("Usage: %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);

    pin_cpu(core_id);

    uint8_t *base_addr = (uint8_t*)allocate_huge_pages(NUM_HUGE_PAGES);

    // Eviction set array exactly sized to the cache associativity
    void* eviction_set[LLC_WAYS];
    int ev_idx = 0;

    printf("[*] Building Eviction Set for Set: 0x%x, Slice: %d (Targeting %d Ways)\n", 
           TARGET_SET, TARGET_SLICE, LLC_WAYS);

    for (size_t i = 0; i < NUM_HUGE_PAGES; i++) {
        uint8_t *page_base = base_addr + (i * HUGE_PAGE_SIZE);
        uint64_t phys_base = virt_to_phys(page_base);
        
        if (phys_base == 0) continue;

        // Iterate through cache lines (64 bytes)
        for (size_t offset = 0; offset < HUGE_PAGE_SIZE; offset += CACHE_LINE_SIZE) {
            void *virt_line = page_base + offset;
            uint64_t phys_line = phys_base + offset;

            // Extract the 12-bit set index for a 4096-set slice
            uint64_t current_set = (phys_line >> SET_INDEX_SHIFT) & SET_INDEX_MASK;
            
            if (current_set == TARGET_SET) {
                int current_slice = get_cache_slice(phys_line);
                
                if (current_slice == TARGET_SLICE) {
                    eviction_set[ev_idx++] = virt_line;
                    if (ev_idx >= LLC_WAYS) {
                        goto search_done;
                    }
                }
            }
        }
    }

search_done:
    if (ev_idx < LLC_WAYS) {
        printf("[-] Could not find enough lines (%d/%d). Try increasing NUM_HUGE_PAGES.\n", 
               ev_idx, LLC_WAYS);
        return 1;
    }

    printf("[+] Found full eviction set (%d addresses)\n", ev_idx);
    for (int i = 0; i < ev_idx; i++) {
        printf("    [%2d] VA: %p | PA: 0x%lx\n", i, eviction_set[i], virt_to_phys(eviction_set[i]));
    }

    printf("\n[*] Timing verification (First element)...\n");
    maccess(eviction_set[0]);
    uint64_t hit_time = measure_access_time(eviction_set[0]);
    printf("    Cache Hit Time: %lu cycles\n", hit_time);

    return 0;
}
