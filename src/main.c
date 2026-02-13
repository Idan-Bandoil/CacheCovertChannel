#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

// Include MASTIK headers
#include <mastik/low.h>
#include <mastik/mm.h>
#include <mastik/l3.h>

#define L3_SETS 1
#define L3_ASSOCIATIVITY 12

int main(int argc, char **argv) {
    mm_t mm = mm_prepare(NULL, NULL, NULL);
    if (!mm) {
        fprintf(stderr, "Error: mm_prepare failed.\n");
        return 1;
    }

    // ---------------------------------------------------------
    // Step 3: Loop through EVERY Set
    // ---------------------------------------------------------
    
    int lines_needed = 1;
    void **eviction_set = (void**)malloc(lines_needed * sizeof(void*));

    for (int set_index = 0; set_index < L3_SETS; set_index++) {
        printf("Requesting lines\n");
        mm_requestlines(mm, L3, set_index, eviction_set, 1);
        printf("Done\n");

        // Print progress every 100 sets so we don't spam the console
        if (set_index % 100 == 0 || set_index == L3_SETS - 1) {
            printf("Set %4d: Found %d lines. (First: %p)\n", 
                   set_index, lines_needed, eviction_set[0]);
        }

        // In a real attack, you would keep these pointers to use them.
        // However, if we loop through ALL sets (e.g. 8192 sets) and keep 
        // 20 lines for each, we will allocate ~1GB of HugePages instantly.
        // For this demo, we return them to the pool so they can be reused.
        
        // Comment this out if you actually want to store all of them!
        mm_returnlines(mm, eviction_set, lines_needed);
    }

    printf("\n[*] Scan complete.\n");

    // ---------------------------------------------------------
    // Step 4: Cleanup
    // ---------------------------------------------------------
    free(eviction_set);
    mm_release(mm);
    
    return 0;
}
