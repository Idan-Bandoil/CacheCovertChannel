#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <x86intrin.h>
#include "utils.h"

/* ===========================================================================
 * slice_oracle — feasibility-first static interconnect-latency slice oracle
 *
 * Tests HYBRID_CORE_MAPPING.md §2, hypothesis H3: does an L3 hit's latency
 * depend on the line *only* through its physical slice, by a margin Δμ above
 * the per-core noise floor ζ?  Feasibility is the inequality Δμ ≳ ζ/√r.
 *
 * Method.  Build a pool of anchor lines whose TRUE slice is known from ground
 * truth (virt_to_phys + get_cache_slice, Eq. 6).  Self-stage each line into the
 * shared L3 and time it from every distinct physical core ("agent").  Then,
 * BEFORE any classifier, report the per-(agent,slice) latency gradient and its
 * statistical separability — so a negative result distinguishes "no signal"
 * from "weak classifier", which the prior attempt (12.5% = chance) could not.
 *
 * Primary output  : μ_{m,k} table + per-agent ANOVA F/η² + z-space Δμ vs noise.
 * Secondary output: a supervised nearest-centroid classifier vs 12.5% chance.
 *
 * Reuses src/utils.{c,h}.  Ground truth needs REAL root (sudo) or virt_to_phys
 * returns 0.  Run under performance governor with P-core SMT siblings idled.
 * ===========================================================================*/

#define K_SLICES   8
#define MAX_AGENTS 32

/* Compile-time knobs (#ifndef-guarded so a sweep needs no source edits). */
#ifndef NUM_HUGE_PAGES
#define NUM_HUGE_PAGES 20         /* anchor pool spread across many frames (multi-page control) */
#endif
#ifndef ANCHORS_PER_SLICE
#define ANCHORS_PER_SLICE 32      /* anchors timed per slice */
#endif
#ifndef REPEATS
#define REPEATS 150               /* staged measurements per (agent,anchor); median'd */
#endif
#ifndef L2EVICT_WAYS
#define L2EVICT_WAYS 32           /* set-congruent eviction lines per stage (≥ L2 associativity) */
#endif
#ifndef L2EVICT_PASSES
#define L2EVICT_PASSES 2          /* sweeps of the congruent set (beats L2 replacement policy) */
#endif
#define L2_SET_STRIDE (1u << 17)  /* 128 KB: a stride increment preserves L2 set-index bits 6..16 */
#ifndef CALIB_SAMPLES
#define CALIB_SAMPLES 256         /* per-core L1/staged/DRAM calibration medians */
#endif
#ifndef BAND_CAP_TICKS
#define BAND_CAP_TICKS 150        /* cap the L3-band upper edge at staged+this, to reject
                                     contention-inflated slow hits (ring gradient is ≪ 150 ticks) */
#endif
#ifndef LDA_SHRINKAGE
#define LDA_SHRINKAGE 0.2         /* γ: shrink the pooled within-class covariance toward a
                                     scaled identity (Ledoit-Wolf style) before inverting it.
                                     Keeps it positive-definite under sub-tick ζ and
                                     near-collinear agents; also regularizes 1/ζ² for inv-var. */
#endif
#ifndef MIN_KEPT_CLASSIFY
#define MIN_KEPT_CLASSIFY 0.25    /* agent-selection: an agent whose staging never settled
                                     (kept-ratio below this) is dropped from the separability
                                     metric and the classifier — its near-constant column is
                                     uninformative and poisons the variance-weighted fusions. */
#endif

#define NUM_ANCHORS (K_SLICES * ANCHORS_PER_SLICE)
/* L2 eviction buffer must span all L2EVICT_WAYS strides so every congruent line fits */
#define EVBUF_BYTES ((size_t)L2EVICT_WAYS * L2_SET_STRIDE)
#define EVBUF_PAGES ((EVBUF_BYTES + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE)

/* Default agents: the 14 distinct physical cores of the i7-12700H — one per
 * P-core SMT pair (0,2,4,6,8,10) plus all 8 E-cores (12..19). Overridable via a
 * comma list in argv[1].  Measuring all agents is strictly more informative:
 * E-only / P-only subsets are read off the same data. */
