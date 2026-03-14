#define _GNU_SOURCE
#include "utils.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <x86intrin.h>

// Helper macro for the hash function
#define GET_BIT(addr, bit) (((addr) >> (bit)) & 1)

// Recovered linear hash function for the i7-12700H (8 slices)
int get_cache_slice(uint64_t phys_addr) {
    int bit0 = GET_BIT(phys_addr, 6) ^ GET_BIT(phys_addr, 10) ^ GET_BIT(phys_addr, 12) ^ 
               GET_BIT(phys_addr, 14) ^ GET_BIT(phys_addr, 16) ^ GET_BIT(phys_addr, 17) ^ 
               GET_BIT(phys_addr, 18) ^ GET_BIT(phys_addr, 20) ^ GET_BIT(phys_addr, 22) ^ 
               GET_BIT(phys_addr, 24) ^ GET_BIT(phys_addr, 25) ^ GET_BIT(phys_addr, 26) ^ 
               GET_BIT(phys_addr, 27) ^ GET_BIT(phys_addr, 28) ^ GET_BIT(phys_addr, 30) ^ 
               GET_BIT(phys_addr, 32) ^ GET_BIT(phys_addr, 33);

    int bit1 = GET_BIT(phys_addr, 9) ^ GET_BIT(phys_addr, 12) ^ GET_BIT(phys_addr, 16) ^ 
               GET_BIT(phys_addr, 17) ^ GET_BIT(phys_addr, 19) ^ GET_BIT(phys_addr, 21) ^ 
               GET_BIT(phys_addr, 22) ^ GET_BIT(phys_addr, 23) ^ GET_BIT(phys_addr, 25) ^ 
               GET_BIT(phys_addr, 26) ^ GET_BIT(phys_addr, 27) ^ GET_BIT(phys_addr, 29) ^ 
               GET_BIT(phys_addr, 31) ^ GET_BIT(phys_addr, 32);

    int bit2 = GET_BIT(phys_addr, 10) ^ GET_BIT(phys_addr, 11) ^ GET_BIT(phys_addr, 13) ^ 
               GET_BIT(phys_addr, 16) ^ GET_BIT(phys_addr, 17) ^ GET_BIT(phys_addr, 18) ^ 
               GET_BIT(phys_addr, 19) ^ GET_BIT(phys_addr, 20) ^ GET_BIT(phys_addr, 21) ^ 
               GET_BIT(phys_addr, 22) ^ GET_BIT(phys_addr, 27) ^ GET_BIT(phys_addr, 28) ^ 
               GET_BIT(phys_addr, 30) ^ GET_BIT(phys_addr, 31) ^ GET_BIT(phys_addr, 33);

    return (bit2 << 2) | (bit1 << 1) | bit0;
}

void* allocate_huge_pages(size_t num_pages) {
    size_t size = num_pages * HUGE_PAGE_SIZE;
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

    if (ptr == MAP_FAILED) {
        printf("[!] Huge pages not available. Attempting to allocate %zu pages via OS...\n", num_pages);
        
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "echo %zu | sudo tee /proc/sys/vm/nr_hugepages > /dev/null", num_pages);
        if (system(cmd) != 0) {
            perror("[-] Failed to allocate huge pages. Ensure you run as root");
            exit(EXIT_FAILURE);
        }

        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (ptr == MAP_FAILED) {
            perror("[-] Huge pages allocation still failed");
            exit(EXIT_FAILURE);
        }
    }
    
    // Fault pages into physical memory
    for (size_t i = 0; i < num_pages; i++) {
        ((volatile char*)ptr)[i * HUGE_PAGE_SIZE] = 'A';
    }
    
    printf("[+] Successfully allocated %zu huge pages.\n", num_pages);
    return ptr;
}

uint64_t virt_to_phys(void *virtual_address) {
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("[-] Cannot open /proc/self/pagemap. Run as root");
        exit(EXIT_FAILURE);
    }

    uint64_t offset = ((uintptr_t)virtual_address / PAGE_SIZE) * sizeof(uint64_t);
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
        return 0;
    }

    uint64_t entry;
    if (read(fd, &entry, sizeof(uint64_t)) != sizeof(uint64_t)) {
        close(fd);
        return 0;
    }
    close(fd);

    if (!(entry & PAGEMAP_PRESENT_BIT)) return 0;

    uint64_t pfn = entry & PAGEMAP_PFN_MASK;
    return (pfn * PAGE_SIZE) + ((uintptr_t)virtual_address % PAGE_SIZE);
}

uint64_t measure_access_time(volatile void *addr) {
    uint64_t start, end;
    unsigned int aux;

    _mm_mfence();
    _mm_lfence();
    start = __rdtsc();
    _mm_lfence();

    (void)*(volatile uint8_t *)addr;

    _mm_lfence();
    end = __rdtscp(&aux);
    _mm_mfence();

    return end - start;
}

void maccess(volatile void *addr) {
    (void)*(volatile uint8_t *)addr;
}

void pin_cpu(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

void set_realtime_priority() {
    struct sched_param param;
    param.sched_priority = 99; 
    sched_setscheduler(0, SCHED_FIFO, &param);
}

void set_realtime_latency() {
    int32_t lat = 0; 
    int latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (latency_fd != -1){
        if (sizeof(lat) != write(latency_fd, &lat, sizeof(lat)))
        {
            printf("write() failed to write all bytes. errno = %d\n", errno);
            exit(1);
        }
    }
}
