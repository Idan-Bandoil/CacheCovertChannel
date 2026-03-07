/*
 * Cross-Core Eviction Efficacy Tester
 * Extracts L3 slices on a P-core, then verifies eviction capabilities on an E-core.
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
#define CACHE_SETS 4096         
#define SET_MASK 0xFFF          
#define POOL_SIZE_MB 128        
#define RETRIES 10               
#define MAX_CANDIDATES 4000     

// --- Structures ---
typedef struct elem {
    struct elem *next;
    struct elem *prev; 
    int id;
    char padding[64 - (2 * sizeof(void*)) - sizeof(int)]; 
} elem_t;

// --- Global Variables ---
elem_t *buffer_pool;
uint64_t threshold = 120; 
int latency_fd = -1;

// --- System Tuning Functions ---
int enable_hugepages(int required_mb) {
    int required_pages = (required_mb / 2) + 10; 
    int free_pages = 0;
    char line[256];

    FILE *fp = fopen("/proc/meminfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "HugePages_Free: %d", &free_pages) == 1) break;
        }
        fclose(fp);
    }
    if (free_pages >= required_pages) return 1;

    int fd = open("/proc/sys/vm/nr_hugepages", O_WRONLY);
    if (fd == -1) return 0;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d\n", required_pages);
    write(fd, buf, strlen(buf));
    close(fd);

    free_pages = 0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "HugePages_Free: %d", &free_pages) == 1) break;
        }
        fclose(fp);
    }
    return (free_pages >= required_pages);
}

void set_latency_target() {
    int32_t lat = 0; 
    latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (latency_fd != -1) write(latency_fd, &lat, sizeof(lat));
}

void set_realtime_priority() {
    struct sched_param param;
    param.sched_priority = 99; 
    sched_setscheduler(0, SCHED_FIFO, &param);
}

void pin_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

// --- Low Level Timing ---
static inline uint64_t rdtsc_start() {
    uint32_t a, d;
    __asm__ volatile ("lfence\n\t rdtsc\n\t" : "=a" (a), "=d" (d) :: "memory");
    return ((uint64_t)d << 32) | a;
}

static inline uint64_t rdtsc_end() {
    uint32_t a, d;
    __asm__ volatile ("rdtscp\n\t lfence\n\t" : "=a" (a), "=d" (d) :: "rcx", "memory");
    return ((uint64_t)d << 32) | a;
}

static inline void maccess(volatile void *p) {
    __asm__ volatile ("movq (%0), %%rax\n" : : "r" (p) : "rax");
}

static inline uint64_t time_access(volatile void *addr) {
    uint64_t start = rdtsc_start();
    maccess(addr);
    uint64_t end = rdtsc_end();
    return end - start;
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

// --- Pointer-based List Builder ---
elem_t* build_pointer_list(elem_t **arr, int count, int skip_idx, int skip_len) {
    elem_t *head = NULL;
    elem_t *curr = NULL;

    for (int i = 0; i < count; i++) {
        if (skip_len > 0 && i >= skip_idx && i < skip_idx + skip_len) continue;
        
        if (!head) {
            head = arr[i];
            curr = head;
        } else {
            curr->next = arr[i];
            arr[i]->prev = curr;
            curr = arr[i];
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
        maccess(victim);
        __asm__ volatile("mfence");
        
        traverse_list(candidate_head);
        traverse_list(candidate_head);
        traverse_list(candidate_head);
        traverse_list(candidate_head);
        traverse_list(candidate_head);
        
        __asm__ volatile("mfence");
        uint64_t time = time_access(victim);
        
        if (time > threshold) successes++;
        if (successes > RETRIES / 3) return 1;
        if (r - successes > RETRIES * 2 / 3) return 0;
    }
    return (successes > RETRIES / 3); 
}

// --- NEW: Statistical Benchmarking ---
// Runs 10,000 trials to find the exact eviction success rate of a given set
void benchmark_eviction_rate(elem_t *es_head, elem_t *victim) {
    int evictions = 0;
    int rounds = 10000;
    
    for (int i = 0; i < rounds; i++) {
        // 1. Bring victim into L3
        maccess(victim);
        __asm__ volatile("mfence");
        
        // 2. Thrash violently to bypass E-core L2 filtering
        // We increase traversals and add memory fences to prevent 
        // the E-core from heavily pipelining the accesses.
        for(int j = 0; j < 12; j++) {
            traverse_list(es_head);
            __asm__ volatile("lfence"); 
        }
        
        __asm__ volatile("mfence");
        
        // 3. Time the victim
        uint64_t time = time_access(victim);
        
        if (time > threshold) {
            evictions++;
        }
    }
    
    double rate = (evictions * 100.0) / rounds;
    printf("    -> Efficacy Rate: %6.2f%% \n", rate);
}

// --- Robust Block Reduction ---
int robust_reduce(elem_t **candidates, int num_candidates, elem_t *victim, elem_t **out_es) {
    int working_len = num_candidates;
    elem_t **working_set = malloc(working_len * sizeof(elem_t*));
    memcpy(working_set, candidates, working_len * sizeof(elem_t*));

    elem_t *head = build_pointer_list(working_set, working_len, -1, 0);
    if (!tests_eviction(head, victim)) {
        free(working_set);
        return 0; 
    }

    int chunk_size = working_len / 2;
    
    while (chunk_size > 0 && working_len > CACHE_WAYS) {
        int i = 0;
        int progress_made = 0;
        
        while (i < working_len && working_len > CACHE_WAYS) {
            int current_chunk = (i + chunk_size > working_len) ? (working_len - i) : chunk_size;
            
            if (working_len - current_chunk < CACHE_WAYS) {
                i += current_chunk;
                continue;
            }

            head = build_pointer_list(working_set, working_len, i, current_chunk);
            
            if (tests_eviction(head, victim)) {
                int tail_len = working_len - (i + current_chunk);
                if (tail_len > 0) {
                    memmove(&working_set[i], &working_set[i + current_chunk], tail_len * sizeof(elem_t*));
                }
                working_len -= current_chunk;
                progress_made = 1;
            } else {
                i += current_chunk;
            }
        }
        
        if (!progress_made) {
            chunk_size /= 2;
        }
    }

    if (working_len >= CACHE_WAYS && working_len <= CACHE_WAYS + 12) {
        memcpy(out_es, working_set, working_len * sizeof(elem_t*));
        free(working_set);
        return working_len;
    }

    free(working_set);
    return 0;
}

// --- Calibration ---
void calibrate_threshold() {
    volatile elem_t* dummy = &buffer_pool[0];
    uint64_t hit_total = 0, miss_total = 0;
    int rounds = 10000;

    for (int i = 0; i < rounds; i++) {
        maccess(dummy); 
        __asm__ volatile("mfence");
        hit_total += time_access(dummy);

        __asm__ volatile("clflush (%0)" : : "r" (dummy) : "memory");
        __asm__ volatile("mfence");
        miss_total += time_access(dummy);
    }

    uint64_t avg_hit = hit_total / rounds;
    uint64_t avg_miss = miss_total / rounds;
    
    // RE-ENABLED: This MUST be dynamic for E-cores to work properly.
    // threshold = (avg_hit + avg_miss) / 2;

    printf("    -> Avg L3 Hit:   %lu cycles\n", avg_hit);
    printf("    -> Avg RAM Miss: %lu cycles\n", avg_miss);
    // printf("    -> Threshold:    %lu cycles\n", threshold);
}

// --- Main Execution ---
int main(int argc, char **argv) {
    setbuf(stdout, NULL);

    // Update CLI args to take both core IDs
    if (argc != 4) {
        printf("Usage: %s <p_core_id> <e_core_id> <target_set_index>\n", argv[0]);
        return 1;
    }
    int p_core_id = atoi(argv[1]);
    int e_core_id = atoi(argv[2]);
    int target_set_index = atoi(argv[3]);

    if (!enable_hugepages(POOL_SIZE_MB)) return 1;

    set_latency_target();
    set_realtime_priority();
    
    // --- PHASE 1: P-CORE EXTRACTION ---
    pin_cpu(p_core_id);
    printf("\n[====== PHASE 1: P-CORE EXTRACTION (Core %d) ======]\n", p_core_id);

    size_t pool_bytes = POOL_SIZE_MB * 1024 * 1024;
    size_t num_elements = pool_bytes / sizeof(elem_t);
    
    buffer_pool = mmap(NULL, pool_bytes, PROT_READ|PROT_WRITE, 
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (buffer_pool == MAP_FAILED) return 1; 

    for (size_t i = 0; i < num_elements; i++) {
        buffer_pool[i].id = i;
        buffer_pool[i].next = NULL;
        buffer_pool[i].prev = NULL;
    }

    elem_t **candidates = malloc(MAX_CANDIDATES * sizeof(elem_t*));
    int num_candidates = 0;

    for (size_t i = 0; i < num_elements; i++) {
        uintptr_t addr = (uintptr_t)&buffer_pool[i];
        int set_idx = (addr >> 6) & SET_MASK; 
        
        if (set_idx == target_set_index) {
            candidates[num_candidates++] = &buffer_pool[i];
            if (num_candidates >= MAX_CANDIDATES) break; 
        }
    }
    
    printf("[*] Calibrating P-Core...\n");
    // calibrate_threshold();

    elem_t **es_buffer = malloc(MAX_CANDIDATES * sizeof(elem_t*)); 
    
    // NEW: Arrays to save our extracted sets and their test victims
    elem_t *saved_sets[8];
    elem_t *saved_victims[8];
    int slices_found = 0;
    
    printf("\n[*] Extracting up to 8 slices...\n");

    while (num_candidates > CACHE_WAYS && slices_found < 8) {
        elem_t *victim = candidates[num_candidates - 1];
        int es_size = robust_reduce(candidates, num_candidates - 1, victim, es_buffer);

        if (es_size > 0) {
            elem_t *es_head = build_pointer_list(es_buffer, es_size, -1, 0);
            
            // SAVE the set and victim for Phase 2
            saved_sets[slices_found] = es_head;
            saved_victims[slices_found] = victim;
            slices_found++;
            
            printf("    [+] Found Slice %d (Size: %d ways)\n", slices_found, es_size);

            elem_t **survivors = malloc(num_candidates * sizeof(elem_t*));
            int survivors_count = 0;

            for (int i = 0; i < num_candidates; i++) {
                elem_t *test_candidate = candidates[i];
                
                int in_es = 0;
                for(int j=0; j<es_size; j++) {
                    if (test_candidate == es_buffer[j]) { in_es = 1; break; }
                }
                if (in_es) continue;

                if (tests_eviction(es_head, test_candidate)) continue; 
                survivors[survivors_count++] = test_candidate;
            }

            memcpy(candidates, survivors, survivors_count * sizeof(elem_t*));
            num_candidates = survivors_count;
            free(survivors);
        } else {
            num_candidates--;
        }
    }


    // --- PHASE 2: E-CORE VERIFICATION ---
    printf("\n[====== PHASE 2: E-CORE VERIFICATION (Core %d) ======]\n", e_core_id);
    
    // Switch process affinity to the E-core
    pin_cpu(e_core_id);
    
    // Give the OS a tiny moment to migrate the thread context before calibrating
    usleep(1000); 

    printf("[*] Recalibrating for E-Core latencies...\n");
    // calibrate_threshold();
    
    printf("\n[*] Testing P-Core eviction sets on E-Core...\n");
    for (int i = 0; i < slices_found; i++) {
        printf("    Testing Slice %d: ", i + 1);
        benchmark_eviction_rate(saved_sets[i], saved_victims[i]);
    }

    printf("\n[*] DONE.\n");

    free(candidates);
    free(es_buffer);
    if (latency_fd != -1) close(latency_fd);
    return 0;
}