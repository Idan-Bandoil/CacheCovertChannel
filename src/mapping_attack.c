#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "utils.h"
#ifdef PROBE_TIMING
#include <x86intrin.h>   /* _mm_clflush for the timing-distribution probe */
#endif
#if defined(NO_PREFETCH) || defined(FREQ_PROBE)
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef FREQ_PROBE
#include <time.h>
#endif
#ifdef NO_PREFETCH
// Measurement only (-DNO_PREFETCH): toggle the four Intel HW prefetchers on the
// pinned core via MSR 0x1A4 (MSR_MISC_FEATURE_CONTROL): bit0 L2 HWP, bit1 L2
// adjacent-line, bit2 DCU (L1), bit3 DCU-IP. 0xF disables all four. The binary
// runs as root (via sudoers), so it can open /dev/cpu/<core>/msr directly.
// ALWAYS restored to 0x0 on exit (atexit) so the machine is left untouched.
static int g_msr_core = -1;
static uint64_t rd_1a4(int core) {
    char p[64]; snprintf(p, sizeof(p), "/dev/cpu/%d/msr", core);
    int fd = open(p, O_RDONLY); if (fd < 0) return ~0ull;
    uint64_t v = ~0ull; if (pread(fd, &v, 8, 0x1A4) != 8) v = ~0ull;
    close(fd); return v;
}
static void wr_1a4(int core, uint64_t v) {
    char p[64]; snprintf(p, sizeof(p), "/dev/cpu/%d/msr", core);
    int fd = open(p, O_WRONLY); if (fd < 0) { perror("[-] open msr"); return; }
    if (pwrite(fd, &v, 8, 0x1A4) != 8) perror("[-] pwrite msr 0x1A4");
    close(fd);
}
static void restore_prefetch(void) {
    if (g_msr_core < 0) return;
    wr_1a4(g_msr_core, 0x0ull);
    printf("[*] NO_PREFETCH: restored MSR 0x1A4=0x%llx on core %d\n",
           (unsigned long long)rd_1a4(g_msr_core), g_msr_core);
    fflush(stdout);
}
#endif

#define NUM_PAGES 100
#define BOOTSTRAP_SET 0x5A
#define TARGET_EVICTION_COUNT (LLC_WAYS * 2)
#ifndef MISS_THRESHOLD
#define MISS_THRESHOLD 150
#endif

// Self-validating bootstrap retry. The two parity cosets are physically ~50/50,
// so a healthy run maps a comparable number of pages to each. A badly skewed
// split (one coset starved) is the signature of the dominant failure mode:
// under shared-L3 contention a seed's eviction set false-positives and absorbs
// pages of the OTHER coset, polluting the map. Balance is observable without
// ground truth, so we use it to reject a contaminated bootstrap and retry. This
// does NOT fix a noisy machine -- it just keeps re-rolling past the bad attempts
// (see docs/NOISE_REDUCTION_NOTES.md / run via scripts/quiet_run.sh).
#define BALANCE_MIN (NUM_PAGES / 4)     // each coset must reach >= 25 pages
#define MAX_BOOTSTRAP_ATTEMPTS 6

// A pruned set is provably SLICE-SPECIFIC once it shrinks to <= 2W-1 lines: to
// still evict the victim it must keep >= W lines in the victim's slice, which
// leaves <= W-1 for ALL other slices combined -- too few to evict any of them.
// So a set of this size evicts exactly the victim's slice. We stop pruning here
// (going to the marginal W-line minimum just invites timing-noise misreads) and
// use the size as the slice-specificity certificate when seeding a coset.
#define SLICE_SPECIFIC_MAX (2 * LLC_WAYS - 1)

// Max victims to try when seeking a slice-specific PRUNE to seed a coset. Under
// timing noise a prune sometimes stalls above SLICE_SPECIFIC_MAX (passengers
// misread as essential); those are rejected and we try a fresh victim.
#define SEED_RETRIES 25

// EM refinement rounds after both cosets are seeded (see remap_round). Each
// round re-classifies every page against the full slice oracles; the map
// converges in a couple of rounds, so this just caps the work.
#define MAX_REMAP_ROUNDS 3

// Eq. 7 (parity blindness): within a single huge page only bits 18-20 vary, and
// they satisfy S2 ^ S1 ^ S0 = 0, so a page's lines reach only 4 of the 8 slices
// -- one parity coset. Because every bootstrap candidate sits at the same set
// index, H_K has a fixed parity and the algebraic bridge can only ever connect
// pages within the SAME coset. The pool therefore splits into two cosets whose
// lines never conflict, so Algorithm 1 must anchor each coset independently
// (the paper's "iterate across parity groups"). For 8 slices that is 2 cosets.
#define NUM_PARITY_COSETS 2
#define NUM_SLICES 8  // K_max on the i7-12700H (s = log2(8) = 3 slice bits)
#define L2_WASH_LINES 40000 // 40,000 lines * 64 bytes = ~2.5MB (flushes 1.25MB L2 easily)

void *l2_wash_pool[L2_WASH_LINES];