static const int DEFAULT_AGENTS[] = {0, 2, 4, 6, 8, 10, 12, 13, 14, 15, 16, 17, 18, 19};
#define NUM_DEFAULT_AGENTS ((int)(sizeof(DEFAULT_AGENTS) / sizeof(DEFAULT_AGENTS[0])))

/* State (static to keep these arrays off the stack). */
static uint8_t *anchors[NUM_ANCHORS];
static int      true_slice[NUM_ANCHORS];
static double   Phi[MAX_AGENTS][NUM_ANCHORS];     /* median staged-L3 latency, TSC ticks */
static double   kept_ratio[MAX_AGENTS];
static int      agents[MAX_AGENTS];
static int      num_agents;

/* i7-12700H: logical 0..11 = P-cores, 12..19 = E-cores. */
static inline int is_pcore(int cpu) { return cpu < 12; }

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static uint64_t median_u64(uint64_t *buf, int n) {
    if (n == 0) return 0;
    qsort(buf, n, sizeof(uint64_t), cmp_u64);
    return buf[n / 2];
}
/* Low-percentile estimator — robust to *upward* contamination: a contention
 * burst only inflates a staged-L3 sample, never deflates it, so the fast-L3
 * mode is recovered even when a chunk of the calibration ran slow. */
static uint64_t percentile_u64(uint64_t *buf, int n, int pct) {
    if (n == 0) return 0;
    qsort(buf, n, sizeof(uint64_t), cmp_u64);
    return buf[(int)((long)pct * (n - 1) / 100)];
}

/* Self-stage: dirty the line (now in L1), then evict it from this core's private
 * L1+L2 with a SET-CONGRUENT eviction set — lines sharing the line's L2 set-index
 * (addr bits 6..16) but differing above (stride 128 KB), so they collide in L1/L2
 * yet miss the line's own L3 set/slice.  The dirty victim writes back to L3 and
 * STAYS there (the eviction set does not thrash its LLC set), so the next timed
 * access is an L3 hit carrying the interconnect term g(d(core, slice)).  ~64
 * accesses vs a 64K-line blind wash: ~1000× less traffic, so it neither saturates
 * bandwidth nor evicts its own staged line under external contention.  No
 * cross-core staging → no c2c-forward confound. */
static inline void self_stage(volatile uint8_t *line, uint8_t *evbuf) {
    *line = 0x5A;
    uintptr_t setoff = (uintptr_t)line & ((L2_SET_STRIDE - 1) & ~(uintptr_t)(CACHE_LINE_SIZE - 1));
    for (int pass = 0; pass < L2EVICT_PASSES; pass++)
        for (int b = 0; b < L2EVICT_WAYS; b++)
            maccess(evbuf + (size_t)b * L2_SET_STRIDE + setoff);
}

/* Per-core residency calibration: median L1-hit, self-staged-L3, and clflush'd
 * DRAM latencies on the pinned core.  Used to set an L3-acceptance band so we
 * keep only confirmed L3 hits (residency is trimodal — L1-hit on stage failure,
 * L3-hit, DRAM on over-eviction). */
static void calibrate(volatile uint8_t *line, uint8_t *evbuf,
                      uint64_t *l1, uint64_t *staged, uint64_t *dram) {
    static uint64_t buf[CALIB_SAMPLES];

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        maccess(line); maccess(line);
        buf[i] = measure_access_time(line);
    }
    *l1 = median_u64(buf, CALIB_SAMPLES);

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        self_stage(line, evbuf);
        warm_tlb(line);
        buf[i] = measure_access_time(line);
    }
    *staged = percentile_u64(buf, CALIB_SAMPLES, 30);   /* robust to contention inflation */

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        _mm_clflush((const void *)line);
        _mm_mfence();
        buf[i] = measure_access_time(line);
    }
    *dram = median_u64(buf, CALIB_SAMPLES);
}

/* Calibrate, retrying if the staged-L3 estimate is contaminated.  A contention
 * burst during the staged loop can inflate `staged` into the DRAM band; the
 * acceptance window then lands above the true L3 hits and starves the agent
 * (kept ≈ 0, flat garbage column).  Require staged comfortably below the DRAM
 * floor (and above L1), else recalibrate in a fresh window. */
