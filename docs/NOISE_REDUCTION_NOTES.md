# Noise-Reduction Experiments — Bootstrap+Map (`mapping_attack`)

Running log of changes we've attempted to reduce timing noise / improve Phase-3
verification rate, with measured results and conclusions. Updated as we go.

## Methodology

- Hardware: i7-12700H, pinned to core 4 (P-core).
- Metric: out of N runs of `sudo ./mapping_attack 4`, count how many print
  `[+] VERIFIED: The mathematical eviction set triggers L3 misses!` vs
  `[-] The set was mathematically generated, but hardware absorbed the thrash.`
- Secondary metric: alignment count (`Successfully aligned X/100 pages.`).
  Phase-1/2 correctness; should always be 100/100.
- Each change is gated on **algorithm correctness preserved** before keeping.

## Baseline (unmodified `mapping_attack.c`)

10-run sample: **VERIFIED 4/10 (40%)**, alignment 100/100 every run.

Phase 1/2 (bootstrap) is reliable; Phase 3 (verification of a mathematically
generated set against an unfamiliar `(target_set=0x008, target_slice=2)`) is the
flaky part.

---

## Experiment 1 — Hold the `/dev/cpu_dma_latency` fd open for process lifetime

**Motivation.** The kernel's PM_QoS C-state constraint is honored only while
the file descriptor on `/dev/cpu_dma_latency` is open. The original
`set_realtime_latency()` opened the fd in a local variable; the fd was closed
when the function returned, so the constraint was effectively dropped before
any measurement happened. C-state transitions add tens to hundreds of cycles of
jitter to RDTSC-bracketed measurements.

**Change.** `set_realtime_latency()` in `src/utils.c` — fd stored in a
`static int`, set on first call, never closed.

**Result.**
- 10 runs: VERIFIED 6/10 (60%) — up from 4/10
- 30 runs: VERIFIED **24/30 (80%)** — up from baseline 40%
- Alignment 100/100 in every run.

**Conclusion.** Kept. Roughly doubles the Phase-3 success rate. The constraint
is released automatically when the process exits, so there is no resource leak
in practice.

---

## Experiment 2 — Raise `test_group_robust` from 5/3 to 11/7  (REVERTED)

**Motivation.** Conventional advice for cache-side-channel pruning literature is
that an 11-trial supermajority (≥7 misses) is dramatically more robust than a
5-trial simple majority (≥3 misses) at the same per-trial flake rate. The naive
binomial calculation suggests the false-decision rate drops by ~10×.

**Change.** In `test_group_robust` (`src/mapping_attack.c`) — `tests = 11`,
`return misses >= 7`.

**Result (regression).**
- 30 runs: VERIFIED **3/30 (10%)** — much worse than baseline.
- Alignment fell short of 100/100 on **22/30** runs, i.e. the bootstrap stopped
  early without recovering all 100 page deltas.

**Conclusion.** Reverted. Naive intuition was wrong for this codebase, and the
failure mode is informative.

### Why the threshold raise broke pruning here

`find_eviction_set` doesn't ask "is this an eviction set?" once. It asks, in a
loop, "does removing this candidate *still* evict?" If yes → discard;
if no → put back, mark essential. That has the following structure:

