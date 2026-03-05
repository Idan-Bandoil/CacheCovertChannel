/*
 * Prime+Prune Eviction Set Finder (Huge Page Bucketing)
 * Features:
 * - 64-byte Cache-Line Alignment.
 * - Virtual Address Bucketing (O(1) set index calculation).
 * - Instant Reduction (Isolating Slices within Buckets).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>

// --- Configuration ---
#define CACHE_WAYS 12           
#define CACHE_SETS 32768        // 2^15 sets
#define SET_MASK 0x7FFF         // Mask for bits 6-20
#define POOL_SIZE_MB 128        // Increased to ensure enough elements per bucket
#define RETRIES 30               

// --- Structures ---
// EXTREMELY IMPORTANT: Pad the struct to exactly 64 bytes (1 Cache Line)
typedef struct elem {
    struct elem *next;
    struct elem *prev; 
    int id;
    char padding[64 - (2 * sizeof(void*)) - sizeof(int)]; 
} elem_t;

// --- Global Variables ---
elem_t *buffer_pool;
uint64_t threshold = 80; 
int latency_fd = -1;

// --- System Tuning Functions ---

/**
 * @name    set_latency_target
 * @purpose Locks the CPU into the C0 state by writing to /dev/cpu_dma_latency. 
 * This prevents the processor from entering deep sleep states, which 
 * would otherwise introduce massive latency spikes and ruin cache 
 * timing measurements.
 * @param   None
 */
void set_latency_target() {
    int32_t lat = 0; 
    latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (latency_fd != -1) write(latency_fd, &lat, sizeof(lat));
}

/**
 * @name    set_realtime_priority
 * @purpose Elevates the process scheduling priority to maximum (SCHED_FIFO, 99).
 * This bypasses standard OS time-slicing and throttling, ensuring the
 * timing loop is not interrupted by background system noise.
 * @param   None
 */
void set_realtime_priority() {
    struct sched_param param;
    param.sched_priority = 99; 
    sched_setscheduler(0, SCHED_FIFO, &param);
}

/**
 * @name    pin_cpu
 * @purpose Binds the executing thread to a specific physical CPU core. 
 * This is strictly required for targeting L3 cache slices and avoiding 
 * cross-core migration noise, especially on P/E-Core hybrid architectures.
 * @param   core_id  The integer ID of the physical core to pin the process to.
 */
void pin_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// --- Helpers ---

/**
 * @name    rdtsc
 * @purpose Accurately reads the CPU's Time Stamp Counter (TSC). Uses `mfence` 
 * instructions before and after the read to serialize execution and 
 * prevent out-of-order execution from skewing the measurement.
 * @param   None
 * @return  The current 64-bit cycle count.
 */
static inline uint64_t rdtsc() {
    uint64_t a, d;
    asm volatile ("mfence");
    asm volatile ("rdtsc" : "=a" (a), "=d" (d));
    asm volatile ("mfence");
    return (d << 32) | a;
}

/**
 * @name    maccess
 * @purpose Executes a dummy memory read at a specific address to force the 
 * hardware to load the corresponding cache line into the CPU cache hierarchy.
 * @param   p  Pointer to the memory address to be accessed.
 */
static inline void maccess(void *p) {
    asm volatile ("movq (%0), %%rax\n" : : "c" (p) : "rax");
}

/**
 * @name    traverse_list
 * @purpose Iterates through a linked list of memory elements, accessing each 
 * one to load it into the cache. This is the primary mechanism for 
 * priming the cache and forcing evictions.
 * @param   head  Pointer to the first element in the linked list.
 * @return  The total number of elements traversed.
 */
static int traverse_list(elem_t *head) {
    elem_t *p = head;
    int count = 0;

    while (p) {
        maccess(p);
        p = p->next;
        count++;
    }

    return count;
}

/**
 * @name    time_access
 * @purpose Measures the exact latency (in CPU cycles) of a single memory access.
 * High latencies indicate a cache miss (eviction), while low latencies 
 * indicate a cache hit.
 * @param   addr  Pointer to the memory address to measure.
 * @return  The latency of the memory access in CPU cycles.
 */
