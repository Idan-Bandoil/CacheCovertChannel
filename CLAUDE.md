# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

The reference implementation for the paper *"Eviction Set Discovery via Page Alignment"* (Bandoil & Gilboa, Ben-Gurion University). The latest manuscript lives at `docs/Eviction_Set_Discovery_Research_Documentation.pdf`. Read it for the full algorithm; this file covers what is needed to work on the code.

The paper's contribution is the **Bootstrap+Map** algorithm: instead of running pruning O(S) times to discover eviction sets for every LLC set (the O(S·W·log S) state-of-the-art of Prune+PlumTree — Kessous & Gilboa, S&P 2024, sitting next to the manuscript at `docs/PrunePlumTree_-_Finding_Eviction_Sets_at_Scale.pdf`), it runs pruning a small number of times to recover per-page slice-offset deltas, then performs an O(1) **algebraic bucketing** step per cell using only bitwise math. The reference implementation targets Intel Core i7-12700H (Alder Lake, 8 LLC slices, 12-way, 4096 sets/slice, 30 MB LLC).

## The `docs/` folder (this is the design-doc surface)

The user is currently working on the algorithm at the pseudocode level. The docs folder is the primary working surface; the C code is *not* yet aligned with the latest manuscript (see §"Code vs. paper gap" below). Order of importance:

1. **`Eviction_Set_Discovery_Research_Documentation.pdf`** — the latest version of the paper. Major restructuring vs. the older draft:
   - The algorithm is now **parametric in `α := log₂(page_size)`** rather than hardcoded to 2 MB huge pages. The huge-page case is the α = 21 instance; 4 KB is α = 12.
   - **New notation**: per-page tag `δ_P := (I_U^P, Δ^h_P)` of width `max(0, β − α) + s` bits, where `β := 6 + log₂(S) = 18` (first bit above the set-index region) and `s := log₂(K_max) = 3`. At α = 21 this collapses to a 3-bit `Δ^h_P` (what the old draft called `Δ_P`); at α = 12 it widens to 9 bits.
   - **`v_var := max(0, α − β)`** formalises the in-page slice-variation bits (the "bits 18, 19, 20 within a huge page" trick).
   - **Algorithm 2 is now "Global Cache Mapping"** — one linear sweep that fills the entire `ES[I][S]` 2D table, *not* the older "query (T, S) and build one eviction set" form. The change matters for the 4 KB adaptation (see `4KB_THEORY_GLOBAL_MAPPING.md`).
   - Equation labels have shifted: `S(L) = H_K(L) ⊕ Δ^h_P` is **Eq. 1**; the per-page tag definition is **Eq. 3**; the algebraic bridge `Δ^h_{P_i} ⊕ Δ^h_{P_j} = H_K(L_i) ⊕ H_K(L_j)` is **Eq. 5**. The recovered slice hash for i7-12700H is **Eq. 6**. The parity-blindness identity `S₂ ⊕ S₁ ⊕ S₀ = 0` (within one huge page) is **Eq. 7**.
   - The new manuscript explicitly cites Prune+PlumTree as ref [13] — the comparison the user wants to draw is against this paper.

2. **`PrunePlumTree_-_Finding_Eviction_Sets_at_Scale.pdf`** — Kessous & Gilboa, S&P 2024. The O(S·W·log S) state-of-the-art that Bootstrap+Map beats. Kept in `docs/` for direct comparison while the user works on pseudocode.

3. **`PPT_VS_BOOTSTRAP_MAP.md`** — side-by-side comparison of Prune+PlumTree (ref [13]) and Bootstrap+Map: operating principle, asymptotic runtime, assumptions, artifact. Headlines: B+M beats PPT (~700× fewer timed accesses on i7-12700H) **only in the `v_var > 0` huge-page regime** via its algebra; at `v_var = 0` the naive form is `Θ(W·(S·K)²)` — asymptotically *worse* than PPT's `Θ(S·W·log S)` — and needs bisection to reach PPT-parity. PPT recovers no slice mapping; B+M does.

