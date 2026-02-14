/*
 * Hybrid-Arch Eviction Set Finder (P-Core Force Mode)
 * Features:
 * - Real-Time Priority (SCHED_FIFO) to bypass scheduler throttling.
 * - PM QoS Latency Lock to prevent C-State sleep.
 * - Optimized Group Reduction (Batching + Bitmap).
 *
 * Usage:
 * sudo ./eviction_finder <core_id>
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
#define CACHE_WAYS 12           // Adjust for your P-Core (Usually 12 or 16)
#define CACHE_SETS 32768        // 32K sets (Typical for large L3)
#define POOL_SIZE_MB 128        
#define RETRIES 3               
#define BATCH_SIZE 550000       

// --- Structures ---
typedef struct elem {
    struct elem *next;
    struct elem *prev; // Unused but kept for alignment
    int id;
} elem_t;

// --- Global Variables ---
elem_t *buffer_pool;
// USER CONFIRMED: Threshold 80 is correct for this P-Core setup.
uint64_t threshold = 80; 
int latency_fd = -1;

// --- System Tuning Functions ---

// 1. Prevent C-States (Sleep) by locking DMA latency
void set_latency_target() {
    int32_t lat = 0; // 0 latency required
    latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (latency_fd == -1) {
        perror("[!] Warn: Open /dev/cpu_dma_latency failed (Run as Root?)");
        return;
    }
    if (write(latency_fd, &lat, sizeof(lat)) != sizeof(lat)) {
        perror("[!] Warn: Write latency failed");
    }
    printf("[*] PM QoS: Locked CPU to C0 state (No Sleep).\n");
}

// 2. Set Real-Time Priority
void set_realtime_priority() {
    struct sched_param param;
    param.sched_priority = 99; // Max priority
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("[!] Warn: Failed to set SCHED_FIFO");
    } else {
        printf("[*] Priority: Real-Time (SCHED_FIFO). Throttling disabled.\n");
    }
}

// --- Helpers ---

static inline uint64_t rdtsc() {
    uint64_t a, d;
    asm volatile ("mfence");
    asm volatile ("rdtsc" : "=a" (a), "=d" (d));
    asm volatile ("mfence");
    return (d << 32) | a;
}

static inline void maccess(void *p) {
    asm volatile ("movq (%0), %%rax\n" : : "c" (p) : "rax");
}

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

static inline uint64_t time_access(void *addr) {
    uint64_t start = rdtsc();
    maccess(addr);
    uint64_t end = rdtsc();
    return end - start;
}

void pin_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }
    printf("[*] Pinned to Core %d (Hard Affinity).\n", core_id);
}

// --- List Management ---

elem_t* build_list_with_skip(elem_t *base, int *indices, int count, int skip_idx, int skip_len) {
    elem_t *head = NULL;
    elem_t *curr = NULL;

    for (int i = 0; i < count; i++) {
        if (i >= skip_idx && i < skip_idx + skip_len) continue;

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

int tests_eviction(elem_t *candidate_head, elem_t *victim) {
    if (!candidate_head) return 0;

    int successes = 0;
    for (int r = 0; r < RETRIES; r++) {
        traverse_list(candidate_head);
        traverse_list(candidate_head); 
        
        maccess(victim);
        asm volatile("mfence");
        
        traverse_list(candidate_head);
        asm volatile("mfence");

        if (time_access(victim) > threshold) {
            successes++;
        }
    }
    return (successes > RETRIES / 2);
}

// --- Reduction ---

int reduce_group(elem_t *base, int *input_indices, int count, elem_t *victim, int *output_set) {
    // Sanity check
    elem_t *head = build_list_with_skip(base, input_indices, count, -1, 0); 
    if (!tests_eviction(head, victim)) return 0;

    static int current_set[BATCH_SIZE * 2]; 
    if (count > BATCH_SIZE * 2) return 0; 

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
                if (tail_len > 0) {
                    memmove(&current_set[i], &current_set[i + this_chunk], tail_len * sizeof(int));
                }
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
    // 1. Force flush to fix display stutter
    setbuf(stdout, NULL);

    if (argc != 2) {
        printf("Usage: %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);

    // 2. APPLY HYBRID CPU FIXES
    set_latency_target();      // Prevents C-State Sleep
    set_realtime_priority();   // Prevents Scheduler Throttling
    pin_cpu(core_id);          // Enforces P-Core

    // 3. Allocate Memory
    size_t pool_bytes = POOL_SIZE_MB * 1024 * 1024;
    size_t num_elements = pool_bytes / sizeof(elem_t);
    
    buffer_pool = mmap(NULL, pool_bytes, PROT_READ|PROT_WRITE, 
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (buffer_pool == MAP_FAILED) {
        printf("[!] HugePages failed. Using standard.\n");
        buffer_pool = mmap(NULL, pool_bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    }
    
    for (size_t i = 0; i < num_elements; i++) {
        buffer_pool[i].id = i;
        buffer_pool[i].next = NULL;
    }
    
    int *global_indices = malloc(num_elements * sizeof(int));
    for(int i=0; i<num_elements; i++) global_indices[i] = i;

    unsigned char *status_map = calloc(num_elements, sizeof(unsigned char));
    int *batch_buffer = malloc(BATCH_SIZE * 2 * sizeof(int));
    int *es_buffer = malloc(CACHE_WAYS * 10 * sizeof(int)); 

    printf("[*] Starting P-Core Mapping (Target: %d sets)...\n", CACHE_SETS);

    int sets_found = 0;
    int current_scan_idx = 0;
    time_t start_time = time(NULL);

    while (sets_found < CACHE_SETS && current_scan_idx < num_elements) {
        
        // A. Pick Victim
        while (current_scan_idx < num_elements && status_map[global_indices[current_scan_idx]]) {
            current_scan_idx++;
        }
        if (current_scan_idx >= num_elements) break;

        int victim_idx = global_indices[current_scan_idx];
        elem_t *victim = &buffer_pool[victim_idx];

        // B. Form Batch
        int batch_count = 0;
        int search_idx = current_scan_idx + 1;
        
        while (batch_count < BATCH_SIZE && search_idx < num_elements) {
            int idx = global_indices[search_idx];
            if (!status_map[idx]) {
                batch_buffer[batch_count++] = idx;
            }
            search_idx++;
        }
        if (batch_count < BATCH_SIZE / 2 && sets_found > CACHE_SETS * 0.95) break; 

        // C. Reduce
        int es_size = reduce_group(buffer_pool, batch_buffer, batch_count, victim, es_buffer);
        
        if (es_size >= CACHE_WAYS) {
            sets_found++;
            if (sets_found % 400 == 0) {
                 printf("[+] Found %d sets... (%ld sec)\n", sets_found, time(NULL) - start_time);
            }

            status_map[victim_idx] = 1;
            for(int k=0; k<es_size; k++) {
                status_map[es_buffer[k]] = 1;
            }
        } 
        current_scan_idx++;
    }

    printf("[*] DONE. Found %d sets in %ld seconds.\n", sets_found, time(NULL) - start_time);
    if (latency_fd != -1) close(latency_fd);
    return 0;
}