static void calibrate_robust(volatile uint8_t *line, uint8_t *evbuf,
                             uint64_t *l1, uint64_t *staged, uint64_t *dram, int cpu) {
    for (int attempt = 0; attempt < 5; attempt++) {
        calibrate(line, evbuf, l1, staged, dram);
        if (*staged < *dram * 13 / 20 && *staged > *l1 / 2) return;   /* staged < 0.65·dram */
        if (attempt < 4) {
            printf("    [retry %d] cpu%d staged=%lu vs dram=%lu looks contended; recalibrating...\n",
                   attempt + 1, cpu, *staged, *dram);
            usleep(5000);
        }
    }
}

/* ---- ground-truth anchor pool ------------------------------------------- */
static void build_anchors(uint8_t *base) {
    int count[K_SLICES] = {0};
    int total = 0;
    /* Cap anchors taken per (slice, page) so each slice's anchors spread across
     * many huge pages.  Same-slice lines on different frames must differ in addr
     * bits 6..20 (to hash to the same slice under different high bits), hence in
     * their L2-set index (bits 6..16) — this breaks the slice↔L2-set correlation
     * that exists when all anchors share one page, ruling out a staging/residency
     * confound masquerading as a ring gradient. */
    int per_page_cap = (ANCHORS_PER_SLICE + NUM_HUGE_PAGES - 1) / NUM_HUGE_PAGES;

    for (size_t p = 0; p < NUM_HUGE_PAGES && total < NUM_ANCHORS; p++) {
        uint8_t  *page_base = base + p * HUGE_PAGE_SIZE;
        uint64_t  phys_base = virt_to_phys(page_base);
        if (phys_base == 0) {
            fprintf(stderr, "[!] page %zu: virt_to_phys=0 (not root?), skipping\n", p);
            continue;
        }
        int page_count[K_SLICES] = {0};
        for (size_t off = 0; off < HUGE_PAGE_SIZE && total < NUM_ANCHORS; off += CACHE_LINE_SIZE) {
            int k = get_cache_slice(phys_base + off);
            if (count[k] >= ANCHORS_PER_SLICE) continue;
            if (page_count[k] >= per_page_cap) continue;
            int idx = k * ANCHORS_PER_SLICE + count[k];
            anchors[idx]    = page_base + off;
            true_slice[idx] = k;
            count[k]++;
            page_count[k]++;
            total++;
        }
    }

    printf("[*] Anchors per slice:");
    for (int k = 0; k < K_SLICES; k++) printf(" s%d=%d", k, count[k]);
    printf("\n");
    for (int k = 0; k < K_SLICES; k++) {
        if (count[k] < ANCHORS_PER_SLICE) {
            fprintf(stderr, "[-] Slice %d under-sampled (%d/%d). Raise NUM_HUGE_PAGES.\n",
                    k, count[k], ANCHORS_PER_SLICE);
            exit(EXIT_FAILURE);
        }
    }
}

/* ---- measurement -------------------------------------------------------- */
static void measure_all(uint8_t *evbuf) {
    static uint64_t sbuf[REPEATS];

    for (int am = 0; am < num_agents; am++) {
        pin_cpu(agents[am]);
        set_realtime_priority();

        uint64_t l1, staged, dram;
        calibrate_robust(anchors[0], evbuf, &l1, &staged, &dram, agents[am]);
        /* L3-acceptance band.  Lower edge sits between the L1-hit mode and the
         * staged-L3 mode; upper edge between staged and DRAM.  If the L1
         * calibration glitched (l1 >= staged — observed once on cpu19 as a
         * spurious l1=630 from an interrupt during the L1 loop), the naive
         * (l1+staged)/2 inverts the band and starves the agent (0% kept), so
         * fall back to 3/4 of the staged latency for the lower edge. */
        uint64_t hi = (staged + dram) / 2;
        if (hi > staged + BAND_CAP_TICKS) hi = staged + BAND_CAP_TICKS;
        uint64_t lo = (l1 < staged) ? (l1 + staged) / 2 : staged * 3 / 4;
        printf("[*] agent cpu%-2d (%s): calib L1=%lu staged=%lu DRAM=%lu  -> L3 band [%lu,%lu]\n",
               agents[am], is_pcore(agents[am]) ? "P" : "E", l1, staged, dram, lo, hi);
        if (dram > 500) printf("    [!] elevated DRAM calib (%lu) — shared-memory contention; numbers may drift\n", dram);

        long kept_tot = 0, samp_tot = 0;
        for (int ai = 0; ai < NUM_ANCHORS; ai++) {
            volatile uint8_t *line = anchors[ai];
            int kept = 0;
            for (int r = 0; r < REPEATS; r++) {
                self_stage(line, evbuf);
                warm_tlb(line);
                uint64_t t = measure_access_time(line);
                if (t >= lo && t <= hi) sbuf[kept++] = t;
            }
            samp_tot += REPEATS;
            kept_tot += kept;
            Phi[am][ai] = kept ? (double)median_u64(sbuf, kept) : (double)staged;
        }
        kept_ratio[am] = (double)kept_tot / (double)samp_tot;
        printf("    in-band L3 samples kept: %.1f%%\n", 100.0 * kept_ratio[am]);
        if (kept_ratio[am] < 0.25)
            printf("    [!] cpu%d unreliable: kept <25%% — staging never settled (column auto-downweighted)\n",
                   agents[am]);
    }
}