4. **`4KB_THEORY_GLOBAL_MAPPING.md`** — theoretical adaptation of Bootstrap+Map to 4 KB pages, **aligned with the new global-mapping form of Algorithm 2**. This is the current canonical 4 KB writeup. Key results:
   - At α = 12, `v_var = 0`, so the **algebraic bridge collapses** (every line in a single PRUNE pool shares the same offset → same `H_K` → Eq. 5 gives `0 = 0`).
   - Algorithm 1 degrades from "per-page tag vector" to **equivalence partition over up to 2^9 = 512 buckets**.
   - Algorithm 2's structure (one sweep, cap-at-W per cell) lifts cleanly; only the cell coordinate function changes from `(I, K) = (set_index, H_K ⊕ Δ^h_P)` to `(within-bucket offset, bucket id)`.
   - Cross-bucket Δ-relations are unrecoverable from timing alone — fundamental theoretical limit at α < β. An attack-time bridging step (≤ 512 prime+probe tests) recovers absolute `(I, K)` per target VA.

5. **`L2_HYBRID.md`** — develops §7's "L2-assisted bridging" open question into a full hybrid. The private, **non-sliced** L2 is used as a cheap bisection oracle on the *set-index* bits of the page tag, cutting the naive `v_var = 0` cost from `Θ(W·(S·K)²)` to `Θ(W·S·K·log(S·K))` = **PPT-parity**. Architecture-parametric (no hardcoded constants). Limits: L2 never resolves slices (residual ≥ K); it *ties* PPT, not beats it (gains are constant-factor + a queryable artifact + keeping B+M one algorithm); and it cannot restore the huge-page `S`-independence (that needs `v_var > 0` algebra).

6. **`4KB_THEORY.md`** — older 4 KB theory writeup, against the **per-target** form of Algorithm 2. **Superseded** by `4KB_THEORY_GLOBAL_MAPPING.md` for anything touching Algorithm 2's shape; still useful for the Algorithm-1 reasoning, which is identical in both forms.

7. **`4KB_PAGE_ADAPTATION.md`** — implementation-flavoured (rather than pure-theory) 4 KB adaptation. Concrete `H_known` rewrite, candidate-pool changes, per-page state layout, cost table (huge vs. 4 KB across pages, PRUNE runs, memory, etc.). Use this when translating the theory docs into code.

8. **`NOISE_REDUCTION_NOTES.md`** — running log of experiments aimed at improving Phase-3 verification rate on `mapping_attack`. Key takeaways the user has already accepted/rejected:
   - **Experiment 1 (kept):** hold `/dev/cpu_dma_latency` fd open for process lifetime via `static int` in `set_realtime_latency()`. Doubles Phase-3 success (40% → ~80%).
   - **Experiment 2 (reverted):** raising `test_group_robust` from 5/3 to 11/7 *regressed* both Phase-3 (10%) and alignment (22/30 short of 100). Pruning's "is this still an eviction set?" loop interprets a too-conservative threshold as "essential candidate, keep" → set never shrinks → no bridges. Direction of the threshold matters.
   - **Experiment 3 (paused):** a `(tests, threshold)` sweep; partial data shows 5/2 and 3/2 are both worse than the 5/3 default. Sweep paused because `WARM_TLB`, fence pattern, governor, and SMT sibling are unresolved confounds.
   - **Environmental fixes (in progress):** E-core pinning gave the best Phase-3 (28/30) but broke alignment (4/30) due to threshold being P-core-tuned. SMT-sibling offline + P-core kept (25/30 verified, 30/30 aligned). `performance` governor on top regressed slightly — needs a second sample.
   - When the user wants to "tune noise", these are the unresolved tasks and their interaction effects. Don't re-derive any of this from scratch.

## Build & run

```bash
make                                       # builds eviction_builder, negative_control, mapping_attack
make clean
sudo ./eviction_builder <core_id>
sudo ./negative_control <core_id>
sudo ./mapping_attack  <core_id>
```

