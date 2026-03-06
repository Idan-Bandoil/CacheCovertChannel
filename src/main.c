/*
 * Prime+Prune Eviction Set Finder (Huge Page Bucketing)
 * Features:
 * - 64-byte Cache-Line Alignment.
 * - Virtual Address Bucketing (O(1) set index calculation).
 * - Instant Reduction (Isolating Slices within Buckets).
 * - Auto-Detection and Allocation of Huge Pages.
 * - Tuned for Intel Alder Lake (i7-12700H) - 8 Slices, 4096 Sets/Slice.
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
#define CACHE_SETS 4096         // Sets PER SLICE (2^12 sets) for i7-12700H
#define SET_MASK 0xFFF          // Mask for exactly 12 bits (Bits 6-17)
#define POOL_SIZE_MB 128        // Memory pool size
#define RETRIES 10               

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
uint64_t threshold = 150; 
int latency_fd = -1;

// --- System Tuning Functions ---

/**
 * @name    enable_hugepages
 * @purpose Verifies if the OS has enough free 2MB huge pages to satisfy the 
 * requested POOL_SIZE_MB. If not, it attempts to allocate them dynamically 
 * by writing to /proc/sys/vm/nr_hugepages.
 * @param   required_mb  The amount of memory in Megabytes needed.
 * @return  1 on success, 0 on failure.
 */
int enable_hugepages(int required_mb) {
    // 2MB per page, plus a 10-page safety buffer
    int required_pages = (required_mb / 2) + 10; 
    int free_pages = 0;
    char line[256];

    // 1. Check current free huge pages
    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "HugePages_Free: %d", &free_pages) == 1) {
                break;
            }
        }
        fclose(fp);
    }

    if (free_pages >= required_pages) {
        printf("[*] Huge pages check passed (%d free).\n", free_pages);
        return 1;
    }

    printf("[!] Insufficient huge pages (%d free, need %d). Attempting to allocate...\n", free_pages, required_pages);

    // 2. Try to allocate more (Requires Root)
    int fd = open("/proc/sys/vm/nr_hugepages", O_WRONLY);
    if (fd == -1) {
        printf("\n[!] CRITICAL ERROR: Cannot write to /proc/sys/vm/nr_hugepages.\n");
        printf("[!] You must run this program as ROOT (sudo) to allocate huge pages.\n");
        printf("[!] Alternatively, allocate them manually:\n");
        printf("    sudo sysctl -w vm.nr_hugepages=%d\n\n", required_pages);
        return 0;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d\n", required_pages);
    if (write(fd, buf, strlen(buf)) != strlen(buf)) {
         printf("[!] ERROR: Failed to write to nr_hugepages. OS rejected request.\n");
         close(fd);
         return 0;
    }
    close(fd);

    // 3. Re-verify allocation (Fragmentation might cause OS to reject the request silently)
    free_pages = 0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "HugePages_Free: %d", &free_pages) == 1) {
                break;
            }
        }
        fclose(fp);
    }

    if (free_pages >= required_pages) {
        printf("[+] Successfully allocated %d huge pages dynamically!\n", free_pages);
        return 1;
    } else {
        printf("\n[!] CRITICAL ERROR: OS rejected huge page allocation.\n");
        printf("[!] Reason: Physical memory is likely too fragmented to find contiguous 2MB blocks.\n");
        printf("[!] Fix: Clear system caches and try again:\n");
        printf("    sync; echo 3 > /proc/sys/vm/drop_caches\n\n");
        return 0;
    }
}


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
    if (latency_fd != -1) {
        if (!write(latency_fd, &lat, sizeof(lat)))
            exit(1);
    }
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
 * @name    rdtsc_start
 * @purpose Serializes the pipeline with lfence, then reads the TSC.
 */
static inline uint64_t rdtsc_start() {
    uint32_t a, d;
    __asm__ volatile ("lfence\n\t rdtsc\n\t" : "=a" (a), "=d" (d) :: "memory");
    return ((uint64_t)d << 32) | a;
}

/**
 * @name    rdtsc_end
 * @purpose Reads the TSC with rdtscp (which forces serialization), then lfences.
 */
static inline uint64_t rdtsc_end() {
    uint32_t a, d;
    __asm__ volatile ("rdtscp\n\t lfence\n\t" : "=a" (a), "=d" (d) :: "rcx", "memory");
    return ((uint64_t)d << 32) | a;
}

static inline void maccess(void *p) {
    __asm__ volatile ("movq (%0), %%rax\n" : : "r" (p) : "rax");
}

static inline uint64_t time_access(void *addr) {
    uint64_t start = rdtsc_start();
    maccess(addr);
    uint64_t end = rdtsc_end();
    return end - start;
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
int tests_eviction(elem_t *candidate_head, elem_t *victim) {
    if (!candidate_head) return 0;
    int successes = 0;
    for (int r = 0; r < RETRIES; r++) {
        maccess(victim);
        __asm__ volatile("mfence");
        traverse_list(candidate_head);
        traverse_list(candidate_head); 
        __asm__ volatile("mfence");
        uint64_t time = time_access(victim);
        if (time > threshold) successes++;
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
    if (!tests_eviction(head, victim)) return 0;

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
            if (tests_eviction(head, victim)) {
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

    // 1. Check and Enable Huge Pages BEFORE allocating memory
    if (!enable_hugepages(POOL_SIZE_MB)) {
        return 1; // Program stops here if huge pages fail
    }

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
        return 1; 
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
        int set_idx = (addr >> 6) & SET_MASK; // Shift out 64-byte line, mask the 12 index bits for 4096 sets
        if (bucket_counts[set_idx] < 512) {
            buckets[set_idx][bucket_counts[set_idx]++] = i;
        }
    }

    int *es_buffer = malloc(CACHE_WAYS * 10 * sizeof(int)); 
    int sets_found = 0;
    time_t start_time = time(NULL);

    printf("[*] Starting per-bucket reduction...\n");

    // Scan through every unique Cache Set Index
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
                if (sets_found % 100 == 0) {
                     printf("[+] Found %d sets... (%ld sec)\n", sets_found, time(NULL) - start_time);
                }

                candidates_available -= (es_size + 1); // remove victim and set
                break; // NOTE: We break here to just find 1 slice per index. Remove `break` to map ALL slices.
            } else {
                break; 
            }
        }
    }

    printf("[*] DONE. Found %d perfectly unique sets in %ld seconds.\n", sets_found, time(NULL) - start_time);
    
    if (latency_fd != -1) close(latency_fd);
    return 0;
}