/* Gauss-Jordan matrix inverse with partial pivoting.  `src` is n×n row-major
 * (preserved); the inverse is written to `dst`.  Returns 0 on success, -1 if the
 * matrix is singular.  n ≤ MAX_AGENTS, so the O(n³) cost is negligible.  Used by
 * the LDA classifier to whiten the pooled within-class covariance. */
static int invert_matrix(const double *src, double *dst, int n) {
    static double a[MAX_AGENTS][2 * MAX_AGENTS];
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) a[i][j] = src[i * n + j];
        for (int j = 0; j < n; j++) a[i][n + j] = (i == j) ? 1.0 : 0.0;
    }
    for (int col = 0; col < n; col++) {
        int piv = col;
        double best = fabs(a[col][col]);
        for (int r = col + 1; r < n; r++)
            if (fabs(a[r][col]) > best) { best = fabs(a[r][col]); piv = r; }
        if (best < 1e-12) return -1;
        if (piv != col)
            for (int j = 0; j < 2 * n; j++) { double t = a[col][j]; a[col][j] = a[piv][j]; a[piv][j] = t; }
        double inv = 1.0 / a[col][col];
        for (int j = 0; j < 2 * n; j++) a[col][j] *= inv;
        for (int r = 0; r < n; r++) {
            if (r == col) continue;
            double f = a[r][col];
            if (f == 0.0) continue;
            for (int j = 0; j < 2 * n; j++) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) dst[i * n + j] = a[i][n + j];
    return 0;
}