All binaries require root: they `mmap` 2 MB huge pages (`MAP_HUGETLB`), read `/proc/self/pagemap` for V→P translation, and write `/dev/cpu_dma_latency` to disable C-states. CFLAGS pin `-march=native -O2 -std=c11`.

**Huge-page pool pre-flight (in-flight change, uncommitted).** All three `main`s now call `ensure_huge_pages_available(N)` before `allocate_huge_pages` — it reads `HugePages_Free` from `/proc/meminfo` and *additively* bumps `/proc/sys/vm/nr_hugepages` if short. The earlier in-allocator fallback (overwrite the pool count from inside `allocate_huge_pages` if `MAP_HUGETLB` fails) was racy across in-process allocations and has been replaced by this proactive bump. Still needs sudoers grant for `tee /proc/sys/vm/nr_hugepages` because the bump shells out.

**Compile-time pruning knobs.** `mapping_attack.c` gates `ROBUST_TESTS` / `ROBUST_THRESHOLD` behind `#ifndef` so `scripts/threshold_sweep.sh` can recompile the binary across a (tests, threshold) grid without source edits. Default behaviour is unchanged (5 trials, ≥3 majority).

**There is no test suite.** Each binary's correctness signal is the timing histogram it prints; `mapping_attack`'s additional signal is whether the mathematically-built eviction set produces the expected `[+] VERIFIED` line.

**Run `mapping_attack` via `scripts/quiet_run.sh` (not bare).** Its accuracy is gated by shared-L3 contention (the 30 MB LLC is shared by every core). Bare on `powersave` with a busy desktop it collapses; through `quiet_run.sh ./mapping_attack <core>` (performance governor + idled SMT sibling + a contention warning) with browsers/IDEs closed it is far more reliable. `mapping_attack` self-validates each bootstrap by parity-coset balance and **re-rolls up to `MAX_BOOTSTRAP_ATTEMPTS` times** (a badly skewed split is the contention signature), so transient noise costs a retry, not a wrong map.

**`-DGROUND_TRUTH` (measurement only).** `mapping_attack.c` gates `virt_to_phys`-based accuracy probes behind `#ifdef GROUND_TRUTH`. The default build is a clean attack that never reads pagemap; `make CFLAGS="... -DGROUND_TRUTH"` adds a frame-consistency report and a per-slice "genuine lines" count — the *reliable* map-quality signal, since the end-of-run timing `VERIFIED` count is itself noise-hit and badly understates a correct map.

## `scripts/` helpers

- **`scripts/run_with_env.sh <binary> <core_id> [args...]`** — wraps a run by setting `cpufreq/scaling_governor` to `performance` for the target core and restoring the original on `EXIT/INT/TERM`. Requires sudoers grant for `tee` on `cpu*/cpufreq/scaling_governor`.
- **`scripts/quiet_run.sh <binary> <core_id> [args...]`** — the preferred wrapper for `mapping_attack`. Pins `performance`, **idles the target core's SMT sibling(s)**, warns if heavy shared-L3 apps (firefox/chrome/code/gitkraken/…) are running, and restores governor + sibling on exit. Needs sudoers `tee` on the governor and `cpu*/online`. Cannot isolate the L3 (shared by all cores) — close browsers/IDEs for reliable results; a headless / `isolcpus` box is ideal.
- **`scripts/threshold_sweep.sh [runs] [core]`** — drives Experiment 3. Compiles `mapping_attack` once per `(ROBUST_TESTS, ROBUST_THRESHOLD)` cell, runs N trials, prints a TSV summary with verified-rate, aligned-100-rate, and mean runtime.

## The three binaries (progressive experiments)

All three share `src/utils.{c,h}` (slice hash, timing, huge-page alloc, CPU pinning).

1. **`eviction_builder` (`src/main.c`) — ground truth.**
   Uses `virt_to_phys` plus the recovered slice hash to *cheat*: pick lines that mathematically belong to `(TARGET_SET, TARGET_SLICE)`, then verify via timing that they actually evict each other. Establishes the hit/miss baselines that everything else relies on.