// Initialize the L2 Wash Buffer
void init_l2_wash() {
    // Allocate 2 huge pages (4MB) just for wash data
    uint8_t *wash_memory = (uint8_t*)allocate_huge_pages(2);
    int ptr = 0;

    for (uint64_t off = 0; off < 2 * HUGE_PAGE_SIZE && ptr < L2_WASH_LINES; off += CACHE_LINE_SIZE) {
        int set = (off >> SET_INDEX_SHIFT) & SET_INDEX_MASK;
        // CRITICAL: We dodge our target L3 set!
        if (set != BOOTSTRAP_SET) {
            l2_wash_pool[ptr++] = wash_memory + off;
        }
    }
    printf("[+] Initialized L2 Wash Buffer with %d set-dodging lines.\n", ptr);
}

// Function to push everything out of L2 and down into L3
void wash_l2() {
    for (int i = 0; i < L2_WASH_LINES; i++) {
        maccess(l2_wash_pool[i]);
    }
}

// Helper: Calculate the slice hash using ONLY bits 0-20.
// Because it's an XOR function, the zeroed upper bits have no effect on the known bits' sum.
int get_known_hash(void *vaddr) {
    uint64_t addr = (uintptr_t)vaddr;
    uint64_t masked_addr = addr & ((1ULL << 21) - 1);
    return get_cache_slice(masked_addr);
}

// Helper: Tests if a group of addresses successfully evicts the victim
bool test_group(uint8_t *victim, uint8_t **group, int size) {
    // 1. Ensure firmly cached in L1/L2
    maccess(victim);
    maccess(victim);

    // 2. Wash L2 to push the victim down to the L3
    wash_l2();

    // 3. Thrash the set
    for (int sweep = 0; sweep < 3; sweep++) {
        // Bring candidates into L1/L2
        for (int i = 0; i < size; i++) maccess(group[i]);
        
        // Push candidates down to L3 so they conflict with the victim!
        wash_l2();
    }

    warm_tlb(victim);
    return measure_access_time(victim) >= MISS_THRESHOLD;
}

#ifndef ROBUST_TESTS
#define ROBUST_TESTS 5
#endif
#ifndef ROBUST_THRESHOLD
#define ROBUST_THRESHOLD 3
#endif

bool test_group_robust(uint8_t *victim, uint8_t **group, int size) {
    int misses = 0;
    for(int t = 0; t < ROBUST_TESTS; t++) {
        if (test_group(victim, group, size)) misses++;
    }
    // Return true only if it consistently evicts (majority vote)
    return misses >= ROBUST_THRESHOLD;
}

// Strict eviction vote for MEMBERSHIP decisions (which slice a line belongs to).
// A correct same-slice eviction set evicts its victim ~every trial, while a
// wrong-slice set evicts it only occasionally (residual timing noise gives a
// ~15-30% false-positive rate per single test). Those two regimes are far
// apart, so a high-threshold vote over more trials separates them cleanly --
// crucial here because a misclassification writes a wrong per-page Delta and
// (via the oracles) cascades. The prune keeps its own lighter, tuned vote
// (ROBUST_TESTS/THRESHOLD); this stricter one is only for classification.
#define MEMBERSHIP_TRIALS 7
#define MEMBERSHIP_THRESHOLD 5
bool evicts_strict(uint8_t *victim, uint8_t **group, int size) {
    int misses = 0;
    for (int t = 0; t < MEMBERSHIP_TRIALS; t++)
        if (test_group(victim, group, size)) misses++;
    return misses >= MEMBERSHIP_THRESHOLD;
}

// Phase 1: Robust Pruning Algorithm
bool find_eviction_set(uint8_t *victim, uint8_t **pool, int pool_size, uint8_t **eviction_set_out, int *out_size) {
    // 1. Verify the whole pool works as a baseline
    if (!test_group_robust(victim, pool, pool_size)) {
        return false; 
    }

    // 2. Setup a working array we can shrink
    uint8_t *working_set[pool_size];
    for (int i = 0; i < pool_size; i++) working_set[i] = pool[i];
    int w_size = pool_size;

    // 3. Prune until the set is slice-specific (<= SLICE_SPECIFIC_MAX lines).
    //
    // One linear pass discards every line whose removal leaves the set still
    // evicting. A non-conflicting line (different slice) is always removable, so
    // pruning sheds them and drives the set toward the victim's slice. We stop
    // as soon as the set is provably slice-specific (size <= 2W-1, see
    // SLICE_SPECIFIC_MAX) rather than chasing the exact W-line minimum: at the
    // W-line margin the eviction test is noise-marginal and starts misreading
    // removals, which only bloats the set with passengers. Stopping early keeps
    // a healthy eviction margin AND guarantees a single-slice set.
    for (int i = 0; i < w_size; ) {
        uint8_t *candidate = working_set[i];

        // Temporarily remove candidate by shifting left
        for (int j = i; j < w_size - 1; j++) working_set[j] = working_set[j + 1];
        w_size--;

        if (test_group_robust(victim, working_set, w_size)) {
            // Still evicts without the candidate: non-essential. Discard it (do
            // NOT increment i -- a new element shifted into index i).
            if (w_size <= SLICE_SPECIFIC_MAX) break;  // slice-specific: done
        } else {
            // Eviction failed: the candidate is essential. Put it back at i.
            for (int j = w_size; j > i; j--) working_set[j] = working_set[j - 1];
            working_set[i] = candidate;
            w_size++;
            i++; // Move on to test the next element
        }
    }

    for (int i = 0; i < w_size; i++) eviction_set_out[i] = working_set[i];
    *out_size = w_size;
    return w_size >= LLC_WAYS;
}