- **Per-trial flakiness is asymmetric.** A single trial's `test_group` can
  return a "false hit" (victim stayed in cache despite the thrash, e.g. because
  the L2 wash didn't fully push it down). False misses are much rarer than
  false hits in a non-inclusive-L3 setup.
- The 5-trial majority `≥3 misses` already absorbs most false hits. Raising the
  bar to `≥7 misses` makes the test reject the candidate group in marginal
  cases — i.e. it more readily classifies a *real* eviction-capable group as
  "didn't evict".
- In the pruning inner loop, "didn't evict" → "this candidate is essential,
  keep it". So the working set fails to shrink to the minimal `LLC_WAYS` lines,
  pruning exits late or never reaches `TARGET_EVICTION_COUNT`. Worse, in the
  *outer* bootstrap loop, the very first `test_group_robust` call (the
  baseline-pool sanity check at the top of `find_eviction_set`) starts
  returning false. When that happens, `find_eviction_set` returns `false` and
  the outer bootstrap moves to the next victim *without ever pruning a set*.
  No pruned set → no bridge → no new deltas. That's why alignment fell short
  of 100/100 on 22/30 runs.

The *direction* of the threshold matters. Pruning is essentially asking a
yes/no question where "yes" is the conservative answer (keep the candidate),
so raising the threshold for "yes the set still evicts" pushes the algorithm
toward the conservative branch and *enlarges* the set rather than tightening
it.

---

## Experiment 3 — Threshold sweep (started, paused)

After Experiment 2's regression, the question was what threshold *is* right —
the 5/3 default is empirical, and Experiment 2 only proved that 11/7 is
strictly worse, not that 5/3 is optimal.

Plan: sweep a grid of (`tests`, `threshold`) values, N=30 runs each, and look
at both VERIFIED rate and alignment-100/100 rate. The two metrics together
tell us whether a setting is failing because pruning is too lenient (over-prunes
→ small/wrong set → Phase 3 absorbed) or too conservative (under-prunes → no
bridges → alignment short).

To make the sweep cheap we added `#ifndef`-guarded `ROBUST_TESTS` /
`ROBUST_THRESHOLD` macros in `mapping_attack.c`. Default behavior is unchanged
(5/3); compiling with `-DROBUST_TESTS=N -DROBUST_THRESHOLD=K` selects a cell.
Sweep driver: `scripts/threshold_sweep.sh`.

Partial results before pausing:

| tests | threshold | verified/30 | aligned 100/30 | mean runtime (s) |
|------:|----------:|------------:|---------------:|-----------------:|
| 3 | 2 | 21 | 26 | 7.80 |
| 5 | 2 | 23 | 29 | 10.95 |
| 5 | 3 (control) | 24 | 30 | ~5 (from Experiment 1) |

**Paused.** Both lower-threshold cells we measured are *worse* than the 5/3
control, both in alignment count and in mean runtime. The runtime jump is
informative: with a more lenient threshold, individual `test_group_robust`
calls are no faster (5 trials either way), but the bootstrap loop spends more
calls on victims that yield no bridge. So 5/2 isn't free.

We're pausing the sweep here because the threshold is being measured against a
noisy substrate — `WARM_TLB` is broken, the fence pattern is non-optimal, the
governor is `powersave`, and the SMT sibling is live. Any conclusion about
threshold-sensitivity is confounded by these. We pivot to the environmental /
deterministic fixes first, then re-run this sweep on a clean baseline.

---

## Environmental / deterministic fixes — plan

After Experiment 1 (which is environmental in spirit — fix the kernel-side
latency-fd lifecycle) we identified several other deterministic noise sources
that don't depend on tuning. We tackle these before resuming threshold tuning,
because:

1. They have known-correct fixes (no tuning involved).
2. They reduce per-trial noise, which both improves Phase-3 success rates and
   makes future tuning experiments interpretable.
3. They risk no algorithmic regression, only timing-distribution regression
   that is detectable from baseline cycle counts.

Order (cheapest/most-isolated first):

| Step | Fix | Mechanism | Code change? |
|---|---|---|---|
| A | CPU governor → `performance`, no_turbo unchanged | runner script | no |
| B | Disable SMT sibling of measurement core (or pin to an E-core) | runner script | no |
| C | Fix fence pattern in `measure_access_time` | `src/utils.c` | yes |
| D | Fix `WARM_TLB` (currently flips bit 11 → same 2 MB huge page → no real TLB warm, just polluting another cache line on the same page) | `src/main.c`, `src/mapping_attack.c`, `src/negative_control.c` | yes |
| E | Quiet mid-attack `printf` (buffer or `setvbuf` + flush at boundaries) | `src/mapping_attack.c` | yes |

After E, re-run the threshold sweep against the cleaned baseline.

### Step B (first attempt) — pin to E-core (CPU 12)

E-cores on the i7-12700H are 1T (no SMT sibling), so pinning there sidesteps
SMT noise without offlining anything. Tried that first.

**Result (30 runs on CPU 12, no other changes):**
- VERIFIED **28/30 (93%)** — best Phase-3 rate observed in any configuration so far.
- Aligned 100/100 only **4/30** — sharp regression.

**Interpretation.** The E-core's lower noise floor makes measurement more
reliable (Phase 3 wins). But the bootstrap relies on `MISS_THRESHOLD = 150`
cycles, which was tuned against P-core clocks (~4.7 GHz). On a ~3.5 GHz E-core
with a smaller L2, the hit/miss distribution shifts; some real misses no longer
clear the 150-cycle bar, so `test_group_robust` returns false more often during
pruning, and `find_eviction_set` exits early or fails for many victims —
alignment falls short.

So the E-core wins where the measurement is being made (Phase 3) and loses
where pruning depends on a tuned threshold (Phases 1/2). Two ways forward:

1. **Stay on the P-core, offline the SMT sibling** (CPU 4 with CPU 5 offlined).
   Removes SMT noise without changing the cycle units the codebase was tuned
   against. Conservative.
2. **Stay on the E-core, recalibrate `MISS_THRESHOLD`** by measuring the
   actual hit and miss distributions on CPU 12 and picking a new threshold
   between them. More principled, more work, and adds a tuning variable.

We chose option 1 next. Option 2 stays on the table; if the rest of the noise
cleanup keeps Phase-3 at ~95% on the P-core we don't need it, but if Phase 3
plateaus we'll come back to it.

### Step B (second attempt) — offline SMT sibling of CPU 4

Sudoers extended to allow writes to `/sys/.../cpu*/online` and
`/sys/.../cpu*/cpufreq/scaling_governor` (under tightened glob `cpu[0-9]*` so
the entry can't match anything weird).

`echo 0 > /sys/devices/system/cpu/cpu5/online` (CPU 5 is the SMT sibling of
CPU 4 on this box). Pinning unchanged at CPU 4.

**Result (30 runs, sibling offline, governor still `powersave`):**
- VERIFIED **25/30 (83%)**
- Aligned 100/100: **30/30**

That's only marginally above Experiment 1 alone (24/30) — within run-to-run
noise. The improvement is small enough that, on this machine in this session,
SMT contention wasn't a major source of remaining flakiness (CPU 5 may simply
have been mostly idle during these runs). But it removes a future confound at
zero correctness cost, so kept.

### Step A — `performance` governor on CPU 4 (stacked on Step B)

`echo performance > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor`.
Confirmed CPU 4 jumped from ~400 MHz idle to 4.1 GHz under load.

**Result (30 runs, sibling offline + governor performance):**
- VERIFIED **21/30 (70%)** — *regression* vs the previous 25/30
- Aligned 100/100: **29/30**

The regression is small enough that it could be sample variance (n=30 with
underlying p≈0.8 has σ ≈ 2.2, so a swing of ±4 between two samples is normal).
But it's also a possible interaction with `MISS_THRESHOLD = 150`: a higher
clock means more cycles per fixed-time DRAM access, which biases the threshold.
We need a second sample (and ideally a hit/miss distribution measurement) to
disambiguate.

**Status: paused for the day.** Environment fully restored:
- CPU 4 governor → `powersave`
- CPU 5 → online again
- Sudoers grants left in place (they only enable, don't change state).

When we resume, the next move is to re-run the `performance`-governor
experiment a second time, and if the regression repeats, take a quick
hit-vs-miss cycle distribution to see whether `MISS_THRESHOLD = 150` needs to
move when the clock changes.

### Running tally

| Configuration | VERIFIED / 30 | Aligned 100/30 |
|---|---:|---:|
| Original baseline (P-core, default env) | 4/10 → 24/30 (Exp. 1) | 30/30 |
| + Step B (sibling offline) | 25/30 | 30/30 |
| + Step A (performance governor, on top of B) | 21/30 | 29/30 |
| E-core (CPU 12) baseline | 28/30 | 4/30 |

---

## Backlog — Targeted L2 set-eviction (replace the wash pool)

**Status.** Not attempted. Recorded here so we don't lose the idea. Pick up
after Steps A–E land, so any change here is measured against a clean baseline
rather than entangled with environmental noise.

**Motivation.** `wash_l2()` walks 40 000 lines (~2.5 MB) on every call. It is
called four times per `test_group` (once on line 59, then once per sweep ×3 on
line 67) and `test_group_robust` runs `test_group` five times — so ~20 wash
calls per candidate check, and the pruning loop calls `test_group_robust`
hundreds of times per `find_eviction_set`. The wash works (Experiment 1 hits
80% Phase 3) but it's brute force: blindly thrash all of L2 so the lines we
care about *happen* to be evicted.

The set-disjointness argument in §2 of `PPT_VS_BOOTSTRAP_MAP.md` (per-cell
density ≈ 1.25 wash lines, well under 12-way LLC capacity) shows the wash is
safe but oversized. A targeted version evicts only the L2 sets we actually
need to disturb.

**Proposal.** Replace `wash_l2()` with `evict_l2_set(llc_set)` — a helper that
walks a precomputed ~21-line pool whose addresses all map to the same L2 set
as the victim, but to LLC sets ≠ `llc_set`.

**Address-bit reasoning (Alder Lake P-core).**
- L1d is 48 KB / 12-way / 64-byte lines → 64 sets, set index = bits 6–11.
- L2 is 1.25 MB / 20-way → 1024 sets, set index = bits 6–15.
- LLC slice is 4096 sets, set index = bits 6–17 (`SET_INDEX_MASK = 0xFFF`).

`BOOTSTRAP_SET = 0x5A` therefore implies, for every bootstrap candidate and
the victim:
- L1d set = `0x5A & 0x3F = 0x1A`
- L2 set  = `0x5A & 0x3FF = 0x5A`
- LLC set = `0x5A`

All targets share one L1d set *and* one L2 set. So a single 21-line pool that
overflows L2 set `0x5A` simultaneously overflows L1d set `0x1A` (21 ≫ 12-way
L1d), giving an L1+L2 flush from one walk — no separate L1 wash needed.

To avoid LLC interference, pick lines with bits 16–17 ≠ `00`. Those land in
LLC sets `{0x15A, 0x25A, 0x35A}`, never `0x5A`. With bits 18–20 free, each
huge page exposes `3 × 8 = 24` qualifying offsets — plenty for the 21 we
need.

**Expected impact.**
- Per wash: ~21 timed accesses vs ~40 000 today (≈1900× per call). The
  per-call wall-clock saving is large because most of the wash today is
  L3-resident loops over the 2.5 MB pool.
- Wash overhead today is a significant fraction of bootstrap runtime (see the
  threshold-sweep timings in Experiment 3: 5–11 s, dominated by the pruning
  inner loop). The targeted version should make actual pruning trials the
  bottleneck instead of wash bookkeeping. Bootstrap runtime ought to fall
  noticeably; we should re-measure once it lands.
- Phase 3 verification rate is not the primary target, but removing
  ~10⁶ unrelated memory accesses per `find_eviction_set` should reduce
  TLB/prefetcher/store-buffer churn between thrash and measure, which may
  help marginally.

**Risks / open questions.**
1. **L2 replacement policy.** Intel's L2 is pseudo-LRU in name, QLRU/RRIP-ish
   in practice. 21 lines (associativity + 1) is the theoretical minimum;
   25–30 lines with a two-pass walk is the conservative starting point. Treat
   pool size as the first parameter to sweep.
2. **L2 set-index assumption.** Modern Intel client L2s do not use a complex
   hash, but we have not empirically confirmed bits 6–15 on Alder Lake
   P-core. Cheap one-time calibration: two lines with bits 6–15 equal but
   higher bits differing should L2-conflict; verify before relying on it.
3. **E-core portability.** Gracemont L2 is 2 MB / 16-way / shared across 4
   E-cores in a cluster — different geometry *and* concurrent noise from
   sibling E-cores. The helper must regenerate the pool on E-core pin and
   may need a larger over-provision. Implement P-core-only first.
4. **Phase 3 victim's L2 set differs.** Verification uses `target_set = 0x008`
   (L2 set `0x008`, L1d set `0x08`), not `0x5A`. The pool is target-set-
   specific. Cheap (one-time per target) but a small init refactor:
   `init_l2_evict_set(llc_set)` per target instead of a single global
   `init_l2_wash()`.
5. **Wash pool is currently also used to push the *victim* down to L3** (line
   59 of `test_group`, before the thrash). The targeted pool does the same
   thing — same L2 set, evicts the victim from L2 — so this carries over
   cleanly. Sanity-check after porting.
6. **WARM_TLB interaction.** Today's `WARM_TLB` flips bit 11 and stays in the
   same huge page (CLAUDE.md flags this as Step D). If Step D fixes it to
   touch a fresh 4 KB TLB entry, the targeted-wash pool — allocated from its
   own huge page — must not undo that warming. Re-verify after Step D lands.

**Where to start.** Add `init_l2_evict_set(int llc_set)` and
`evict_l2_set(void)` alongside `init_l2_wash()` / `wash_l2()` in
`mapping_attack.c`. Gate the switch behind `-DUSE_TARGETED_L2_EVICT` so the
binary is A/B-able against the wash default — same compile-time-flag pattern
as `ROBUST_TESTS` / `ROBUST_THRESHOLD` in Experiment 3. First-cut measurement
plan: pool size ∈ {21, 25, 30}, 30 runs each on the post-Step-E baseline,
report VERIFIED rate, aligned-100 rate, and mean runtime.

---

## Session 2026-05-29 — parity-coset fix + the real noise floor

Two separate things were conflated before: the **parity-blindness bug** (an
algorithm defect) and the **timing noise** (this doc). They are now untangled.

**Parity fix (algorithm, now reliable).** The old bootstrap used a single anchor,
so the XOR bridge could only ever propagate one parity coset (every recovered
`delta` was even `{0,3,5,6}`). `bootstrap_page_alignment` was rewritten to anchor
*each* coset independently (slice-specific seed → membership mapping → EM
refinement). Both cosets are now recovered every run.

**The noise floor is shared-L3 contention — not frequency drift, not the prune.**
Evidence chain:
- `eviction_builder` and an in-`mapping_attack` self-test show a *known-correct*
  24-line set evicts 12/12 same-slice and 0/12 different-slice **when measured
  fresh** — the primitive is clean.
- During the long bootstrap, the diff-slice false-positive rate spikes (0/30 →
  30/30 across runs). A diff-slice victim *cannot* be evicted by a slice-specific
  set, so something external is evicting it: the 30 MB LLC is shared by all
  cores, and `firefox`/`gitkraken`/`code`/`gnome-shell` thrash it. Confirmed with
  `loadavg` (1.24) and a firefox content process scheduled on the measurement
  core. This is the bimodal accuracy: good runs = quiet L3, bad runs = busy L3.

**What helped, what didn't:**
- **`performance` governor — mandatory.** On `powersave`, frequency scales
  mid-run and the fixed cycle threshold drifts; bare runs collapsed to ~0 cosets.
  Use `scripts/quiet_run.sh` (governor + idle SMT sibling + restore).
- **Balance-retry (kept).** A skewed coset split (e.g. 93/7) is the contention
  signature — a seed's set false-positives and absorbs the other coset. Rejecting
  imbalanced bootstraps and re-rolling (`BALANCE_MIN`, `MAX_BOOTSTRAP_ATTEMPTS`)
  eliminated the catastrophic single-coset collapses entirely.
- **Adaptive (control-relative) threshold (tried, reverted).** Hypothesised
  frequency-overhead drift; on the `performance` governor the drift isn't the
  issue (contention is), and the control line itself gets evicted under
  contention, making it *less* stable. Reverted to the fixed `MISS_THRESHOLD`.
- **Idling the SMT sibling — marginal.** Helps a little; the shared LLC dominates.

**Residual (open, environmental).** Even with balance-retry + governor + idle
sibling, map *contents* still degrade under contention: a balanced bootstrap can
be wrong (right shape, wrong deltas) and balance can't detect it. How bad scales
directly with concurrent L3 activity:
- **Foreground runs** (apps closed, the measuring shell *blocked* during each
  run): no collapses, every run ≥ 4/8 sets correct (ground truth), ~half 8/8.
- **Background `-DGROUND_TRUTH` batch** (same machine, but the driving agent
  process stayed at ~25% CPU *during* the runs): 1/10 fully correct, 4/10 fully
  wrong. The only difference was the concurrent L3 user — proof that *any* live
  process (even the harness driving the test) is enough to corrupt the map.

So measure with the driver idle, apps closed. The end-of-run timing `VERIFIED`
count is itself noise-hit and **understates** correctness badly (saw 8/8
genuinely-correct sets report 1/8 VERIFIED). **The reliable map-quality signal is
`-DGROUND_TRUTH`'s "genuine lines / set", not the VERIFIED line.** Expected real
fix is environmental: headless session (stop the display manager) or
`isolcpus=<core>` + idle siblings so the LLC is quiet for the whole run. CAT/RDT
(`rdt_a` is present) could partition the LLC, but the attack hard-codes a 12-way
LLC, so a CAT carve-out would have to re-derive `LLC_WAYS` — deferred.