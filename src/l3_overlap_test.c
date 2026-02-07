#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <immintrin.h> // For _mm_lfence

// --- USER CONFIGURATION ---
#define P_CORE_ID 0
#define E_CORE_ID 12

// ADJUST THIS TO YOUR CPU (e.g., 24, 30, 36)
#define L3_SIZE_MB 24

// P-Core fills exactly the L3 size
#define PROBE_SIZE (L3_SIZE_MB * 1024 * 1024)

// E-Core buffer is 2x L3 ("Working Set" range)
#define THRASH_SIZE (2 * L3_SIZE_MB * 1024 * 1024)

// Repeated access defeats Scan Resistance/Prefetcher
#define ATTACK_PASSES 30 

// --- GLOBALS ---
void **p_list_head; 
void **e_list_head; 
volatile int turn = 0; 

// --- HELPER: Create Randomized Linked List ---
// is_circular: 1 = Last node points to First (Infinite loop for Thrashing)
// is_circular: 0 = Last node is NULL (Terminates for Verification)
void **create_random_list(size_t size_bytes, int is_circular) {
    size_t count = size_bytes / sizeof(void*);
    void **buffer = (void**)malloc(size_bytes);
    if (!buffer) { perror("Malloc failed"); exit(1); }

    // Array of pointers to shuffle
    void **ptrs = (void**)malloc(count * sizeof(void*));
    
    // Initialize
    for (size_t i = 0; i < count; i++) {
        ptrs[i] = &buffer[i];
    }

    // Fisher-Yates Shuffle (Randomize the order of nodes)
    for (size_t i = count - 1; i > 0; i--) {
        size_t j = rand() % (i + 1);
        void *temp = ptrs[i];
        ptrs[i] = ptrs[j];
        ptrs[j] = temp;
    }

    // Link them together based on the shuffled order
    for (size_t i = 0; i < count - 1; i++) {
        *(void**)ptrs[i] = ptrs[i+1];
    }
    
    if (is_circular) {
        // Last element points back to First element
        *(void**)ptrs[count-1] = ptrs[0];
    } else {
        // Last element terminates the list
        *(void**)ptrs[count-1] = NULL;
    }

    void **head = ptrs[0];
    free(ptrs); // Free the helper array
    return head;
}

// --- P-CORE (The Owner) ---
void *p_thread_func(void *arg) {
    // Pin to P-Core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(P_CORE_ID, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    printf("[P-Core %d] Building Random List (%d MB)...\n", P_CORE_ID, L3_SIZE_MB);
    
    // Create LINEAR (Terminated) Randomized List
    p_list_head = create_random_list(PROBE_SIZE, 0);

    printf("[P-Core %d] Priming L3...\n", P_CORE_ID);
    
    // Walk the list to load into L3
    // Because the list is randomized, the prefetcher cannot help us here.
    void **ptr = p_list_head;
    while(ptr != NULL) {
        ptr = (void**)*ptr;
    }

    printf("[P-Core %d] L3 Primed. Waiting for E-Core...\n", P_CORE_ID);
    turn = 1; 

    while (turn == 1); // Wait for E-Core

    printf("[P-Core %d] Verifying Residency...\n", P_CORE_ID);

    int hits = 0;
    int misses = 0;
    ptr = p_list_head;
    unsigned int start, end;

    while(ptr != NULL) {
        start = __rdtscp(&start); // Serializing read
        void *next = *ptr;        // The load we want to time
        end = __rdtscp(&end);     // Serializing read
        
        // 130 cycles is a safe bet for L3 hit vs DRAM miss
        if ((end - start) < 130) hits++;
        else misses++;
        
        ptr = (void**)next;
    }

    printf("\n--- RESULTS ---\n");
    printf("Total Lines:   %d\n", hits + misses);
    printf("Hits (Fast):   %d\n", hits);
    printf("Miss (Slow):   %d\n", misses);
    printf("Eviction Rate: %.2f%%\n", (double)misses/(hits+misses) * 100.0);
    
    return NULL;
}

// --- E-CORE (The Attacker) ---
void *e_thread_func(void *arg) {
    // Pin to E-Core
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(E_CORE_ID, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    printf("[E-Core %d] Building Attack List (%d MB)...\n", E_CORE_ID, THRASH_SIZE/1024/1024);
    
    // Create CIRCULAR Randomized List for infinite thrashing
    e_list_head = create_random_list(THRASH_SIZE, 1);

    printf("[E-Core %d] Ready.\n", E_CORE_ID);
    while (turn == 0); 

    printf("[E-Core %d] ATTACKING! (Sustained Thrashing - %d Passes)...\n", E_CORE_ID, ATTACK_PASSES);

    // 2. SUSTAINED THRASH
    // We walk the circular list for many iterations.
    size_t nodes_in_list = THRASH_SIZE / sizeof(void*);
    size_t total_steps = nodes_in_list * ATTACK_PASSES;
    
    void **ptr = e_list_head;
    for (size_t i = 0; i < total_steps; i++) {
        ptr = (void**)*ptr;
    }

    printf("[E-Core %d] Attack complete.\n", E_CORE_ID);
    turn = 2; 
    return NULL;
}

int main() {
    srand(time(NULL));

    pthread_t p_t, e_t;
    pthread_create(&p_t, NULL, p_thread_func, NULL);
    pthread_create(&e_t, NULL, e_thread_func, NULL);

    pthread_join(p_t, NULL);
    pthread_join(e_t, NULL);

    return 0;
}