int create_candidate_pool(int set_idx, int required_candidates, uint8_t* huge_pages_base, 
    uint8_t*** candidate_pool_out)
{
    uint8_t **candidate_pool = malloc(sizeof(uint8_t*) * required_candidates);
    int pool_index = 0;

    for (int p = 0; (p < NUM_PAGES) && (pool_index < required_candidates); p++) {
        uint8_t *page_base = huge_pages_base + (p * HUGE_PAGE_SIZE);
        
        // Loop through the 8 combinations of bits 18, 19, 20
        for (uint64_t variation = 0; variation < 8; variation++) {
            uint64_t offset = (variation << 18) | (set_idx << SET_INDEX_SHIFT);
            candidate_pool[pool_index] = page_base + offset;
            pool_index++;
        }
    }

    *candidate_pool_out = candidate_pool;

    return (pool_index < required_candidates) ? 0 : 1;
}

// Build an eviction set for cache set `set_idx`, RELATIVE slice `rel_slice`,
// taking one line from every ALREADY-MAPPED page whose recovered Delta lands a
// line of that page in the target cell. `exclude_page` (or -1) is skipped so
// the set never contains a line on the page we are about to classify. Caps at
// `cap` lines, one line per page (extra lines per page cluster in L2 and waste
// the page's other slices). Returns the number of lines collected.
//
// This is Algorithm 2's cell-fill specialised to a single cell, and it doubles
// as the membership oracle used during bootstrap. Because Delta is per-page, a
// single page feeds whichever of its 4 reachable slices matches, so one
// well-mapped coset populates ALL of its slice oracles at once.
int build_relslice_set(int set_idx, int rel_slice, uint8_t *pages, bool *page_mapped,
                       int *delta, int exclude_page, int cap, uint8_t **out) {
    int n = 0;
    for (int p = 0; p < NUM_PAGES && n < cap; p++) {
        if (!page_mapped[p] || p == exclude_page) continue;
        uint8_t *page_base = pages + (uintptr_t)p * HUGE_PAGE_SIZE;
        for (uint64_t variation = 0; variation < 8; variation++) {
            uint8_t *candidate = page_base +
                ((variation << 18) | ((uint64_t)set_idx << SET_INDEX_SHIFT));
            if ((get_known_hash(candidate) ^ delta[p]) == rel_slice) {
                out[n++] = candidate;
                break;  // one line per page
            }
        }
    }
    return n;
}

// Membership oracle: is `victim` (sitting at cache set BOOTSTRAP_SET) in
// relative slice `rel_slice`? Build the (BOOTSTRAP_SET, rel_slice) eviction set
// from already-mapped pages (excluding the victim's own page) and robust-test
// whether it evicts the victim.
//
// This is what makes the bootstrap noise-robust. A slice-s' eviction set is
// built entirely from lines in slice s'; if the victim lives in a different
// slice it shares no cache line with any of them, so a correctly-mapped oracle
// CANNOT evict it -- a wrong-slice match is physically impossible rather than
// merely unlikely. And the oracle carries up to 2W lines (a healthy eviction
// margin), unlike a freshly pruned minimal set sitting right at the W-line
// margin where timing noise dominates. We bail out as "no match" when fewer
// than W mapped pages reach the slice: too thin to trust, so we defer to a
// later, larger oracle.
bool victim_in_relslice(uint8_t *victim, int victim_page, int rel_slice,
                        uint8_t *pages, bool *page_mapped, int *delta) {
    uint8_t *oracle[TARGET_EVICTION_COUNT];
    int n = build_relslice_set(BOOTSTRAP_SET, rel_slice, pages, page_mapped, delta,
                               victim_page, TARGET_EVICTION_COUNT, oracle);
    if (n < LLC_WAYS) return false;
    return evicts_strict(victim, oracle, n);
}

// Line at (page p, in-page variation `var`, cache set BOOTSTRAP_SET).
static inline uint8_t *bootstrap_line(uint8_t *pages, int p, uint64_t var) {
    return pages + (uintptr_t)p * HUGE_PAGE_SIZE +
           ((var << 18) | (BOOTSTRAP_SET << SET_INDEX_SHIFT));
}

// True if `victim` is already claimed by one of the slice oracles built from the
// currently-mapped pages -- i.e. it belongs to a coset we have already anchored.
// Used so a fresh coset seed starts from a page in a genuinely NEW coset.
bool in_any_anchored_slice(uint8_t *victim, int victim_page,
                           uint8_t *pages, bool *page_mapped, int *delta) {
    for (int s = 0; s < NUM_SLICES; s++)
        if (victim_in_relslice(victim, victim_page, s, pages, page_mapped, delta))
            return true;
    return false;
}