2. **`negative_control` (`src/negative_control.c`) — sanity check.**
   Same setup, but the "eviction set" is built from `DUMMY_SET = 0x5B` (a different set from the victim's `0x5A`). Confirms the timing signal is set-specific: the victim should *survive* the thrash. If false-miss rate is high here, the timing apparatus is broken.

3. **`mapping_attack` (`src/mapping_attack.c`) — the actual paper.**
   Recovers slice mapping **without** `virt_to_phys`, using only timing + algebraic bridging.

## How `mapping_attack` maps to the paper

The code is a near-direct transcription of Algorithms 1 and 2 in the **older** draft (the per-target form of Algorithm 2). Notation in the table below is updated to the new manuscript; equation numbers reference the new paper.

| Paper concept | Code |
|---|---|
| `H_K(L)` — slice hash on known bits (Eq. 1) | `get_known_hash` — masks VA to bits 0–20 and runs the slice hash |
| `Δ^h_P` — per-page slice tag (Eq. 1, 3) | `delta[p]` array, one entry per huge page |
| Anchor `Δ^h_{P_0} := 0` | each coset's seed page is anchored at its `base` delta (coset 0 → 0, coset 1 → 1) in `seed_new_coset` |
| **PRUNE(C, v)** (paper §III.A and Algorithm 1) | `find_eviction_set` — linear pruning, stops once the set is **slice-specific** (`≤ SLICE_SPECIFIC_MAX = 2W−1` lines), `test_group_robust` 5/3 majority |
| **Algorithm 1 — Bootstrap loop** | `bootstrap_page_alignment` — seeds each parity coset once (`seed_new_coset`), then `remap_round` (EM-style membership re-classification) refines until `M = P` |
| **Algebraic bridge** (Eq. 5: `Δ^h_{P_i} ⊕ Δ^h_{P_j} = H_K(L_i) ⊕ H_K(L_j)`) | applied in `seed_new_coset` (per-page membership against the seed's slice-specific set) and `build_relslice_set` (cell fill / membership oracle). The old single-frame `calculate_page_deltas` bridge was replaced by noise-robust **membership testing** (`victim_in_relslice`, `evicts_strict`) because cross-coset bridging is impossible from L3 timing |
| **In-page slice-variation expansion** (the `v_var = 3` knob at α = 21: bits 18, 19, 20) | `create_candidate_pool` enumerates all 8 variations per page |
| **Parity blindness** (Eq. 7, paper §III.D) | `seed_new_coset` anchors **each** of the 2 parity cosets in its own frame; a fresh coset's seed victim must be unclaimed by the already-anchored slices (`in_any_anchored_slice`) |

### Code vs. paper gap (Algorithm 2)

The current C code (`build_target_eviction_set` in `mapping_attack.c`) is the **per-target** form of Algorithm 2: given `(target_set, target_slice)`, iterate over aligned pages and pick the line whose computed slice matches. The latest manuscript instead specifies Algorithm 2 as a **single linear sweep over all aligned lines, filling the entire `ES[I][S]` table in one pass**. The aggregate work is identical, but the artefact's shape differs — the implementation hasn't been refactored to emit the global table yet. If you touch Algorithm 2 code, decide first whether you're realigning to the new global-mapping form (see `docs/4KB_THEORY_GLOBAL_MAPPING.md` §1.2 for the cleanest pseudocode).

### Why "known hash" works (subtle, easy to break)

The whole technique rests on two facts:
- 2 MB huge pages → VA bits 0–20 == PA bits 0–20.
- The slice hash is a pure XOR (linear), so unknown upper bits contribute a *constant* per-page offset that XOR-cancels when comparing two lines on the same page.

If either premise is violated — non-huge pages, or a non-linear hash (some hybrid Intel SKUs have these per the paper's "Related Work" / ref [13]) — `Δ^h_P` is no longer a constant and the algorithm collapses. Do not change `get_known_hash` to take more bits without also re-deriving the math.

### Parity-blindness gotcha (Eq. 7)

A single huge page only varies bits 18, 19, 20 of the address. Restricting Eq. 6 to those bits:

    S₀ contribution: p18 ⊕ p20
    S₁ contribution: p19
    S₂ contribution: p18 ⊕ p19 ⊕ p20

XOR-summing the three gives `S₂ ⊕ S₁ ⊕ S₀ = 0`. So lines within one huge page can only land in 4 of 8 slices (one parity coset). `bootstrap_page_alignment` therefore anchors **each** parity coset with its own PRUNE seed (`seed_new_coset`, base delta 0 vs 1, kept in disjoint slice-parity classes) and maps the rest by membership. The two cosets live in **separate coordinate frames** — their relative offset is unrecoverable from L3 timing alone (no cross-coset conflict exists; this is the same limit as the 4 KB `v_var = 0` case, `docs/4KB_THEORY_GLOBAL_MAPPING.md` §4.5), which is fine for building per-`(set,slice)` eviction sets but means slice *labels* are consistent only within a coset.

Reliability split (important): **parity coverage is reliable** — both cosets are anchored on essentially every run on a quiet core. But **per-page slice accuracy is gated by the eviction-test false-positive rate**, so the Phase-3 verified-slice count swings run-to-run (8/8 on a good run, 2–4/8 on a noisy one). Two environmental knobs dominate and are *not* optional for meaningful results: run on the **`performance` governor** (use `scripts/run_with_env.sh`; on `powersave` the frequency scales mid-run and the cycle threshold drifts, collapsing the map) and keep the core quiet (idle SMT sibling). This is the same noise battle tracked in `NOISE_REDUCTION_NOTES.md` — the parity-iteration fix is orthogonal to it. Reducing `NUM_PAGES` still silently starves coverage.

### L2 wash (mapping_attack only)

`mapping_attack` uses an explicit `l2_wash_pool` (~2.5 MB of set-dodging lines) to push candidate lines from L2 down to L3 between accesses. On Alder Lake, L2 is non-inclusive of L3, so a simple thrash-the-set loop can leave the victim alive in L2 even after eviction from L3. The wash pool deliberately *skips* the bootstrap set so it doesn't perturb the measurement. `eviction_builder` and `negative_control` use a simpler 3-sweep forward/backward thrash — that's adequate when the lines are already known-correct.

## Hardware coupling

`src/utils.h` and `src/utils.c` hard-code the i7-12700H:
- `LLC_WAYS = 12`, `LLC_SETS_PER_SLICE = 4096`, `SET_INDEX_MASK = 0xFFF` (12 bits, paper §II.A and §III.A).
- `get_cache_slice` is the **recovered linear hash for 8 slices on this CPU** (paper Eq. 6). The XOR bit-positions for `bit0/bit1/bit2` are not portable. Porting to another CPU = re-derive this function.

`measure_access_time` uses `mfence; lfence; rdtsc; lfence; <load>; lfence; rdtscp; mfence` — the fence pattern matters; reordering changes the cycle counts. `WARM_TLB` XORs the address with `0x800` to touch a sibling 4 KB page entry — but at α = 21 that's still inside the same huge page, so it doesn't actually warm a fresh TLB entry. `NOISE_REDUCTION_NOTES.md` flags this as Step D of the environmental cleanup plan.

## Conventions

- Addresses are `uint8_t*` (byte-granular pointer arithmetic for offset math).
- `shuffle_addresses_randomly` uses a fixed seed (`srand(1337)`) so runs are reproducible.
- Hash function uses a `GET_BIT(addr, n)` macro defined in `utils.c`.
- Huge-page allocation failure is fatal (`exit(EXIT_FAILURE)`); pagemap reads returning 0 are silently skipped during the scan.
- When citing the paper in commit messages or comments, the algorithms are: Algo 1 = "Bootstrapping and Page Alignment", Algo 2 = "Global Cache Mapping".
- When citing equations, use the new manuscript's numbering: Eq. 1 (decomposition), Eq. 3 (per-page tag δ_P), Eq. 5 (algebraic bridge), Eq. 6 (slice hash), Eq. 7 (parity-blindness identity).