static inline uint64_t time_access(void *addr) {
    uint64_t start = rdtsc();
    maccess(addr);
    uint64_t end = rdtsc();

    return end - start;
}

/**
 * @name    build_list_with_skip
 * @purpose Dynamically constructs a linked list from an array of candidate indices,
 * allowing a specific contiguous chunk of elements to be omitted. This is 
 * the core list-building function used by the binary reduction algorithm.
 * @param   base      The base pointer of the allocated memory pool.
 * @param   indices   Array of integer indices pointing to candidate elements.
 * @param   count     The total number of candidate indices provided.
 * @param   skip_idx  The starting index within the array to begin skipping elements.
 * @param   skip_len  The number of contiguous elements to skip.
 * @return  A pointer to the head of the newly constructed linked list.
 */
elem_t* build_list_with_skip(elem_t *base, int *indices, int count, int skip_idx, int skip_len) {
    elem_t *head = NULL;
    elem_t *curr = NULL;

    for (int i = 0; i < count; i++) {
        if (skip_len > 0 && i >= skip_idx && i < skip_idx + skip_len) continue;
        if (!head) {
            head = &base[indices[i]];
            curr = head;
        } else {
            elem_t *next = &base[indices[i]];
            curr->next = next;
            next->prev = curr; 
            curr = next;
        }
    }

    if (curr) curr->next = NULL;
    
    return head;
}

// --- Eviction Testing ---

/**
 * @name    tests_eviction
 * @purpose Evaluates whether accessing the provided candidate list successfully 
 * evicts the victim address from the cache. Uses multiple retries to 
 * filter out systemic noise and false positives.
 * @param   candidate_head  The linked list of candidate memory addresses to access.
 * @param   victim          The target memory address to test for eviction.
 * @return  1 if the victim was successfully evicted (latency > threshold), 0 otherwise.
 */
int test_eviction(elem_t *candidate_head, elem_t *victim) {
    if (!candidate_head) return 0;
    int successes = 0;
    for (int r = 0; r < RETRIES; r++) {
        maccess(victim);
        asm volatile("mfence");
        traverse_list(candidate_head);
        asm volatile("mfence");
        if (time_access(victim) > threshold) successes++;
    }
    return (successes > RETRIES / 2);
}

// --- Reduction ---

/**
 * @name    reduce_group
 * @purpose Minimizes a large batch of candidate addresses into a minimal eviction 
 * set (typically equal to CACHE_WAYS) that still successfully evicts the victim. 
 * Uses an optimized, binary-search-like divide-and-conquer strategy.
 * @param   base           The base pointer of the memory pool.
 * @param   input_indices  Array containing the starting batch of candidate indices.
 * @param   count          The total number of candidates in the input batch.
 * @param   victim         The target address being evicted.
 * @param   output_set     Buffer to store the final, minimized eviction set indices.
 * @return  The size of the minimized eviction set (e.g., 12), or 0 if reduction failed.
 */