// EM-style refinement round. Re-classify every page against the current slice
// oracles, overwrite its Delta, and return how many pages changed. Oracles are
// built from a SNAPSHOT of the map taken at the round's start, so every page is
// classified against the same oracles (no mid-round drift). A page is assigned
// to the unique slice whose oracle evicts it; 0 or >1 matches => left for a
// later round.
//
// Why this converges: each coset seed plants >= W genuine lines in ALL FOUR of
// its slices (a correctly-mapped page contributes one line to each of its four
// reachable slices), so every slice oracle starts majority-correct. A
// majority-correct oracle still has >= W genuine lines (so it evicts its own
// slice's victims) and < W wrong-slice lines spread thin (so it evicts no other
// slice) -- it classifies correctly despite the errors. Re-mapping therefore
// corrects the passenger pages a seed propagated with the wrong slice label and
// maps pages the seeds never touched, and the oracles only get cleaner.
int remap_round(uint8_t *pages, bool *page_mapped, int *delta, int *mapped_count) {
    int snap_delta[NUM_PAGES];
    bool snap_mapped[NUM_PAGES];
    for (int p = 0; p < NUM_PAGES; p++) { 
        snap_delta[p] = delta[p]; 
        snap_mapped[p] = page_mapped[p]; 
    }

    int changed = 0;
    for (int p = 0; p < NUM_PAGES; p++) {
        uint8_t *v = bootstrap_line(pages, p, 0);   // representative line of page p
        int match_s = -1, matches = 0;
        for (int s = 0; s < NUM_SLICES && matches < 2; s++) {
            uint8_t *oracle[TARGET_EVICTION_COUNT];
            int n = build_relslice_set(BOOTSTRAP_SET, s, pages, snap_mapped, snap_delta,
                                       p, TARGET_EVICTION_COUNT, oracle);
            if (n >= LLC_WAYS && evicts_strict(v, oracle, n)) { 
                matches++; 
                match_s = s;
            }
        }
        if (matches != 1) continue;   // ambiguous / unclaimed: leave for a later round
        int nd = get_known_hash(v) ^ match_s;   // rel_slice = H_K ^ delta = match_s
        if (!page_mapped[p]) { 
            page_mapped[p] = true; 
            (*mapped_count)++; changed++; 
        }
        else if (delta[p] != nd) changed++;
        delta[p] = nd;
    }
    return changed;
}

// Anchor a brand-new parity coset by PRUNING one slice-specific eviction set and
// propagating its Delta to the pages it spans (Eq. 5). This is the only timed
// PRUNE per coset; remap_round then maps and refines the rest.
//
// We need a slice-SPECIFIC set -- one that evicts only the victim's slice. The
// size certificate gives it: a set evicting the victim with <= SLICE_SPECIFIC_MAX
// lines holds >= W in the victim's slice and < W in every other, so it evicts
// that slice alone. A prune that stalled larger (timing noise kept passengers)
// is rejected; we also strict re-test, since the prune's lenient 5/3 vote
// occasionally passes a noise artifact that lacks W genuine lines.
//
// Crucially, a slice-specific set contains NO lines of the OTHER coset: those
// never conflict with the victim and are always pruned out. So propagating it
// can only map pages of THIS coset -- it can never swallow the other coset's
// pages. That is what lets us anchor BOTH cosets before any refinement. For a
// later coset we also require the victim to be unclaimed by the already-anchored
// slices, so it genuinely lives in a new coset.
//
// Coset 0 uses base 0 (even frame), coset 1 base 1 (odd frame): the differing
// parity keeps the two cosets in DISJOINT slice-parity classes, so an eviction
// set built for any one slice draws from a single coset. The true offset between
// coset frames is unrecoverable from L3 timing (the cosets share no conflict,
// Eq. 7), so each coset is its own coordinate frame -- correct for building
// eviction sets, slice labels consistent only within a coset
// (docs/4KB_THEORY_GLOBAL_MAPPING.md sec 4.5).
bool seed_new_coset(int base, uint8_t **candidate_pool, int total_candidates,
                    uint8_t *pages, bool *page_mapped, int *delta, int *mapped_count) {
    int attempts = 0;
    for (int seed_page = 0; seed_page < NUM_PAGES && attempts < SEED_RETRIES; seed_page++) {
        if (page_mapped[seed_page]) continue;
        uint8_t *victim = bootstrap_line(pages, seed_page, 0);

        // For a later coset, skip victims an anchored coset already claims.
        if (base > 0 && in_any_anchored_slice(victim, seed_page, pages, page_mapped, delta))
            continue;

        uint8_t *current_pool[total_candidates];
        int cps = 0;
        for (int i = 0; i < total_candidates; i++)
            if (candidate_pool[i] != victim) current_pool[cps++] = candidate_pool[i];
        shuffle_addresses_randomly(current_pool, cps);

        uint8_t *E[total_candidates];
        int ev = 0;
        if (!find_eviction_set(victim, current_pool, cps, E, &ev)) continue;
        attempts++;

        if (ev > SLICE_SPECIFIC_MAX) {
            printf("[*] coset #%d: page %d PRUNE stalled at %d lines (> %d), retrying.\n",
                   base, seed_page, ev, SLICE_SPECIFIC_MAX);
            continue;
        }
        if (!evicts_strict(victim, E, ev)) {
            printf("[*] coset #%d: page %d PRUNE (%d lines) failed strict re-test, retrying.\n",
                   base, seed_page, ev);
            continue;
        }

        // E is a clean s_rel oracle: >= W genuine lines in the victim's slice
        // (s_rel) and < W in every other slice, and -- being slice-specific --
        // ZERO lines of the other coset. So "E evicts line L" <=> "L lies in
        // slice s_rel", a healthy >= W-line test. Map every page that has a line
        // in s_rel by probing its variations: the one E evicts is in s_rel, and
        // Delta follows from Eq. 5 (S = H_K ^ Delta = s_rel). A page of the OTHER
        // coset has no line in s_rel, so E evicts none of its variations and it
        // is left for the next coset -- propagating only E's own lines would
        // instead carry its passenger lines (other slices of THIS coset) and
        // mis-map them, so we classify each page directly against E instead.
        int s_rel = get_known_hash(victim) ^ base;
        int mapped_here = 0;
        for (int p = 0; p < NUM_PAGES; p++) {
            if (page_mapped[p]) continue;
            for (uint64_t var = 0; var < 8; var++) {
                uint8_t *L = bootstrap_line(pages, p, var);
                if (evicts_strict(L, E, ev)) {   // E evicts L  =>  L in slice s_rel
                    delta[p] = get_known_hash(L) ^ s_rel;
                    page_mapped[p] = true;
                    (*mapped_count)++;
                    mapped_here++;
                    break;
                }
            }
        }
        printf("[*] Seeded coset #%d from page %d (%d-line slice-specific PRUNE): %d pages mapped (base delta %d).\n",
               base, seed_page, ev, mapped_here, base);
        return true;
    }
    return false;
}

