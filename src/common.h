#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>      // For open
#include <sys/mman.h>   // For mmap
#include <unistd.h>     // For close

#define SHARED_FILE "shared_data.bin"
#define DATA_OFFSET 0
#define FLAG_OFFSET 64

// 10ms per bit (Very slow, very safe for debugging)
#define SLOT_DURATION 10000000 

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void maccess(void *p) {
    volatile uint32_t val = *(volatile uint32_t *)p;
    (void)val;
}

static inline uint64_t wait_for_next_slot() {
    uint64_t now = rdtsc();
    uint64_t next_slot = ((now / SLOT_DURATION) + 1) * SLOT_DURATION;
    while (rdtsc() < next_slot) { asm volatile("nop"); }
    return next_slot;
}

// NEW FUNCTION: Maps file with Read AND Write permissions
static inline void *map_shared_rw(const char *path) {
    int fd = open(path, O_RDWR); // Open as Read-Write
    if (fd == -1) {
        perror("Error opening file for RW");
        exit(1);
    }

    // Map 4096 bytes (1 page) with Write permissions
    void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); // We can close FD once mapped

    if (addr == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
    return addr;
}

#endif