int reduce_group(elem_t *base, int *input_indices, int count, elem_t *victim, int *output_set) {
    elem_t *head = build_list_with_skip(base, input_indices, count, -1, 0); 
    if (!test_eviction(head, victim)) return 0;

    int current_set[1024]; // Max elements in a bucket won't exceed this
    memcpy(current_set, input_indices, count * sizeof(int));
    int current_len = count;
    int chunk_size = current_len / 2;
    
    while (chunk_size >= 1) {
        int i = 0;
        int progress_made = 0;
        while (i < current_len) {
            int this_chunk = (i + chunk_size > current_len) ? (current_len - i) : chunk_size;
            if ((current_len - this_chunk) < CACHE_WAYS) {
                i += this_chunk;
                continue;
            }
            head = build_list_with_skip(base, current_set, current_len, i, this_chunk);
            if (test_eviction(head, victim)) {
                int tail_len = current_len - (i + this_chunk);
                if (tail_len > 0) memmove(&current_set[i], &current_set[i + this_chunk], tail_len * sizeof(int));
                current_len -= this_chunk;
                progress_made = 1;
                if (current_len == CACHE_WAYS) goto cleanup;
            } else {
                i += this_chunk;
            }
        }
        if (!progress_made) chunk_size /= 2;
    }

cleanup:
    if (current_len >= CACHE_WAYS && current_len <= CACHE_WAYS + 4) { 
        memcpy(output_set, current_set, current_len * sizeof(int));
        return current_len;
    }
    return 0;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);

    if (argc != 2) {
        printf("Usage: %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);

    set_latency_target();
    set_realtime_priority();
    pin_cpu(core_id);

    if (sizeof(elem_t) != 64) {
        printf("[!] ERROR: elem_t is %zu bytes, not 64! Aborting.\n", sizeof(elem_t));
        return 1;
    }

    size_t pool_bytes = POOL_SIZE_MB * 1024 * 1024;
    size_t num_elements = pool_bytes / sizeof(elem_t);
    
    buffer_pool = mmap(NULL, pool_bytes, PROT_READ|PROT_WRITE, 
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (buffer_pool == MAP_FAILED) {
        printf("[!] HugePages required for Bucketing! Failed with %d.\n", errno);
        return 1; // Strict enforcement, we NEED huge pages for this math to work.
    }
    
    for (size_t i = 0; i < num_elements; i++) {
        buffer_pool[i].id = i;
        buffer_pool[i].next = NULL;
    }

    // --- NEW: BUCKETING LOGIC ---
    printf("[*] Bucketing addresses by Cache Set Index...\n");
    int **buckets = malloc(CACHE_SETS * sizeof(int*));
    int *bucket_counts = calloc(CACHE_SETS, sizeof(int));
    for(int i=0; i<CACHE_SETS; i++) {
        buckets[i] = malloc(512 * sizeof(int)); // Safely hold candidates
    }

    for (size_t i = 0; i < num_elements; i++) {
        uintptr_t addr = (uintptr_t)&buffer_pool[i];
        int set_idx = (addr >> 6) & SET_MASK; // Shift out 64-byte line, mask the 15 index bits
        if (bucket_counts[set_idx] < 512) {
            buckets[set_idx][bucket_counts[set_idx]++] = i;
        }
    }

    int *es_buffer = malloc(CACHE_WAYS * 10 * sizeof(int)); 
    int sets_found = 0;
    time_t start_time = time(NULL);

    printf("[*] Starting per-bucket reduction...\n");

    // Scan through every guaranteed unique Cache Set Index
    for (int set_idx = 0; set_idx < CACHE_SETS; set_idx++) {
        int candidates_available = bucket_counts[set_idx];
        int *current_bucket = buckets[set_idx];

        // Within this index, try to find sets (representing different slices)
        while (candidates_available > CACHE_WAYS) {
            
            // Pick a victim from the end of the bucket
            int victim_idx = current_bucket[candidates_available - 1];
            elem_t *victim = &buffer_pool[victim_idx];

            // Use the rest of the bucket as candidates
            int batch_size = candidates_available - 1;
            
            // REDUCE! Notice how fast this is, batch_size is only ~64 addresses!
            int es_size = reduce_group(buffer_pool, current_bucket, batch_size, victim, es_buffer);

            if (es_size >= CACHE_WAYS) {
                sets_found++;
                if (sets_found % 1000 == 0) {
                     printf("[+] Found %d sets... (%ld sec)\n", sets_found, time(NULL) - start_time);
                }

                // Optimization: In a full toolkit, you would save `es_buffer` here.
                // For now, we remove the found elements from this bucket so we can find the next slice.
                candidates_available -= (es_size + 1); // remove victim and set
                break; // NOTE: We break here to just find 1 slice per index. Remove `break` to map ALL slices.
            } else {
                // If we can't reduce it, we either found all slices or hit noise.
                break; 
            }
        }
    }

    printf("[*] DONE. Found %d perfectly unique sets in %ld seconds.\n", sets_found, time(NULL) - start_time);
    
    if (latency_fd != -1) close(latency_fd);
    return 0;
}