// Phase 1 & 2: Iterative Bootstrapping and Algebraic Alignment
//
// Algorithm 1 ("Bootstrapping and Page Alignment"), realising the paper's
// "iterate across parity groups". Parity blindness (Eq. 7) splits the candidate
// pool into NUM_PARITY_COSETS cosets whose lines never conflict, so no single
// eviction set can reach both -- the single-anchor predecessor of this code
// recovered only one coset. We instead:
//
//   1. Anchor every coset (seed_new_coset): one slice-specific PRUNE per coset,
//      used as a clean oracle to map (most of) that coset's pages by membership.
//      A coset-0 seed contains no coset-1 lines, so it leaves the other coset's
//      pages for the next seed.
//   2. Refine (remap_round): re-classify every page against the full set of
//      slice oracles, correcting any page a seed labelled with the wrong slice
//      and mapping the pages the seeds missed.
//
// Only NUM_PARITY_COSETS timed PRUNE searches happen (one seed per coset);
// everything else is membership testing and arithmetic. The two cosets live in
// separate coordinate frames (their offset is unrecoverable from L3 timing --
// Eq. 7), which is sufficient for building per-(set,slice) eviction sets.
int bootstrap_page_alignment(uint8_t **candidate_pool, int total_candidates, uint8_t *pages, bool *page_mapped, int *delta) {
    int mapped_count = 0;
    int num_cosets = 0;

    // (1) Anchor BOTH parity cosets up front, before any membership saturation.
    //     Each seed maps (most of) its own coset via its verified slice-specific
    //     oracle; the pages it leaves behind belong to the OTHER coset (Eq. 7),
    //     which the next seed then anchors. Saturating between seeds would be a
    //     mistake: a single membership false-positive could let the first coset
    //     swallow the other coset's pages, leaving none to seed coset 1.
    while (num_cosets < NUM_PARITY_COSETS && mapped_count < NUM_PAGES) {
        if (!seed_new_coset(num_cosets, candidate_pool, total_candidates,
                            pages, page_mapped, delta, &mapped_count))
            break;  // no unmapped page could anchor another coset
        num_cosets++;
    }

    // (2) EM refinement: re-classify every page against the full set of slice
    //     oracles until the map stops changing (or the round cap). This corrects
    //     passenger pages the seeds propagated with a wrong slice label and maps
    //     the pages the seeds never reached.
    for (int round = 0; round < MAX_REMAP_ROUNDS; round++) {
        int changed = remap_round(pages, page_mapped, delta, &mapped_count);
        printf("[*] Refinement round %d: %d pages (re)classified; %d/%d mapped.\n",
               round + 1, changed, mapped_count, NUM_PAGES);
        if (changed == 0) break;
    }

    printf("[*] Anchored %d parity coset(s); %d/%d pages aligned.\n",
           num_cosets, mapped_count, NUM_PAGES);
    return mapped_count;
}

void print_page_alignments(bool* page_mapped, int* delta, int mapped_count)
{
    printf("\n=========================================\n");
    printf("        ALGEBRAIC PAGE ALIGNMENT         \n");
    printf("=========================================\n");
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_mapped[i]) {
            printf("Page %02d: Relative Slice Offset (Delta) = %d\n", i, delta[i]);
        }
    }
    printf("-----------------------------------------\n");

    // Per-coset coverage. Within a coset every recovered delta shares the
    // anchor's parity, so counting pages by delta parity tells us whether the
    // bootstrap escaped parity blindness (Eq. 7) and reached BOTH cosets.
    int coset_pages[NUM_PARITY_COSETS] = {0};
    for (int i = 0; i < NUM_PAGES; i++) {
        if (page_mapped[i]) coset_pages[__builtin_parity(delta[i])]++;
    }

    // Which 4 slices each coset's pages occupy is COMPUTED from the slice hash,
    // not hardcoded. A page's reachable rel-slices are { get_known_hash(var) ^
    // delta }; the in-page variation bits contribute parity 0 (Eq. 7), so every
    // bootstrap candidate shares one known-hash parity and the reachable set
    // depends only on the delta's parity -- coset c (delta parity c) occupies
    // slices { get_known_hash(var) ^ c }. We enumerate that directly so the
    // labels stay correct for any BOOTSTRAP_SET. (The old hardcoded text had the
    // two cosets backwards: at set 0x5A every candidate's known-hash is odd, so
    // even-delta pages live in the ODD slices {1,2,4,7}, not {0,3,5,6}.)
    bool coset_slices[NUM_PARITY_COSETS][NUM_SLICES] = {{false}};
    for (uint64_t var = 0; var < 8; var++) {
        int h = get_known_hash((void *)(uintptr_t)((var << 18) | (BOOTSTRAP_SET << SET_INDEX_SHIFT)));
        for (int c = 0; c < NUM_PARITY_COSETS; c++) coset_slices[c][h ^ c] = true;
    }
    for (int c = 0; c < NUM_PARITY_COSETS; c++) {
        printf("Coset %d (%s delta -> slices {", c, c ? "odd " : "even");
        for (int s = 0, first = 1; s < NUM_SLICES; s++)
            if (coset_slices[c][s]) { printf("%s%d", first ? "" : ",", s); first = 0; }
        printf("}): %d pages\n", coset_pages[c]);
    }
    printf("Successfully aligned %d/%d pages across %d/%d parity cosets.\n",
           mapped_count, NUM_PAGES,
           (coset_pages[0] > 0) + (coset_pages[1] > 0), NUM_PARITY_COSETS);
    printf("=========================================\n");
}

