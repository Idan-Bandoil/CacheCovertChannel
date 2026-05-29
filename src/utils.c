#define _GNU_SOURCE
#include "utils.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <stdbool.h>
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

void shuffle_addresses_randomly(uint8_t** addresses, int addresses_count)
{
    static bool seeded = false;

    if (!seeded)
    {
        srand(1337);
        seeded = true;
    }

    for (int i = addresses_count - 1; i > 0; i--) 
    {
        int j = rand() % (i + 1);
        uint8_t *temp = addresses[i];
        addresses[i] = addresses[j];
        addresses[j] = temp;
    }
}

// Read /proc/sys/vm/nr_hugepages.
static size_t read_nr_hugepages(void) {
    FILE *f = fopen("/proc/sys/vm/nr_hugepages", "r");
    if (!f) return 0;
    size_t v = 0;
    if (fscanf(f, "%zu", &v) != 1) v = 0;
    fclose(f);
    return v;
}

// Read HugePages_Free from /proc/meminfo.
static size_t read_hugepages_free(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    size_t free_pages = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "HugePages_Free: %zu", &free_pages) == 1) break;
    }
    fclose(f);
    return free_pages;
}

// Bump /proc/sys/vm/nr_hugepages by `delta`. Always additive — never shrinks.
static int grow_huge_pool(size_t delta) {
    size_t current = read_nr_hugepages();
    size_t target = current + delta;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "echo %zu | sudo tee /proc/sys/vm/nr_hugepages > /dev/null", target);
    return system(cmd);
}

void ensure_huge_pages_available(size_t num_pages) {
    size_t free_pages = read_hugepages_free();
    if (free_pages >= num_pages) return;

    size_t deficit = num_pages - free_pages;
    printf("[*] Huge page pool short by %zu (free=%zu, need=%zu). Growing pool...\n",
           deficit, free_pages, num_pages);
    if (grow_huge_pool(deficit) != 0) {
        perror("[-] Failed to grow huge page pool. Run as root or extend sudoers");
        exit(EXIT_FAILURE);
    }

    size_t free_after = read_hugepages_free();
    if (free_after < num_pages) {
        fprintf(stderr,
                "[-] Pool growth requested but free count still %zu < %zu. "
                "Likely memory fragmentation; reboot or reduce NUM_PAGES.\n",
                free_after, num_pages);
        exit(EXIT_FAILURE);
    }
    printf("[+] Pool grown. HugePages_Free=%zu.\n", free_after);
}

void* allocate_huge_pages(size_t num_pages) {
    size_t size = num_pages * HUGE_PAGE_SIZE;
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

    if (ptr == MAP_FAILED) {
        // Fallback path: pool wasn't pre-sized (or got drained mid-process).
        // Grow the pool ADDITIVELY by num_pages — never overwrite the count,
        // or in-process allocations will starve each other.
        printf("[!] Huge pages exhausted. Growing pool by %zu...\n", num_pages);
        if (grow_huge_pool(num_pages) != 0) {
            perror("[-] Failed to grow huge page pool. Ensure you run as root");
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

    // Drain prior loads before reading the TSC. lfence is sufficient on Intel
    // since Sandy Bridge — it serialises with respect to older loads, which is
    // all we need: this function is called from load-only hot paths (wash +
    // sweep), so there is no pending store the heavier mfence would have to
    // drain. The second lfence prevents the timed load from issuing ahead of
    // rdtsc.
    _mm_lfence();
    start = __rdtsc();
    _mm_lfence();

    (void)*(volatile uint8_t *)addr;

    // The lfence here forces the timed load to retire before rdtscp executes,
    // tightening the upper edge of the measurement window. rdtscp is itself
    // pseudo-serialising (waits for older instructions to retire before reading
    // the counter) — the fence is defensive but cheap. The trailing lfence
    // stops the next-iteration code from issuing ahead of rdtscp; in a tight
    // measurement loop this matters more than a trailing mfence (mfence's
    // store-buffer drain costs cycles we never use).
    _mm_lfence();
    end = __rdtscp(&aux);
    _mm_lfence();

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
    // The kernel only honors the PM_QoS constraint while the fd remains open.
    // Keep it in a static so it lives for the lifetime of the process.
    static int latency_fd = -1;
    if (latency_fd != -1) return;

    int32_t lat = 0;
    latency_fd = open("/dev/cpu_dma_latency", O_RDWR);
    if (latency_fd != -1) {
        if (sizeof(lat) != write(latency_fd, &lat, sizeof(lat)))
        {
            printf("write() failed to write all bytes. errno = %d\n", errno);
            exit(1);
        }
    }
}