/* ---- feasibility report ------------------------------------------------- */
static void report(void) {
    double mean[MAX_AGENTS][K_SLICES];
    for (int am = 0; am < num_agents; am++)
        for (int k = 0; k < K_SLICES; k++) {
            double s = 0;
            for (int j = 0; j < ANCHORS_PER_SLICE; j++) s += Phi[am][k * ANCHORS_PER_SLICE + j];
            mean[am][k] = s / ANCHORS_PER_SLICE;
        }

    /* (1) per-(agent,slice) mean-latency table */
    printf("\n=== (1) Per-(agent, slice) mean staged-L3 latency (raw TSC ticks) ===\n");
    printf("slice |");
    for (int am = 0; am < num_agents; am++) printf(" %5d", agents[am]);
    printf("\n type |");
    for (int am = 0; am < num_agents; am++) printf(" %5s", is_pcore(agents[am]) ? "P" : "E");
    printf("\n------+");
    for (int am = 0; am < num_agents; am++) printf("------");
    printf("\n");
    for (int k = 0; k < K_SLICES; k++) {
        printf("  %d   |", k);
        for (int am = 0; am < num_agents; am++) printf(" %5.0f", mean[am][k]);
        printf("\n");
    }

    /* (1b) confound test: per-agent nearest (argmin) / farthest (argmax) slice.
     * Genuine ring distance => agents DISAGREE on which slice is nearest (each
     * core sees its own ring neighbourhood fastest).  A residency/staging
     * artifact => all agents agree on the fast/slow slices. */
    printf("\n(1b) Per-agent nearest/farthest slice (ring-distance confound test):\n");
    int nearest_hist[K_SLICES] = {0};
    for (int am = 0; am < num_agents; am++) {
        int kmin = 0, kmax = 0;
        for (int k = 1; k < K_SLICES; k++) {
            if (mean[am][k] < mean[am][kmin]) kmin = k;
            if (mean[am][k] > mean[am][kmax]) kmax = k;
        }
        nearest_hist[kmin]++;
        printf("  cpu%-2d (%s): nearest=s%d (%.0f)  farthest=s%d (%.0f)  spread=%.0f\n",
               agents[am], is_pcore(agents[am]) ? "P" : "E",
               kmin, mean[am][kmin], kmax, mean[am][kmax], mean[am][kmax] - mean[am][kmin]);
    }
    int distinct_near = 0;
    for (int k = 0; k < K_SLICES; k++) if (nearest_hist[k]) distinct_near++;
    printf("  -> %d distinct 'nearest' slices across %d agents "
           "(>1 ⇒ ring geometry, not a uniform residency artifact)\n", distinct_near, num_agents);

    /* (2a) per-agent one-way ANOVA across the 8 slices */
    printf("\n=== (2) Separability ===\n");
    printf("(2a) Per-agent one-way ANOVA across the 8 slices:\n");
    printf("  cpu  type  zeta(within-sd)   F(%d,%d)   eta^2   signif?\n",
           K_SLICES - 1, NUM_ANCHORS - K_SLICES);
    int nsig = 0;
    for (int am = 0; am < num_agents; am++) {
        double grand = 0;
        for (int ai = 0; ai < NUM_ANCHORS; ai++) grand += Phi[am][ai];
        grand /= NUM_ANCHORS;

        double ssb = 0, ssw = 0;
        for (int k = 0; k < K_SLICES; k++) {
            ssb += ANCHORS_PER_SLICE * (mean[am][k] - grand) * (mean[am][k] - grand);
            for (int j = 0; j < ANCHORS_PER_SLICE; j++) {
                double d = Phi[am][k * ANCHORS_PER_SLICE + j] - mean[am][k];
                ssw += d * d;
            }
        }
        double dfb = K_SLICES - 1, dfw = NUM_ANCHORS - K_SLICES;
        double F    = (ssb / dfb) / (ssw / dfw);
        double eta2 = ssb / (ssb + ssw);
        double zeta = sqrt(ssw / dfw);
        int    sig  = F > 2.05;                 /* ~F_crit(0.05; 7, 248) */
        if (sig) nsig++;
        printf("  cpu%-2d  %s   %12.1f   %7.2f   %.3f   %s\n",
               agents[am], is_pcore(agents[am]) ? "P" : "E", zeta, F, eta2, sig ? "YES" : "no");
    }
    printf("  (F>~2.05 ⇒ p<0.05;  F>~2.7 ⇒ p<0.01.  %d/%d agents show a significant per-slice gradient.)\n",
           nsig, num_agents);

    /* Active-agent selection (the third fusion lever).  An agent whose staging
     * never settled leaves a near-constant column (≈0 within-class variance) that
     * is uninformative AND actively harmful: 1/ζ² *rewards* it, and it makes the
     * LDA covariance near-singular.  Drop kept_ratio < MIN_KEPT_CLASSIFY from the
     * separability metric (2b) and the classifier (3); such agents still appear in
     * the (1)/(1b)/(2a) diagnostics above. */
    int act[MAX_AGENTS], nact = 0;
    for (int am = 0; am < num_agents; am++)
        if (kept_ratio[am] >= MIN_KEPT_CLASSIFY) act[nact++] = am;
    if (nact < 2) {                 /* pathological: nearly everything failed → keep all */
        printf("\n[!] <2 agents passed the kept-ratio filter; falling back to all %d agents.\n", num_agents);
        for (nact = 0; nact < num_agents; nact++) act[nact] = nact;
    } else if (nact < num_agents) {
        printf("\n[*] agent-selection: %d/%d agents used (dropped", nact, num_agents);
        for (int am = 0; am < num_agents; am++)
            if (kept_ratio[am] < MIN_KEPT_CLASSIFY)
                printf(" cpu%d@%.0f%%", agents[am], 100.0 * kept_ratio[am]);
        printf(", kept<%.0f%%)\n", 100.0 * MIN_KEPT_CLASSIFY);
    }

    /* (2b) joint z-space centroid separability: Δμ vs noise radius (active agents) */
    static double z[MAX_AGENTS][NUM_ANCHORS];
    for (int am = 0; am < num_agents; am++) {
        double m = 0;
        for (int ai = 0; ai < NUM_ANCHORS; ai++) m += Phi[am][ai];
        m /= NUM_ANCHORS;
        double v = 0;
        for (int ai = 0; ai < NUM_ANCHORS; ai++) { double d = Phi[am][ai] - m; v += d * d; }
        double sd = sqrt(v / (NUM_ANCHORS - 1));
        if (sd < 1e-9) sd = 1e-9;
        for (int ai = 0; ai < NUM_ANCHORS; ai++) z[am][ai] = (Phi[am][ai] - m) / sd;
    }
    double cen[K_SLICES][MAX_AGENTS];
    for (int k = 0; k < K_SLICES; k++)
        for (int ia = 0; ia < nact; ia++) {
            int am = act[ia];
            double s = 0;
            for (int j = 0; j < ANCHORS_PER_SLICE; j++) s += z[am][k * ANCHORS_PER_SLICE + j];
            cen[k][am] = s / ANCHORS_PER_SLICE;
        }
    /* noise radius: RMS standard error of one centroid coordinate, summed over dims */
    double radius2 = 0;
    for (int ia = 0; ia < nact; ia++) {
        int am = act[ia];
        double ssw = 0;
        for (int k = 0; k < K_SLICES; k++)
            for (int j = 0; j < ANCHORS_PER_SLICE; j++) {
                double d = z[am][k * ANCHORS_PER_SLICE + j] - cen[k][am];
                ssw += d * d;
            }
        double within_var = ssw / (NUM_ANCHORS - K_SLICES);
        radius2 += within_var / ANCHORS_PER_SLICE;
    }
    double radius_pair = sqrt(2.0 * radius2);   /* noise on a centroid *difference* */
    double dmin = 1e18; int ka = -1, kb = -1;
    for (int k = 0; k < K_SLICES; k++)
        for (int k2 = k + 1; k2 < K_SLICES; k2++) {
            double d2 = 0;
            for (int ia = 0; ia < nact; ia++) {
                int am = act[ia];
                double d = cen[k][am] - cen[k2][am];
                d2 += d * d;
            }
            double dd = sqrt(d2);
            if (dd < dmin) { dmin = dd; ka = k; kb = k2; }
        }
    double ratio = dmin / radius_pair;
    printf("\n(2b) Joint z-space centroid separability (%d active agents):\n", nact);
    printf("  Min pairwise centroid distance  Δμ   = %.3f   (closest pair: slices %d,%d)\n", dmin, ka, kb);
    printf("  Noise radius on a difference    ρ    = %.3f\n", radius_pair);
    printf("  Separation ratio                Δμ/ρ = %.2f   -> %s\n", ratio,
           ratio > 3.0 ? "SEPARABLE (signal present)" :
           ratio > 1.5 ? "MARGINAL"                    :
                         "BURIED (Δμ below noise — static gradient not resolvable here)");

    /* (3) Supervised slice classifier — three fusion metrics, 2-fold CV.
     *
     * The equal-weight z-space nearest-centroid (the prior baseline) treats every
     * agent's standardized dimension alike, so the few-tick margin separating an
     * adjacent ring-neighbour pair (s2/s3, s6/s7) on the 2–3 agents that *see* it is
     * swamped by sub-tick noise summed over the agents that don't.  Two fixes, each
     * exploiting the *within-class* structure the equal-weight metric discards:
     *   - inverse-variance: weight agent m by 1/ζ²_m (down-weights noisy agents);
     *   - shrinkage-LDA   : Mahalanobis distance under the pooled within-class
     *                       covariance, which whitens *correlated* agent noise so the
     *                       optimal linear boundary upweights exactly the agents that
     *                       separate a given pair.  LDA ≫ inv-var ⇒ noise is correlated.
     * 2-fold CV (train half / test half, then swap) doubles the held-out set and
     * removes the fixed-split bias of the prior single-half evaluation. */
    const double shrink = LDA_SHRINKAGE;
    int half = ANCHORS_PER_SLICE / 2;
    int n_train = K_SLICES * half;
    enum { M_EQ = 0, M_IV = 1, M_LDA = 2, NUM_METHODS = 3 };
    const char *mname[NUM_METHODS] = { "equal-weight (z)", "inverse-variance", "shrinkage-LDA" };
    int correct[NUM_METHODS] = {0}, total = 0;
    int conf[K_SLICES][K_SLICES];           /* confusion of the LDA predictor */
    memset(conf, 0, sizeof(conf));

    static double C[MAX_AGENTS * MAX_AGENTS], Cinv[MAX_AGENTS * MAX_AGENTS];
    int lda_singular = 0;

    for (int fold = 0; fold < 2; fold++) {
        int tr_lo = fold ? half : 0, tr_hi = fold ? ANCHORS_PER_SLICE : half;
        int te_lo = fold ? 0 : half, te_hi = fold ? half : ANCHORS_PER_SLICE;

        /* per-class centroids from the train half: raw Phi (for inv-var/LDA) and
         * z-scored (to reproduce the equal-weight baseline exactly) */
        double mu[K_SLICES][MAX_AGENTS], muz[K_SLICES][MAX_AGENTS];
        for (int k = 0; k < K_SLICES; k++)
            for (int ia = 0; ia < nact; ia++) {
                int am = act[ia];
                double s = 0, sz = 0;
                for (int j = tr_lo; j < tr_hi; j++) {
                    s  += Phi[am][k * ANCHORS_PER_SLICE + j];
                    sz +=   z[am][k * ANCHORS_PER_SLICE + j];
                }
                mu[k][am]  = s  / half;
                muz[k][am] = sz / half;
            }

        /* pooled within-class covariance over the train half (active agents,
         * compact nact×nact so the inverse never sees a dropped agent's column) */
        for (int ia = 0; ia < nact; ia++)
            for (int ib = 0; ib < nact; ib++) {
                int am = act[ia], bm = act[ib];
                double s = 0;
                for (int k = 0; k < K_SLICES; k++)
                    for (int j = tr_lo; j < tr_hi; j++) {
                        int ai = k * ANCHORS_PER_SLICE + j;
                        s += (Phi[am][ai] - mu[k][am]) * (Phi[bm][ai] - mu[k][bm]);
                    }
                C[ia * nact + ib] = s / (n_train - K_SLICES);
            }
        /* shrink toward a scaled identity: C ← (1-γ)C + γ·(trace/A)·I */
        double trace = 0;
        for (int ia = 0; ia < nact; ia++) trace += C[ia * nact + ia];
        double ridge = shrink * trace / nact;
        for (int ia = 0; ia < nact; ia++)
            for (int ib = 0; ib < nact; ib++)
                C[ia * nact + ib] = (1.0 - shrink) * C[ia * nact + ib]
                                    + (ia == ib ? ridge : 0.0);
        double ivar[MAX_AGENTS];
        for (int ia = 0; ia < nact; ia++) ivar[ia] = 1.0 / C[ia * nact + ia];
        if (invert_matrix(C, Cinv, nact) != 0) lda_singular = 1;

        for (int k = 0; k < K_SLICES; k++)
            for (int j = te_lo; j < te_hi; j++) {
                int ai = k * ANCHORS_PER_SLICE + j;
                int best[NUM_METHODS]; double bd[NUM_METHODS];
                for (int m = 0; m < NUM_METHODS; m++) { best[m] = 0; bd[m] = 1e300; }
                for (int kk = 0; kk < K_SLICES; kk++) {
                    double deq = 0, div = 0, dl = 0;
                    for (int ia = 0; ia < nact; ia++) {
                        int am = act[ia];
                        double dz = z[am][ai] - muz[kk][am];
                        deq += dz * dz;
                        double dr = Phi[am][ai] - mu[kk][am];
                        div += dr * dr * ivar[ia];
                    }
                    if (!lda_singular)
                        for (int ia = 0; ia < nact; ia++) {
                            int am = act[ia];
                            double da = Phi[am][ai] - mu[kk][am], acc = 0;
                            for (int ib = 0; ib < nact; ib++)
                                acc += Cinv[ia * nact + ib] * (Phi[act[ib]][ai] - mu[kk][act[ib]]);
                            dl += da * acc;
                        }
                    else dl = div;
                    if (deq < bd[M_EQ])  { bd[M_EQ]  = deq; best[M_EQ]  = kk; }
                    if (div < bd[M_IV])  { bd[M_IV]  = div; best[M_IV]  = kk; }
                    if (dl  < bd[M_LDA]) { bd[M_LDA] = dl;  best[M_LDA] = kk; }
                }
                for (int m = 0; m < NUM_METHODS; m++) if (best[m] == k) correct[m]++;
                conf[k][best[M_LDA]]++;
                total++;
            }
    }

    printf("\n=== (3) Supervised slice classifier — fusion comparison (2-fold CV, %d agents) ===\n", nact);
    printf("  method               accuracy            (chance = %.1f%%)\n", 100.0 / K_SLICES);
    for (int m = 0; m < NUM_METHODS; m++)
        printf("  %-18s   %4d/%-4d = %5.1f%%%s\n", mname[m], correct[m], total,
               100.0 * correct[m] / total,
               (m == M_LDA && lda_singular) ? "   [singular -> fell back to inv-var]" : "");
    printf("  LDA confusion (rows=true slice, cols=predicted):\n      ");
    for (int p = 0; p < K_SLICES; p++) printf(" p%d ", p);
    printf("\n");
    for (int k = 0; k < K_SLICES; k++) {
        printf("   t%d |", k);
        for (int p = 0; p < K_SLICES; p++) printf(" %3d", conf[k][p]);
        printf("\n");
    }

    int best_m = 0;
    for (int m = 1; m < NUM_METHODS; m++) if (correct[m] > correct[best_m]) best_m = m;
    printf("\n=== VERDICT ===\n");
    printf("  Feasibility (signal exists) is (2): %d/%d agents significant, Δμ/ρ=%.2f (%s).\n",
           nsig, num_agents, ratio,
           ratio > 3.0 ? "separable" : ratio > 1.5 ? "marginal" : "buried");
    printf("  Best fusion: %s at %.1f%%  (vs %.1f%% equal-weight, %.1f%% chance).\n",
           mname[best_m], 100.0 * correct[best_m] / total,
           100.0 * correct[M_EQ] / total, 100.0 / K_SLICES);
}