// Phase 3 / Algorithm 2 (Global Cache Mapping): generate a perfect eviction set
// for any (target_set, target_slice) by pointer arithmetic on the recovered
// Deltas -- no timing. `target_slice` is the slice index in the recovered
// coordinate frame; since the two parity cosets occupy disjoint slice-parity
// classes, a given target_slice is served by exactly one coset and the lines
// genuinely co-reside. Returns the number of lines found (one per mapped page,
// capped at TARGET_EVICTION_COUNT).
int build_target_eviction_set(int target_set, int target_slice, uint8_t *pages,
                              bool *page_mapped, int *delta, uint8_t **eviction_set_out) {
    return build_relslice_set(target_set, target_slice, pages, page_mapped, delta,
                              -1, TARGET_EVICTION_COUNT, eviction_set_out);
}

#if defined(PROBE_TIMING) || defined(FREQ_PROBE)
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
#endif
#ifdef PROBE_TIMING
// Order statistics + threshold-crossing rate for a sample of TSC-tick deltas.
// `above` selects which crossing is the error: 1 => count v >= thr (false MISS
// for a cache-resident sample), 0 => count v < thr (false HIT for a DRAM sample).
static void print_dist(const char *name, uint64_t *v, int n, int thr, int above) {
    qsort(v, n, sizeof(uint64_t), cmp_u64);
    int cross = 0; double s = 0;
    for (int i = 0; i < n; i++) { s += v[i]; if (above ? (v[i] >= thr) : (v[i] < thr)) cross++; }
    printf("  %-9s n=%d min=%lu p1=%lu p10=%lu med=%lu p90=%lu p99=%lu max=%lu mean=%.1f | %s%d: %d (%.1f%%)\n",
           name, n, v[0], v[n/100], v[n/10], v[n/2], v[(int)(n*0.90)], v[(int)(n*0.99)], v[n-1], s/n,
           above ? ">=" : "<", thr, cross, 100.0 * cross / n);
}
#endif

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <core_id>\n", argv[0]);
        return 1;
    }
    int core_id = atoi(argv[1]);

    // Fully buffer stdout. The per-victim status prints in
    // bootstrap_page_alignment otherwise force a write() syscall per
    // iteration, which can deschedule the measurement core and pollute cache
    // state. We fflush() at phase boundaries below so progress is still
    // visible to a human watcher.
    setvbuf(stdout, NULL, _IOFBF, 0);

    pin_cpu(core_id);
#ifdef NO_PREFETCH
    g_msr_core = core_id;
    printf("[*] NO_PREFETCH: MSR 0x1A4 before = 0x%llx\n", (unsigned long long)rd_1a4(core_id));
    wr_1a4(core_id, 0xFull);
    printf("[*] NO_PREFETCH: MSR 0x1A4 after  = 0x%llx (0xf => all 4 HW prefetchers OFF)\n",
           (unsigned long long)rd_1a4(core_id));
    atexit(restore_prefetch);   // leave the machine untouched no matter how we exit
#endif
    set_realtime_priority();
    set_realtime_latency();

    // NUM_PAGES for the bootstrap arena + 2 for init_l2_wash().
    ensure_huge_pages_available(NUM_PAGES + 2);

    uint8_t *pages = (uint8_t*)allocate_huge_pages(NUM_PAGES);

    init_l2_wash();
    fflush(stdout);  // setup status out before the (long, quiet) bootstrap

