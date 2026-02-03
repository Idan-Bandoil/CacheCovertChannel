#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHARED_FILE "shared_data.bin"

// --- MEMORY LAYOUT ---
#define OFFSET_DATA 0     // The Covert Channel Line
#define OFFSET_FLAG 64    // Handshake Flag (Receiver -> Sender)
#define OFFSET_LEN  128   // Metadata: Message Length (Sender -> Receiver)

// 10ms per bit (Robust Speed)
#define SLOT_DURATION 10000000 
#define CACHE_THRESHOLD 120

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

static inline void *map_shared_rw(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd == -1) { perror("open"); exit(1); }
    void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return addr;
}

#endif