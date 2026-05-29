#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

// --- Hardware & OS Constants ---
#define PAGE_SIZE 4096
#define HUGE_PAGE_SIZE (2 * 1024 * 1024) // 2MB
#define CACHE_LINE_SIZE 64
#define PAGEMAP_PRESENT_BIT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)

// --- Cache Specifications (i7-12700H) ---
#define LLC_WAYS 12
#define LLC_SETS_PER_SLICE 4096
#define SET_INDEX_SHIFT 6
#define SET_INDEX_MASK 0xFFF // 12 bits for 4096 sets

// --- Process management ---
void pin_cpu(int core_id);
void set_realtime_priority();
void set_realtime_latency();

// --- Memory management ---
// Pre-flight: ensure at least num_pages huge pages are FREE in the kernel pool.
// If short, bumps /proc/sys/vm/nr_hugepages by the deficit. Call once at
// startup, before any allocate_huge_pages() call, so the pool is sized for
// everything this process will need.
void ensure_huge_pages_available(size_t num_pages);
void* allocate_huge_pages(size_t num_pages);
uint64_t virt_to_phys(void *virtual_address);

// --- Precise pipeline-serialized timing ---
uint64_t measure_access_time(volatile void *addr);
void maccess(volatile void *addr);

// Warm the TLB entry covering `addr` ahead of a timing measurement on it.
// The wash + sweep pattern can evict the victim's huge-page TLB entry (Alder
// Lake's huge-page DTLB is ~32 entries while a bootstrap candidate pool can
// span many more huge pages); without this, the rdtsc-bracketed load on the
// victim would include a page walk and inflate every cycle count.
//
// Implementation: a load on a sibling cache line within the same 4 KB / 2 MB
// page. The XOR offset 0x800 flips bit 11, which lives inside both page
// sizes, so the load's address translation hits the *same* TLB entry as the
// subsequent measurement will use. Bit 11 is also inside every cache level's
// set-index range, so the touched line lands at set ⊕ 32 at L1, L2, and L3 —
// disjoint from the victim's sets, so it cannot evict the victim from any
// level. Defined here (not as a per-binary macro) so all three executables
// share one definition. Caller must access `addr` itself separately.
static inline void warm_tlb(const volatile void *addr) {
    uint8_t dummy = *(const volatile uint8_t *)((uintptr_t)addr ^ 0x800);
    (void)dummy;
}

// --- Cache Mapping ---
int get_cache_slice(uint64_t phys_addr);

void shuffle_addresses_randomly(uint8_t** addresses, int addresses_count);

#endif // UTILS_H