#ifdef PROBE_TIMING
    // -----------------------------------------------------------------------
    // Timing-distribution probe (measurement only; -DPROBE_TIMING). Uses the
    // REAL measure_access_time + wash_l2 on THIS core to characterise the two
    // distributions MISS_THRESHOLD must separate:
    //   survivor : a cache-resident line (washed down to L2/L3) -- the "victim
    //              survived" case; must read BELOW threshold to be a HIT.
    //   dram     : a clflush'd line -- the "victim evicted" case; must read
    //              AT/ABOVE threshold to be a MISS.
    // The crossing-rate column is the per-core false-miss / false-hit rate the
    // fixed threshold induces -- exactly what gates the attack's stability.
    {
        const int NS = 2000;
        uint64_t *l1 = malloc(NS * sizeof(uint64_t));
        uint64_t *sv = malloc(NS * sizeof(uint64_t));
        uint64_t *dr = malloc(NS * sizeof(uint64_t));
        uint8_t *probe = pages + 0x1000 * CACHE_LINE_SIZE; // arbitrary mapped line
        for (int i = 0; i < NS; i++) {
            maccess(probe); maccess(probe);              // L1-resident
            warm_tlb(probe); l1[i] = measure_access_time(probe);

            maccess(l2_wash_pool[0]); wash_l2();         // pushed down to L2/L3
            warm_tlb(l2_wash_pool[0]); sv[i] = measure_access_time(l2_wash_pool[0]);

            _mm_clflush(probe);                          // forced to DRAM
            warm_tlb(probe); dr[i] = measure_access_time(probe);
        }
        printf("\n===== TIMING PROBE (core %d, MISS_THRESHOLD=%d) =====\n", core_id, MISS_THRESHOLD);
        print_dist("l1hit",    l1, NS, MISS_THRESHOLD, 1);  // expect ~0% >= thr
        print_dist("survivor", sv, NS, MISS_THRESHOLD, 1);  // false-MISS rate
        print_dist("dram",     dr, NS, MISS_THRESHOLD, 0);  // false-HIT  rate
        printf("=====================================================\n");
        fflush(stdout);
        free(l1); free(sv); free(dr);
        return 0;
    }
#endif

#ifdef FREQ_PROBE
    // Delivered-core-frequency probe (measurement only; -DFREQ_PROBE). Reads
    // IA32_APERF (MSR 0xE8 = actual core clock ticks) around attack-like memory
    // load (wash_l2) and divides by CLOCK_MONOTONIC wall time -> the frequency
    // the core ACTUALLY delivers, sampled sub-millisecond. This is the
    // high-resolution view that scaling_cur_freq (20 Hz, time-averaged) hides.
    {
        char mp[64]; snprintf(mp, sizeof(mp), "/dev/cpu/%d/msr", core_id);
        int fd = open(mp, O_RDONLY);
        if (fd < 0) { perror("[-] FREQ_PROBE open msr (need msr module + root)"); return 1; }
        enum { NS = 2500, WPS = 6 };
        static uint64_t mhz[NS];
        struct timespec t0, t1; uint64_t a0, a1;
        if (pread(fd, &a0, 8, 0xE8) != 8) { perror("[-] pread APERF"); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < NS; i++) {
            for (int w = 0; w < WPS; w++) wash_l2();   // attack-like memory load
            pread(fd, &a1, 8, 0xE8);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            mhz[i] = dt > 0 ? (uint64_t)((double)(a1 - a0) / dt / 1e6) : 0;
            a0 = a1; t0 = t1;
        }
        close(fd);
        qsort(mhz, NS, sizeof(uint64_t), cmp_u64);
        double s = 0; for (int i = 0; i < NS; i++) s += mhz[i];
        printf("\n===== FREQ PROBE (core %d, delivered core MHz under wash load, n=%d) =====\n", core_id, NS);
        printf("  min=%lu p1=%lu p10=%lu med=%lu p90=%lu p99=%lu max=%lu mean=%.0f spread(p99-p1)=%lu\n",
               mhz[0], mhz[NS/100], mhz[NS/10], mhz[NS/2], mhz[(int)(NS*0.90)],
               mhz[(int)(NS*0.99)], mhz[NS-1], s / NS, mhz[(int)(NS*0.99)] - mhz[NS/100]);
        printf("=========================================================================\n");
        fflush(stdout);
        return 0;
    }
#endif

    int total_candidates = NUM_PAGES * 8;
    uint8_t **candidate_pool = malloc(sizeof(uint8_t*) * total_candidates);
    if (!create_candidate_pool(BOOTSTRAP_SET, total_candidates, pages, &candidate_pool))
    {
        printf("create_candidate_pool() failed to find enough candidates!\n");
        return 1;
    }
    
    int delta[NUM_PAGES];
    bool page_mapped[NUM_PAGES];

    // Bootstrap, retrying while the parity cosets come out badly imbalanced
    // (the contention signature -- see BALANCE_MIN).
    int mapped_count = 0;
    bool balanced = false;
    for (int attempt = 1; attempt <= MAX_BOOTSTRAP_ATTEMPTS && !balanced; attempt++) {
        for (int i = 0; i < NUM_PAGES; i++) page_mapped[i] = false;
        mapped_count = bootstrap_page_alignment(candidate_pool, total_candidates, pages, page_mapped, delta);
        int coset[2] = {0, 0};
        for (int i = 0; i < NUM_PAGES; i++)
            if (page_mapped[i]) coset[__builtin_parity(delta[i])]++;
        balanced = (coset[0] >= BALANCE_MIN && coset[1] >= BALANCE_MIN);
        printf(balanced ? "[+] Balanced bootstrap accepted on attempt %d (cosets %d/%d).\n"
                        : "[!] Bootstrap attempt %d imbalanced (cosets %d/%d) -- likely L3 contention; re-rolling.\n",
               attempt, coset[0], coset[1]);
    }
    if (!balanced)
        printf("[-] Could not balance the cosets in %d attempts. The shared L3 is too "
               "contended -- close browsers/IDEs and run via scripts/quiet_run.sh; "
               "the map below is unreliable.\n", MAX_BOOTSTRAP_ATTEMPTS);

    print_page_alignments(page_mapped, delta, mapped_count);