/* ---- main --------------------------------------------------------------- */
static void parse_agents(const char *csv) {
    num_agents = 0;
    char *s = strdup(csv);
    for (char *tok = strtok(s, ","); tok && num_agents < MAX_AGENTS; tok = strtok(NULL, ","))
        agents[num_agents++] = atoi(tok);
    free(s);
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "[-] Must run as REAL root (sudo): virt_to_phys returns 0 otherwise,\n"
                        "    which silently corrupts the ground-truth slice labels.\n");
        return 1;
    }

    if (argc >= 2) parse_agents(argv[1]);
    else { memcpy(agents, DEFAULT_AGENTS, sizeof(DEFAULT_AGENTS)); num_agents = NUM_DEFAULT_AGENTS; }

    printf("=== Static interconnect-latency slice oracle — feasibility test ===\n");
    printf("[*] Config: pages=%d  anchors/slice=%d  repeats=%d  L2evict=%dx%d  agents=%d\n",
           NUM_HUGE_PAGES, ANCHORS_PER_SLICE, REPEATS, L2EVICT_WAYS, L2EVICT_PASSES, num_agents);
    printf("[*] Agents:");
    for (int i = 0; i < num_agents; i++) printf(" cpu%d(%s)", agents[i], is_pcore(agents[i]) ? "P" : "E");
    printf("\n[*] Reminder: run on performance governor with P-core SMT siblings idled.\n");

    set_realtime_latency();
    ensure_huge_pages_available(NUM_HUGE_PAGES + EVBUF_PAGES);
    uint8_t *base  = (uint8_t *)allocate_huge_pages(NUM_HUGE_PAGES);
    uint8_t *evbuf = (uint8_t *)allocate_huge_pages(EVBUF_PAGES);   /* set-congruent L2 eviction */

    build_anchors(base);
    measure_all(evbuf);
    report();
    return 0;
}