#ifdef GROUND_TRUTH
    // Ground-truth accuracy probe (virt_to_phys; MEASUREMENT ONLY -- compiled in
    // with -DGROUND_TRUTH for noise tuning, never part of the real attack, which
    // must not consult pagemap). Reports how frame-consistent the recovered map
    // actually is -- a clean signal independent of the noisy timing verification.
    {
        int true_delta_of[NUM_PAGES], true_split[2] = {0, 0};
        for (int p = 0; p < NUM_PAGES; p++) {
            uint8_t *L = pages + (uintptr_t)p * HUGE_PAGE_SIZE + (BOOTSTRAP_SET << SET_INDEX_SHIFT);
            true_delta_of[p] = get_known_hash(L) ^ get_cache_slice(virt_to_phys(L));
            true_split[__builtin_parity(true_delta_of[p])]++;
        }
        // A correct map has delta[p] = true_delta[p] ^ K with ONE constant K per
        // RECOVERED coset (the frame's unknown offset; can be odd, which flips
        // parity vs the true tag -- so we must group by recovered parity, not
        // true). Within each recovered coset, the dominant (delta ^ true_delta)
        // offset counts the consistently-mapped pages; pages off that offset are
        // frame-inconsistent or cross-coset contamination. This consistency is
        // exactly what makes an eviction set evict, so it predicts Phase 3.
        (void)true_split;
        int total_ok = 0;
        for (int rc = 0; rc < 2; rc++) {
            int off_hist[8] = {0}, mapped = 0;
            for (int p = 0; p < NUM_PAGES; p++)
                if (page_mapped[p] && __builtin_parity(delta[p]) == rc) {
                    off_hist[(delta[p] ^ true_delta_of[p]) & 7]++; mapped++;
                }
            int best = 0;
            for (int o = 1; o < 8; o++) if (off_hist[o] > off_hist[best]) best = o;
            total_ok += off_hist[best];
            printf("[DIAG] recovered coset %d: %d pages mapped, %d frame-consistent (offset %d)\n",
                   rc, mapped, off_hist[best], best);
        }
        printf("[DIAG] ACCURACY: %d/%d mapped pages frame-consistent.\n", total_ok, mapped_count);
    }
#endif // GROUND_TRUTH
    fflush(stdout);  // alignment summary out before Phase 3 starts

    // =========================================
    // PHASE 3: O(1) MATHEMATICAL BUCKETING
    // =========================================
    // Build an eviction set for every slice of one set, purely from the
    // recovered Deltas, and verify each by timing. Slices split by parity
    // across the two cosets (even {0,3,5,6} vs odd {1,2,4,7}), so verifying ALL
    // slices is the end-to-end proof that BOTH parity groups were recovered --
    // the whole point of escaping parity blindness.
    int target_set = 0x34;   // any set (0..4095)
    printf("[*] Phase 3: building + verifying an eviction set for every slice of set 0x%03X...\n", target_set);

    int verified = 0, buildable = 0;
#ifdef GROUND_TRUTH
    int correct_sets = 0;
#endif
    for (int target_slice = 0; target_slice < NUM_SLICES; target_slice++) {
        uint8_t *ev[TARGET_EVICTION_COUNT];
        int found = build_target_eviction_set(target_set, target_slice, pages,
                                              page_mapped, delta, ev);
        if (found < LLC_WAYS) {
            printf("    slice %d: only %2d lines (need >= %d) -- slice under-populated.\n",
                   target_slice, found, LLC_WAYS);
            continue;
        }
        buildable++;
        bool ok = evicts_strict(ev[0], ev + 1, found - 1);  // strict vote: end-of-run timing is noisy
        verified += ok;
#ifdef GROUND_TRUTH
        // virt_to_phys, no timing: how many built lines are actually co-resident
        // (largest same-true-slice group)? >= W means a genuinely correct set,
        // independent of whether the noisy timing check confirms it.
        int sl_hist[NUM_SLICES] = {0};
        for (int i = 0; i < found; i++) sl_hist[get_cache_slice(virt_to_phys(ev[i])) & 7]++;
        int genuine = 0;
        for (int s = 0; s < NUM_SLICES; s++) if (sl_hist[s] > genuine) genuine = sl_hist[s];
        if (genuine >= LLC_WAYS) correct_sets++;
        printf("    slice %d: %2d lines, %2d genuine (ground truth) -> %s\n",
               target_slice, found, genuine, ok ? "[+] VERIFIED" : "[-] absorbed");
#else
        printf("    slice %d: %2d lines -> %s\n", target_slice, found,
               ok ? "[+] VERIFIED (triggers L3 misses)" : "[-] hardware absorbed the thrash");
#endif
    }
#ifdef GROUND_TRUTH
    printf("[*] Phase 3: %d/%d slices VERIFIED (timing); %d/%d sets CORRECT (>=%d genuine, ground truth); %d buildable.\n",
           verified, NUM_SLICES, correct_sets, NUM_SLICES, LLC_WAYS, buildable);
#else
    printf("[*] Phase 3 result: %d/%d slices VERIFIED (%d buildable) for set 0x%03X.\n",
           verified, NUM_SLICES, buildable, target_set);
#endif

    free(candidate_pool);
    return 0;
}
