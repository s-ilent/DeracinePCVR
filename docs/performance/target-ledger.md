# Déraciné PCVR — performance investigation target ledger

| | |
|---|---|
| **Repository** | `s-ilent/DeracinePCVR` |
| **Authoritative branch** | `main` |
| **HEAD when this ledger was created** | `314a2cbc7786206c447016fa7f6ee89d6aa755c5` |
| **Ledger created** | 2026-11-19 |
| **Created from** | the `deracine-performance-ledger-handoff.md` inventory seed (2026-11-19), reconciled against `main` |
| **Rebase note** | the seed's verified base was `9029c0d`. While this ledger was being written, `main` advanced to `314a2cb` (guest sampler-state diagnostic). The ledger was rebased onto that tip and the LUT-01 record updated accordingly; both commits are accounted for below. |
| **Scope of the creating commit** | documentation only — no renderer change, no new instrumentation, no benchmark run, no experiment |
| **Target count** | **17** target IDs: `CPU-01`…`CPU-08` (8), `GPU-01`…`GPU-05` (5), `MEM-01`, `MEM-02`, `VR-01`, `LUT-01` |

> **This is the authoritative performance investigation queue.** Every future investigation must
> update this file when it starts, when it finishes, and when it produces a negative result.
> An experiment whose result is not written here did not happen.

---

## 1. How to use this ledger

- Each item has a **persistent ID**. Never renumber or recycle one. A closed, rejected or
  failed experiment stays in the record with its rejection reason — a negative result is a
  *closed experiment*, not permission to run it again unchanged.
- Every target record carries a **status**, **evidence references**, a **next action** and a
  **dated investigation history**. Update the history in place; do not rewrite old entries.
- One investigation answers **one falsifiable question**. Keep the correctness gates that are
  relevant to the change.
- Separate, in everything you write: **confirmed measurement**, **causal hypothesis**,
  **proposed fix**, **accepted/merged implementation**.
- **No projected FPS gain without matched evidence.** A structural win (fewer passes, fewer
  copies, less traffic) is not an FPS result.

## 2. Status vocabulary

| Status | Meaning |
|---|---|
| `queued` | Open, no investigation in flight. Next action below is the entry point. |
| `investigating` | An investigation is in flight. Record the run IDs and the owner in the history. |
| `blocked` | Open, but cannot proceed until the stated gate clears (hardware, a decision, or prerequisite evidence from another target). |
| `completed` | Its question has been answered. Frozen unless genuinely new evidence appears. |
| `rejected` | The candidate approach was refused and must not be re-run unchanged; the target is no longer a queue item. |

A target may hold **closed experiments** (recorded in §8) while remaining `queued` for a
*different* question. Do not conflate the two.

## 3. Measurement semantics and comparability rules

These rules exist because most of the confusion in this project's history came from comparing
cells that were never comparable. Read them before quoting any number below.

**Non-comparable workloads.** Old front-end/calibration runs (~530 draws, cells that sit at the
title's ~58–60 fps lock) are **not** comparable to M2 heavy gameplay (~1,850–1,870 draws,
~34–37 fps). The title's front end is locked to ~60 fps by the title itself, so no front-end
number can show a sub-frame-time improvement. Render scale, MSAA count, vblank, presentation
size and scene phase all change the measurement; a comparison that differs in any of them is not
an A/B.

**GPU validation layers** markedly inflate CPU-side translation. A validation run is useful for
legality (VUID/SYNC), never for a CPU-time comparison against a non-validation run.

**Nesting.** CPU-01 contains texture/sampler/buffer binding and can include CPU-02/03/04.
CPU-04 can include CPU-02. CPU-02 and CPU-03 overlap. Driver calls can sit inside timed
translation steps. **Do not add child costs to a parent cost.** CPU and GPU execution intervals
overlap in wall time.

**Driver-call vs translation vs command-thread time.** `SHADPS4_DBG_CALL_TIMES=1` prints two
distinct lines:

- `driver calls per frame:` — the timed Vulkan driver calls (draw, push constants, push
  descriptors, vertex input/buffers, index buffer, dynamic state, pipeline, pass begin/end,
  barrier, submit).
- `translation steps per frame:` — the emulator's own per-draw work:
  `prepare state` (`Call::PrepareState`), `bind resources` (`Call::BindResources`),
  `bind vertices` (`Call::BindVertices`). Added at `9029c0d`.

The `gpu timeline` line's **`command thread`** field is the command thread's wall time per
frame. It is **not** the sum of the translation steps, and this ledger never calls it
"translation time": a residual computed as `command thread − translation steps − driver calls`
only means "not covered by those timers", which is the loose attribution CPU-08 exists to
sharpen. (The handoff seed labelled this field "translation"; that reading is corrected here.)

**GPU interval attribution.** In `gpu operations`, each marked interval runs to the next mark, so
a barrier or a copy recorded between two marks is *inside the one before it*. GPU-05 exists
because of exactly this.

**Per-frame normalization.** `RENDERSTAT` counters are emitted per report window, not per frame.
Divide by the window's own frame count (sum the `frame stats at … N frames` lines between two
reports; in the 2026-11-19 M2 runs it was 36 frames). Mixing a per-window counter with a
per-frame one is the most common arithmetic error in this project.

**Noise floor is not established.** A same-binary, same-scene baseline repeated at least three
times is required before any A/B claim (`docs/assignment01/PERF-BASELINE.md`). Interleave arms
(A–B–A–B) and keep identical workload, vblank, render scale and scene phase.

**Correctness gates.** A corrupted frame is never a valid performance result. Any performance
run must pass the visual check; render-target dumps are compared (`mean abs / 255`), and the
Vulkan/VUID status is recorded.

## 4. Current measurement context

### 4.1 M2 gameplay baseline — GPU-bound flags, light instrumentation

Family: `wt-frame-time-2026-11-18/_out/gpu-ops-verify-2026-11-19/{gameplay, gameplay-copies,
gameplay-copies2, gameplay-final, gameplay-xfer}`. Heads `c539eda` / `3278c75` / `6037acb`;
`SHADPS4_DBG_DEPTH_PASS` and `SHADPS4_DBG_BARRIER_IN_PASS` unset (both off).

| run | unique FPS | draws | GPU busy | frame | GPU executing | command thread |
|---|---|---|---|---|---|---|
| `gameplay` | 34.45 | 1,866.9 | 54.5 % | 27.57 ms | 15.20 ms | 23.65 ms |
| `gameplay-copies` | 34.43 | 1,870.0 | 51.0 % | — | — | — |
| `gameplay-copies2` | 35.49 | 1,868.1 | 51.1 % | — | — | — |
| `gameplay-final` | 35.23 | 1,866.9 | 56.9 % | 28.16 ms | 15.57 ms | 23.21 ms |
| `gameplay-xfer` | 34.73 | 1,867.7 | 49.5 % | 28.24 ms | 13.63 ms | 24.74 ms |

Attributed GPU work 13.9–15.5 ms/frame (pass ≈8.6–8.7 ms, dispatch ≈5.2 ms).
**The M2 gameplay frame is CPU-side bound**: the command thread (~23.7 ms) is the frame's long
pole and the GPU is only ~50–57 % busy. This is the single most important framing fact for
choosing work from this ledger.

### 4.2 M2 gameplay baseline — heavy diagnostic flag set

Run `wt-frame-time-2026-11-18/_out/translation-cost-2026-11-19` (`run.json`), head `f2e2831`
plus three dirty files (`vk_rasterizer.cpp`, `vk_scheduler.cpp`, `vk_scheduler.h`), executable
`CEF7688278D43B748685591269A9D0A87F68974AC323EC3BE226D6A2700315C0`.
Verdict: valid, 52/52 in-band samples, `unique_fps` 22.171, `draws_mean` 1,871.5.

> **Provenance gap in that run's manifest.** Both the `driver calls per frame` and the
> `translation steps per frame` lines are printed only when `SHADPS4_DBG_CALL_TIMES=1`
> (`vk_scheduler.cpp:475` `CallTimesEnabled`, gating both parts at `vk_scheduler.cpp:814`). The
> run's log contains both lines, so that gate **was** active; `run.json`'s `env` block does not
> record `SHADPS4_DBG_CALL_TIMES`. Treat these two numbers as real but re-state the flag when
> re-running, and do not repeat the omission.

Selected window (t = 117 s), reproduced from
`lab/user/log/shad_log.txt`:

```
frame stats: 31.5 fps, 58.0 passes (57.0 loading their targets, 909.2 Mpx), 1867 draws,
             33.0 dispatches, 2.6 submits, GPU busy 45%
gpu timeline: 13.83 ms of the 31.79 ms frame executing on the GPU (43%), command thread 28.81 ms
gpu operations: 13.70 ms a frame attributed (pass 8.44 ms x1856, dispatch 5.26 ms x448)
driver calls per frame: 2.94 ms in all: … submit 3x 1.77 ms
translation steps per frame: 16.85 ms in all:
    prepare state 2060x 2.21 ms, bind resources 1867x 11.41 ms, bind vertices 1867x 3.23 ms
```

- `28.81 − 16.85 − 2.94 = 9.02 ms` of command-thread time is **not covered by those timers**.
  That is an attribution gap, not a measured "unclassified cost" (see CPU-08).
- The same run's earlier seconds show the warm-up trend of the translation steps:
  23.06, 22.95, 21.08, 21.10, 20.63, 19.77, 18.10, 16.85 ms. Other logs show ~21.1–24 ms of
  steps under different flags — that is a flag/phase difference, **not** a regression.
- The heavy timers depress frame rate (22.17 vs ~34.7 unique FPS in §4.1). Never compare
  §4.2 against §4.1.

### 4.3 Validation build (legality only)

`query-reset-verify-2026-11-19/gameplay`: GPU executing 15.27 ms, attributed 15.18 ms
(`pass 9.43 ms x513, dispatch 5.75 ms x126`), and **command thread 112.85 ms**. Validation
inflates the CPU side by ~4–5×; use this run for VUID/SYNC status, never for CPU comparisons.

### 4.4 Scene identity and gates

- Scenario `deracine-headless-gameplay-300-msaa2-vblank120`, profile
  `tools/benchmark/reference/scene-gameplay.json`; gate `min_draws 800`, `min_mpx 450`,
  `consecutive_samples 2`; advisory band draws 1500–2300, Mpx 550–750, passes 35–55.
- Environment: `SHADPS4_TITLE_EYE_WIDTH=2880`, `SHADPS4_VR_SCALE=0.5`, `SHADPS4_MAX_MSAA=2`,
  refresh/FPS cap 120, `SHADPS4_VR_HANDS=synthetic`, `SHADPS4_VR_MOVE_FROM_PAD=1`,
  `SHADPS4_DBG_AUTOPRESS_TO=45000`, vblank 120.
- The advisory band is **not** a hard gate and is not currently met: the M2 cells measure
  ~909 Mpx and 58 passes at ~1,867 draws. Re-derive the band only from a live cell, and record
  which band a cell was judged against before comparing two cells.
- VRAM in the M2 cells is steady at ~3,367–3,372 MB (`RENDERSTAT vram_mb`).

### 4.5 Standing constraints

- `docs/AUTONOMOUS_ENGINEERING.md` is mandatory for benchmark, capture and protected-path work;
  its enforcement lives in `tools/benchmark/` (validity validator, preflight/postflight hashes,
  path guard, provenance manifest).
- Headset acceptance (`docs/integration/HEADSET-ACCEPTANCE.md`) is **not executed**; the 120 FPS
  target is recorded there, not used as a pass/fail gate.
- Do not modify `pc-vr/`, the launcher, `settings.txt`, the shared lab or the recovery evidence
  without explicit approval.

---

## 5. Target summary

| ID | Target | Status | Priority | Headline measured cost | Next action (short) |
|---|---|---|---|---|---|
| CPU-01 | `BindResources` parent | `queued` | P0 | 11.41 ms/frame over 1,867 calls | Decompose without adding child costs |
| CPU-02 | Depth-overlap resolution | `queued` | P0 | ~3.7 ms/frame `depth_ms` (M2 window) | Split matching vs conversion vs recreation vs sync |
| CPU-03 | Vulkan image create/destroy | `queued` | P0 | ~0.72 ms create + ~2.14 ms destroy per frame, ~4 creations/frame | Find avoidable churn without reuse hazards |
| CPU-04 | `FindImage` slow path / generation | `queued` | P1 | 13,639 lookups/frame, 1,651 slow, 83.9 % generation mismatches | Quantify wasted-but-exclusive work |
| CPU-05 | Vertex/index binding | `queued` | P1 | 3.23 ms/frame over 1,867 calls | Split cache search vs binds vs barriers |
| CPU-06 | Render-state preparation | `queued` | P1 | 2.21 ms/frame over 2,060 calls; ~193 draws skipped after it | Test safe reordering after the empty-draw decision |
| CPU-07 | Driver queue submission | `queued` | P1 | `vkQueueSubmit` 1.77 ms/frame, 3 calls | Determine driver work vs back-pressure vs dependency |
| CPU-08 | Unclassified translation | `queued` | P1 | ~9.0 ms/frame command-thread gap (selected window) | Find the largest missing exclusive cost with existing timers |
| GPU-01 | Depth/scene preservation copies | `queued` | P0 | ~3.9 copies and ~139 MB/frame (M2 window), `depth_ms` ~3.7 ms | Matched A/B of the existing opt-in depth-write pass |
| GPU-02 | Guest compute `799062d096fa2314` | `queued` | P1 | 8 dispatches/frame, 4.86–5.45 ms/frame aggregate | Identify shader, geometry, I/O and critical path |
| GPU-03 | Render passes and load/store | `queued` | P1 | pass attribution 8.44–8.70 ms/frame; heaviest pass 5.85–6.53 ms | Attribute pass cost vs load/store/barrier transitions |
| GPU-04 | Depth-pass and barrier-in-pass switches | `queued` | P1 | opt-in, both off by default; matched-workload benefit unproven | Re-test the *safe* options only, in matched M2 |
| GPU-05 | Copy attribution coverage | `queued` | P1 diagnostic | direct `Runtime::CopyImage` unmarked (source-confirmed) | Add correctly located direct-copy markers |
| MEM-01 | Image recycler | `blocked` | P0-linked, high risk | historic 4.15 → 0.29 ms create+destroy, scene-mismatched arms | Independent correctness/cohesion review; never partially |
| MEM-02 | High-scale memory commitment and stalls | `queued` | P2 | ~3.37 GB steady VRAM; historic multi-GB commitment and protection failures | Test causal link to current M2 stalls |
| VR-01 | OpenXR presentation and pacing | `blocked` | P2 | instrumentation in `main`; no headset session | Worn-headset latency decomposition |
| LUT-01 | 3D LUT array/volume conversion and sampling | `closed` | P2 correctness | ~2 `ExpandImage`/frame; `ExpandImage` ~0.08 ms/frame | Done 2026-11-19: contents (32/32), conversion (1:1 ordered) and sampler (`clamp_z` = `ClampLastTexel` → `ClampToEdge`, no wrap) all measured clean |

---

## 6. Target records

### CPU-01 — `BindResources` parent

- **Status:** `queued` · **Priority:** P0 · **Last update:** 2026-11-19
- **Measured:** M2 **11.41 ms/frame** over **1,867 calls** (translation step `bind resources`,
  `Call::BindResources`). It is the largest single timed translation step in the M2 window
  (11.41 of 16.85 ms).
- **Measurement semantics:** this is an inclusive parent. It contains texture/sampler/buffer
  binding and **may include CPU-02/03/04**. Do **not** add child costs to 11.41 ms.
- **Provenance:** `translation-cost-2026-11-19` (selected window, §4.2);
  `src/src/video_core/renderer_vulkan/vk_scheduler.h` (`Call::BindResources`);
  `vk_rasterizer.cpp:422` (`CallTimer{FrameStats::Call::BindResources}`),
  `vk_rasterizer.cpp:937` (`Rasterizer::BindResources`).
- **Dependencies:** parent of CPU-02, CPU-03, CPU-04. Feeds the causal chain
  `CPU-01 → CPU-04 → CPU-02 → CPU-03 ↔ MEM-01`.
- **Prior experiments / negative results:** none targeting CPU-01 directly.
- **Safety concerns:** it sits on the resource-binding path for every draw; a change here can
  alter descriptor contents, barriers and aliasing for the whole frame.
- **Open question:** *What fraction of the 11.41 ms is buffer/sampler/descriptor work versus
  image lookup/alias handling, and which part is avoidable?*
- **Next action:** decompose the step **without adding child costs to the parent**, by reading
  the existing timers per sub-phase (image lookup versus descriptor writes) — measure once, and
  attribute each child to its own target.
- **History:**
  - 2026-11-19 — measured at `9029c0d` (11.41 ms / 1,867 calls); recorded as the largest timed
    step; no optimization attempted.

### CPU-02 — Depth-overlap resolution

- **Status:** `queued` · **Priority:** P0 · **Last update:** 2026-11-19
- **Measured:** latest M2 **~3.7 ms/frame** `depth_ms` (`ResolveDepthOverlap`), from the
  `findimage-profile-2026-11-19-0128` RENDERSTAT window (`depth_ms=133.5` over 36 frames).
  Earlier front-end measurement: **~3.5 ms/frame** inclusive of image and copy-related work,
  mostly within `ResolveOverlap` (`overlap_ms=137.9` / 36 ≈ **3.8 ms/frame** in the M2 window).
- **Measurement semantics:** `depth_ms` and `overlap_ms` overlap each other and are nested inside
  CPU-01; they are per-window counters, not per-frame ones (§3).
- **Provenance:** `findimage-profile-2026-11-19-0128` RENDERSTAT
  (`depth_ms`, `overlap_ms`); `src/src/video_core/texture_cache/texture_cache.cpp:310`
  (`TextureCache::ResolveDepthOverlap`), `texture_cache.h:139`;
  `docs/assignment01/PERF-BASELINE.md` "Established bottlenecks" 1.
- **Dependencies:** child of CPU-01/CPU-04; overlaps CPU-03; resolved representationally by
  GPU-01.
- **Prior experiments / negative results:** the unsafe preservation skips (see GPU-04 and §8)
  removed this work but corrupted later loads. The safe variants kept correctness and showed no
  matched FPS gain.
- **Safety concerns:** whatever replaces it **must preserve the contents the next
  `loadOp=LOAD` consumer reads**. The class of mistake is already demonstrated: the skipped
  copies were exactly the depth contents the next depth pass loaded.
- **Open question:** *Which parts are matching/bookkeeping, format alias conversion, image
  recreation and synchronization?*
- **Next action:** split `ResolveDepthOverlap` into those four buckets using existing counters,
  and state which bucket is exclusive of CPU-03.
- **History:**
  - 2026-11-17 — front-end measurement ~3.5 ms/frame; unsafe skip tried and rejected (GPU-04).
  - 2026-11-19 — M2 window measured `depth_ms` 133.5 ms / 36 frames ≈ 3.7 ms/frame; not
    decomposed.

### CPU-03 — Vulkan image creation/destruction

- **Status:** `queued` · **Priority:** P0 · **Last update:** 2026-11-19
- **Measured:** M2 **~0.72 ms create + ~2.14 ms destroy per frame**, **~3.9 image
  creations/frame** (`create_ms=25.9`, `destroy_ms=77.1`, `images_created=140`,
  `images_destroyed=140` over 36 frames). Earlier front-end measurement: **~0.78 + 2.5 ms**.
  The seed's figures (0.68 / 1.91 / ~4) are the same counters in a neighbouring window.
- **Measurement semantics:** **potentially nested** within CPU-01/02. `images_created` counts
  raw creations, not distinct images.
- **Provenance:** `findimage-profile-2026-11-19-0128` and
  `image-cache-4096-diagnostic-b1` RENDERSTAT (`create_ms`, `destroy_ms`, `images_created`,
  `images_destroyed`, `copies`, `copy_mb`);
  `src/src/video_core/texture_cache/image_stats.h`;
  `docs/research/cross-fork-renderer.md` (historical recycler measurement).
- **Dependencies:** child of CPU-01/02; coupled to GPU-01; the historic recycler is MEM-01.
- **Prior experiments / negative results:** the historic recycler (MEM-01) removed this cost but
  is not a proven gameplay win and is not in the runtime; see §8.
- **Safety concerns:** removing allocation churn by reusing images risks premature reuse,
  VRAM pressure and corrupt prior contents. The 2026-11-17 headset trial's soft red overlay is
  the observed failure class for stale image contents.
- **Open question:** *Can avoidable allocation churn be removed without premature reuse, VRAM
  pressure or corrupt prior contents?*
- **Next action:** classify the ~3.9 creations/frame by reason (alias re-creation, expand,
  refresh, depth target) and report which class is avoidable, before proposing any pooling.
- **History:**
  - 2026-11-17 — front-end ~0.78 + 2.5 ms; recycler prototype measured (MEM-01).
  - 2026-11-19 — M2 window ~0.72 + 2.14 ms per frame over 36 frames; creation classes not yet
    separated.

### CPU-04 — `FindImage` slow path / global generation

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** **13,639 image lookups/frame**, of which **1,651/frame are slow**
  (`find_slow_calls=59,437 / 36`); **49,875 / 59,437 = 83.91 %** of the slow misses are
  **generation mismatches** (`find_generation`), the other 16.1 % are key misses
  (`find_key=9,562`). Slow-path cost `find_slow_ms=156.2 / 36` ≈ **4.34 ms/frame**; total lookup
  cost `find_total_ms=161.8 / 36` ≈ **4.5 ms/frame**. The seed's **5.02 ms/frame** elapsed-wall
  figure is a neighbouring window of the same counter family and includes possible lock waits
  and nested `ResolveOverlap`.
- **Measurement semantics:** the slow path is **not exclusive** of CPU-02 (nested
  `ResolveOverlap`), and the wall-clock variant can include lock waits. 13.8k lookups/frame is
  the `frame shortcuts` line (`N of M image lookups`), which is per frame; the `find_*` counters
  are per RENDERSTAT window.
- **Provenance:** `findimage-profile-2026-11-19-0128` (`frame shortcuts` + RENDERSTAT
  `find_*`); `src/src/video_core/texture_cache/texture_cache.cpp` (`FindImage`, register/
  unregister, `alias_generation`);
  `image-cache-4096-clean-b1` / `image-cache-4096-diagnostic-b1` (the rejected capacity
  experiment).
- **Dependencies:** child of CPU-01; can include CPU-02.
- **Prior experiments / negative results:** **cache capacity 1,024 → 4,096 gave no meaningful
  gameplay improvement and was reverted.** Do not retry cache capacity unchanged.
- **Safety concerns:** the slow path is where alias registration and invalidation happen.
  Skipping searches can serve stale contents.
- **Open question:** *What portion of slow-page searches is actually wasted and exclusive of
  CPU-02, without breaking alias/register invalidation?*
- **Next action:** quantify the wasted-but-exclusive portion of the generation-mismatch misses
  (why the generation changed, how often the found image was the same one) using the existing
  counters.
- **History:**
  - 2026-11-19 — capacity experiment (1,024 → 4,096) measured and reverted (see §8).
  - 2026-11-19 — counters re-derived from the M2 window: 13,639 lookups/frame, 1,651 slow/frame,
    83.91 % generation mismatches, ≈4.5 ms/frame total lookup cost.

### CPU-05 — Vertex/index binding

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** M2 **3.23 ms/frame** over **1,867 calls** (`bind vertices`,
  `Call::BindVertices`). Driver-side vertex input/buffers/index-buffer calls are separate and
  small (0.13 + 0.10 + 0.06 ms in the same window) — they are in the driver line, not in this
  step.
- **Provenance:** `translation-cost-2026-11-19` (selected window);
  `vk_scheduler.h` (`Call::BindVertices`).
- **Dependencies:** independent of the depth chain once nesting is excluded; part of the
  translation remainder (CPU-05/06/07/08).
- **Prior experiments / negative results:** none.
- **Safety concerns:** low. Vertex/index binding is state setup; mistakes show up as wrong
  geometry rather than silent corruption.
- **Open question:** *What is the split between buffer-cache search, state binds,
  synchronization and barriers?*
- **Next action:** split the step and test only low-risk, measurable simplifications.
- **History:**
  - 2026-11-19 — measured at `9029c0d`; not decomposed.

### CPU-06 — Render-state preparation

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** M2 **2.21 ms/frame** over **2,060 calls** (`prepare state`,
  `Call::PrepareState`). **~193 draws are skipped *after* state preparation but *before*
  `BindResources`** — the same run reports `prepare state 2060x` against `bind resources 1867x`
  and `193 draws left out that write nothing`.
- **Provenance:** `translation-cost-2026-11-19` (selected window);
  `vk_rasterizer.cpp` (draw path; `BindResources` early-out);
  `vk_scheduler.h` (`Call::PrepareState`).
- **Dependencies:** independent avenue within the translation remainder.
- **Prior experiments / negative results:** none.
- **Safety concerns:** register state can have side effects (dirty flags, pipeline choice,
  barriers) that later draws or the cache depend on. Reordering must account for those before
  changing order.
- **Open question:** *Is any preparation safely movable after the empty-draw decision?*
- **Next action:** enumerate the side effects of preparation for the 193 skipped draws and
  decide whether any part is provably side-effect-free.
- **History:**
  - 2026-11-19 — measured at `9029c0d`; the 2,060 vs 1,867 call gap (≈193) identified from the
    same window.

### CPU-07 — Driver queue submission

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** `vkQueueSubmit` **1.77 ms/frame** over **3 calls**, within **2.94 ms/frame** of
  total timed driver calls (M2 window). It is the largest single driver call in that window
  (next: pipeline 384× 0.30 ms).
- **Measurement semantics:** may overlap other translation steps; driver calls can sit inside
  timed translation steps. Do not add it to the translation steps as if disjoint.
- **Provenance:** `translation-cost-2026-11-19` (`driver calls per frame`);
  `src/src/video_core/renderer_vulkan/vk_scheduler.cpp`.
- **Dependencies:** translation remainder; interacts with GPU-01/GPU-05 (submission counts and
  copy placement).
- **Prior experiments / negative results:** none. Note that in the lighter gpu-ops runs the
  command thread is ~23.7 ms with 2.6 submits/frame, so submission count alone does not explain
  the frame.
- **Safety concerns:** submission structure controls GPU synchronization; changing it risks
  races and losing the correctness evidence the project already has.
- **Open question:** *Is this driver CPU work, back-pressure, or a necessary dependency — and
  does it actually gate frame completion?*
- **Next action:** determine whether submit time correlates with submission content (copy
  placement, waits) or is uniform back-pressure, using existing timers across cells.
- **History:**
  - 2026-11-19 — measured at `9029c0d`; not attributed further.

### CPU-08 — Unclassified translation

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** approximately **~9.0 ms/frame** residual in the selected M2 window, i.e.
  `command thread 28.81 ms − translation steps 16.85 ms − driver calls 2.94 ms = 9.02 ms`. The
  seed's "~9 ms" estimate is reproduced exactly, but it is an **attribution gap**, loosely
  attributable to decode/loop/presentation and other operations — not a fully reconciled
  exclusive account.
- **Provenance:** `translation-cost-2026-11-19` (selected window); same run's `gpu timeline`,
  `driver calls` and `translation steps` lines.
- **Dependencies:** overlaps everything on the command thread; must not be added to CPU-01/05/06/07.
- **Prior experiments / negative results:** none.
- **Safety concerns:** none directly; the risk is methodological — inventing a new framework
  instead of using the timers that exist.
- **Open question:** *What is the largest missing exclusive cost?*
- **Next action:** find it **with the existing instrumentation** rather than building another
  framework.
- **History:**
  - 2026-11-19 — residual reproduced as an arithmetic gap (9.02 ms); explicitly recorded as
    unattributed rather than measured.

### GPU-01 — Depth/scene preservation copies

- **Status:** `queued` · **Priority:** P0 · **Last update:** 2026-11-19
- **Measured:** the depth alias changes `R32_SFLOAT` ↔ `D32_SFLOAT_S8_UINT`; the full surface is
  **74.65 MB** (0x4ac0000) at 5760×3240, **1 sample**, and the guest uses it as both a compute
  storage image and the scene depth/stencil target. Historical conversion copy:
  **3.6–4.9 ms per operation** (`vkCmdCopyImage` R32F → D32S8), versus **0.166 ms** for the
  depth-write pass replacement; ~2 depth representations/frame. Later `XFER_GPU` measured
  **avg 1.949–1.991 ms** per copy (`copies=148`, no drops, no not-ready) — an upper bound that
  includes queue wait.
- **Current M2 state (recorded by this ledger):** in the last RENDERSTAT window of
  `translation-cost-2026-11-19` (36 frames) the run shows `copies=140`, `copy_mb=4992.1`,
  `depth_ms=133.5`, `overlap_ms=137.9` — i.e. **≈3.9 copies and ≈139 MB of copy traffic per
  frame, with ≈3.7 ms `depth_ms` and ≈3.8 ms `overlap_ms` per frame**. That is the right order
  of magnitude for ~2 × 74.65 MB depth-representation copies per frame plus the 32³ LUT's two
  expands; the exact split is not yet attributed and is exactly what GPU-05's missing direct-copy
  markers would settle. **The copies do still occur on `main`**: the depth-write pass that
  replaces them is integrated but **off by default** (`SHADPS4_DBG_DEPTH_PASS`, commits `56a6f8a`
  + `58346e9`).
- **Measurement semantics:** "measured in multiple, noncomparable runs" — the 3.6–4.9 ms figure
  and the 1.99 ms figure come from different builds/configurations and must not be subtracted
  from each other. `XFER_GPU` absorbs waits and is not pure transfer time.
- **Provenance:** `_build/ALIASING-REPORT.md` (formats, roles, stencil, per-operation costs,
  dumps, the pass that replaced it); `_build/PASSES-AND-PACING.md` §10 (the +34 % GPU-bound
  depth-pass A/B); `gpu-ops-verify-2026-11-19/gameplay-xfer` (`XFER_GPU`);
  `translation-cost-2026-11-19` (RENDERSTAT `copies`/`copy_mb`/`depth_ms`/`overlap_ms`);
  `src/src/video_core/texture_cache/texture_cache.cpp:310`; `src/src/video_core/renderer_vulkan/vk_runtime.cpp:227`;
  `docs/assignment01/PERF-BASELINE.md`.
- **Dependencies:** shares its root with CPU-02 and CPU-03; its evidence is informed by GPU-05;
  alternative modes are GPU-04; the recycler (MEM-01) is the historic CPU-side counterpart.
- **Prior experiments / negative results:** the unsafe preservation skip (see GPU-04/§8) removed
  the copies and broke correctness. The depth-write pass is **validated on the title screen
  only** (dumps identical to 0.005/255 mean abs, 0 VUIDs) and measured +34 % on a **400 %
  GPU-bound front-end** A/B — **not** on matched M2 gameplay, and the front-end is 60-locked.
  The +34 % figure is explicitly reported by another workstream and is **not** independently
  re-validated.
- **Safety concerns:** do not simply skip required copies — later `loadOp=LOAD` consumers read
  them. Stencil **is** used (`stencil_enable=1`, `zpass=ReplaceTest`, write mask 0xff,
  `Stencil8`), so the stencil plane must not be dropped; neither path carries stencil across a
  representation change, and it is preserved in place. `gl_FragDepth` writes rely on
  `VK_EXT_depth_range_unrestricted` being enabled for out-of-`[0,1]` values.
- **Open question:** *Which exact copy commands exist in **current M2**, which contents must be
  preserved, what is actual GPU execution versus dependency, and which copies delay the frame?*
- **Next action:** a matched M2 gameplay A/B of the **already integrated** opt-in
  `SHADPS4_DBG_DEPTH_PASS=0` vs `=1` on one binary and one scene, with render-target dumps,
  `XFER_GPU`, the RENDERSTAT copy/`depth_ms`/`overlap_ms` counters and unique FPS per arm,
  interleaved, ≥3 repeats, correctness gates recorded. This is the recommended first
  investigation (§7).
- **History:**
  - 2026-11-17 — aliasing characterised (`R32_SFLOAT` ↔ `D32_SFLOAT_S8_UINT`, 74.65 MB,
    2 re-creations/frame); the copy measured at 3.6–4.9 ms and the depth-write pass at
    0.166 ms; title-screen dumps and 0 VUIDs; frame 24.0 → 17.6 ms reported. Unsafe skips
    rejected (GPU-04).
  - 2026-11-17 — `56a6f8a` landed the salvaged depth-write pass; it was measured +34 % on a
    400 % GPU-bound front-end A/B (`PASSES-AND-PACING.md` §10).
  - 2026-11-18 — `58346e9` made the pass **opt-in**, not the default, pending gameplay evidence.
  - 2026-11-19 — `XFER_GPU` runs recorded (avg 1.949–1.991 ms/copy) and the M2 RENDERSTAT window
    confirmed the churn is still present on `main` with the pass off (≈3.9 copies / ≈139 MB per
    frame). Matched-playback A/B **not** yet run.

### GPU-02 — Guest compute shader `799062d096fa2314`

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** **8 dispatches/frame** (`x176`–`x256` over 22–30 frame windows), aggregate
  **4.86–5.45 ms/frame** in the M2 heavy-diagnostic run; the seed records **~5.0–5.75 ms/frame**
  across other windows. It is the single most expensive dispatch in the run and the second most
  expensive GPU interval after the main scene pass. Dramatically scene-dependent; SPIR-V is
  cached. UV scaling/offset identifiers suggest a resampling/warp pass but do **not** prove it.
- **Provenance:** `translation-cost-2026-11-19` (`gpu operations` costliest list);
  `query-reset-verify-2026-11-19/gameplay` (`dispatch 6.18 ms x80` and `5.32 ms x72` for the
  same shader — scene-dependent).
- **Dependencies:** a guest workload; separate from the depth/resource chain. Informs GPU-03.
- **Prior experiments / negative results:** none. No removal has been attempted or proposed.
- **Safety concerns:** this is guest code. Removing or altering it changes the picture; there is
  no evidence yet that it is off the critical path or redundant.
- **Open question:** *What is this shader, what is its dispatch geometry, what are its inputs and
  outputs, and does it lie on the frame critical path?*
- **Next action:** identify the shader (disassembly, resource bindings, dispatch geometry from a
  capture) **without** speculative removal.
- **History:**
  - 2026-11-19 — cost measured in the M2 window (4.86–5.45 ms/frame at 8 dispatches/frame);
    purpose still unknown.

### GPU-03 — Render passes and load/store behaviour

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured:** historical profiler pass total **~8.24 ms/frame**; heaviest individual pass
  **~5.9 ms/frame**. Confirmed in the M2 window as pass attribution **8.44 ms/frame**
  (`x1856`) with heaviest pass **5.85 ms ×32** — `5760×3240`, `1 colour + depth`, vs
  `72abf67b5ff30cc3`, ps `49d2380cb717a18e`. The full M2 pass mix is ~58 passes/frame, **57 of
  which load their targets** and ~909 Mpx of target writes.
- **Measurement semantics:** the pass total and the heaviest pass are per frame, but the `xN`
  counts are per window. "Marked intervals run to the next mark", so a barrier or copy between
  two marks is inside the earlier one.
- **Provenance:** `translation-cost-2026-11-19` (`gpu operations`); `_build/PASSES-AND-PACING.md`
  (89 → 65 passes from the barrier change; 74 → 12 pass breaks; 556 → 470 Mpx);
  `src/src/video_core/renderer_vulkan/vk_scheduler.cpp` (`PassKind`, `BeginRendering`).
- **Dependencies:** informed by GPU-05; related to GPU-01 (an expensive alias preservation may
  force pass structure); the merged barrier-in-pass change is GPU-04.
- **Prior experiments / negative results:** the barrier-in-pass change removed 24 pass breaks and
  142 Mpx/frame but moved fps by only ~2.6 % on a GPU-bound front-end cell and was **not
  reproduced in gameplay**; see GPU-04 and §8.
- **Safety concerns:** changing pass structure affects attachment load/store, which is a
  correctness surface (uninitialised attachments, lost clears).
- **Open question:** *Which pass, load/store or barrier transition dominates GPU execution and/or
  forces expensive alias preservation?*
- **Next action:** attribute the 8.44 ms pass total across the heaviest passes and the
  load/store/barrier transitions around them, using the existing marks.
- **History:**
  - 2026-11-17 — barrier-in-pass merged as opt-in; structural win measured, fps effect not
    resolvable on the 60-locked front end.
  - 2026-11-19 — M2 window pass total and heaviest pass confirmed; not attributed further.

### GPU-04 — Depth render-pass copy + barrier-in-pass switches

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured / current state:** both options are **integrated and off by default**:
  `SHADPS4_DBG_DEPTH_PASS` (unset = image copy; commits `56a6f8a` + `58346e9`) and
  `SHADPS4_DBG_BARRIER_IN_PASS=1` (commit `267d200`). Historical alternatives reduced recorded
  pass count and Mpx but **not** matched gameplay FPS: the barrier change measured ~+2.6 % on a
  400 % GPU-bound front end and was not reproduced in gameplay.
- **Provenance:** `src/src/video_core/texture_cache/texture_cache.cpp:380`
  (`SHADPS4_DBG_DEPTH_PASS`), `src/src/video_core/texture_cache/image.cpp:584`
  (`SHADPS4_DBG_BARRIER_IN_PASS`); commits `56a6f8a`, `58346e9`, `267d200`, `33ebbf8`;
  `_build/PASSES-AND-PACING.md` §2, §7, §10;
  `docs/integration/INTEGRATION-MATRIX.md` (rows 19, 33; §C).
- **Dependencies:** the depth-pass half is the intervention GPU-01 must test; the barrier half is
  a GPU-03 intervention.
- **Prior experiments / negative results:**
  - **Unsafe NHL-24-style conditional preservation skip** (`SHADPS4_DBG_NHL_COND=1`, skip
    preservation when binding as depth/colour target): **48.3 fps** average
    (47.9 / 46.7 / 50.2) against a 33.5 fps baseline, but **incorrect** — each skipped copy is
    followed by a depth attachment whose `loadOp` is **LOAD**, so the copies it removed were
    exactly the contents the next depth pass read. An earlier unsafe skip reached 73 fps and
    also corrupted rendering. **Rejected; do not revive the NHL predicate.**
  - **Conservative predicate** (`CanDiscardPreviousContents` with the real consumer semantics):
    **34.7 fps**, i.e. no gain over baseline; it correctly preserved everything.
  - **Barrier-in-pass:** +2.6 % at 400 % on a GPU-bound front end, not reproduced in gameplay.
- **Safety concerns:** the failure mode is already demonstrated twice; a full-surface preservation
  skip without consumer-semantics proof is unsafe. Do not revive the NHL predicate in any form.
- **Open question:** *Can either **existing safe** option help in a truly matched M2 workload,
  without content corruption?*
- **Next action:** re-test the safe options only, in matched M2, taking the depth-pass half
  together with GPU-01 (same A/B) and the barrier half separately afterwards.
- **History:**
  - 2026-11-17 — NHL skip measured at 48.3 fps and rejected for load-content corruption;
    conservative predicate measured at 34.7 fps with no gain; barrier change integrated opt-in.
  - 2026-11-18 — barrier/pass behaviour documented in `PASSES-AND-PACING.md` §7 (structural win
    unambiguous, fps effect unresolvable on a 60-locked scene).
  - 2026-11-19 — recorded that both safe options remain off by default with no matched-workload
    result.

### GPU-05 — Copy attribution coverage

- **Status:** `queued` (diagnostic) · **Priority:** P1 · **Last update:** 2026-11-19
- **Measured / current state:** the `PassKind::Blit` labelling exists in `main` and the blit
  helper's passes are marked as copies rather than passes (commit `6037acb`;
  `vk_scheduler.cpp:998`), but the M2 costliest lists show **no `[copy]` entries at all** —
  consistent with the tested scene producing zero such marks. **`Runtime::CopyImage`
  (`vk_runtime.cpp:227`) issues `vkCommandBuffer::copyImage` with no interval mark**
  (source-verified), and `Runtime::CopyImageWithBuffer` likewise. `XFER_GPU` may absorb waits
  and is not pure transfer time.
- **Provenance:** `src/src/video_core/renderer_vulkan/vk_runtime.cpp:227`,
  `src/src/video_core/texture_cache/blit_helper.cpp:100,211,324,463`,
  `src/src/video_core/renderer_vulkan/vk_scheduler.cpp:998`, `vk_scheduler.h` (`PassKind`);
  commit `6037acb`; `translation-cost-2026-11-19` (`gpu operations` has no `[copy]` row).
- **Dependencies:** informs GPU-01 and GPU-03; its absence is why GPU-01's exact copy split is
  currently unavailable.
- **Prior experiments / negative results:** one attempt at labelling blits in `main` produced zero
  marks in the tested scene; the direct-copy path was never marked.
- **Safety concerns:** none; this is measurement only. Do not split render passes merely to get
  timestamps.
- **Open question:** *What do the direct copies cost, and where are they?*
- **Next action:** add correctly located markers to the **direct** `Runtime::CopyImage` path,
  exercise the actual blit path, and separate copies from barriers and passes — without
  splitting render passes merely for timestamps.
- **History:**
  - 2026-11-19 — `PassKind::Blit` labelling landed (`6037acb`); zero marks in the tested scene;
    direct `Runtime::CopyImage` confirmed unmarked in source.

### MEM-01 — Image recycler

- **Status:** `blocked` · **Priority:** P0-linked, high risk · **Last update:** 2026-11-19
- **Measured:** earlier front-end prototype reported **~85 % hits** (first pool scored 0 hits;
  eviction-by-size with 16 slots reached 85 %) and create/destroy time **4.15 → 0.29 ms/frame**
  — but **across scene-mismatched arms** (the `off` arm at 4.15 ms was the *calibration* screen,
  the `on` arm the classroom), so it is **not** a valid A/B and the 4.15 → 0.29 figure remains
  unverified. The prototype claimed to remove ~3.9 ms/frame of GPU-thread work and used a
  **250 MB** pool. Later headless M2 harness work reports the recycler cutting image
  create/destroy from 0.851/2.292 ms to 0.019/0.008 ms per frame at 2.0 hits/frame, costing
  ~205 MB VRAM, **with no FPS change beyond noise**.
- **Provenance:** `docs/research/cross-fork-renderer.md` and `docs/research/source-digest.md`
  (branch `investigation/cross-fork-renderer`); `docs/assignment01/RECYCLER-REVIEW.md`;
  `docs/assignment01/REVISED-CONCLUSIONS.md` §2;
  `candidate/image-recycler-2026-11-17` @ `4d244d1`; `src/src/video_core/texture_cache/image_stats.h`
  (`recycler_hits`/`recycler_misses`/`pool_images`/`pool_mb`/`recycler_destroys`);
  `docs/integration/INTEGRATION-MATRIX.md` rows 18, 22; §C.
- **Dependencies:** the CPU-side counterpart of CPU-03 and closely coupled to GPU-01. Historic
  verdicts: **not integrated**, zero recycler code in the unified branch, must stay opt-in and
  off.
- **Prior experiments / negative results:**
  - Headset gameplay trial (`rec3-A02FB23F.exe`, `de9c78c` + recycler default-on) produced a
    **soft red overlay** over correct geometry/lighting; the recycler was the leading suspect.
  - **The recycler was then cleared of causing the classroom patches**: recycler OFF vs ON at the
    classroom gave byte-identical LUT slice 0 and a floor-region diff of mean 0.58/255, max 4,
    no pixel > 8. The red-overlay attribution in a headset gameplay session **remains
    unresolved**.
  - A later local port **crashed at startup even with the recycler disabled**; a lifecycle/
    integration defect is suspected but **unproven** (local branch `fix/image-recycler-startup-2026-11`
    @ `248e30b`, not published on `origin`).
  - Historical headset colour risk; one standalone candidate hardened only `pNext`.
  - The M2 harness result (no fps change beyond noise, ~205 MB VRAM) is a **negative performance
    result** and should be recorded wherever the recycler is next considered.
- **Safety concerns:** high. A stale recycled image consumed as a mask/light/effect texture is
  the observed failure class. Reusing images requires VMA ownership, async tick completion,
  `UniqueImage` moves, fresh image state, shutdown and fallback all to be correct **together**.
- **Open question:** *Can a complete subsystem preserve VMA ownership, async tick completion,
  `UniqueImage` moves, fresh image state, shutdown and fallback?*
- **Next action:** review the original source as a **cohesive unit** — no partial reconstruction —
  and establish recycler correctness independently (off/on, same binary, same scene, dumps
  compared) before any performance claim. Unblock requires a worn-headset correctness session or
  a headless equivalent that isolates image reuse.
- **History:**
  - 2026-11-17 — prototype measured (85 % hits; scene-mismatched 4.15 → 0.29 ms); candidate
    `4d244d1`.
  - 2026-11-17 — headset trial red overlay; recycler made the leading suspect and its default-on
    integration rejected.
  - 2026-11-18 — classroom A/B cleared the recycler of the patches (identical slice 0, 0.58/255
    floor diff); `LUT-CLOSEOUT.md` quarantines the recycler for performance testing until
    correctness is independently established.
  - 2026-11-19 — recorded as `blocked`; the ~3.9 creations/frame in M2 (CPU-03) is the reason
    the idea keeps returning, but the M2 evidence shows no fps gain beyond noise.

### MEM-02 — High-scale memory commitment and stalls

- **Status:** `queued` · **Priority:** P2 · **Last update:** 2026-11-19
- **Measured:** prior 300 % samples reported multi-GB commitment, long frame stalls and
  allocation/protection failures; conditions and builds differ. Current M2 cells hold a steady
  `vram_mb` of **~3,367–3,372 MB** across windows, with no growth observed. The
  `address_space.cpp Protect: Unreachable code!` crash seen in earlier rounds (e.g. two runs lost
  in `PASSES-AND-PACING.md` §4/§6) is the "allocation/protection failure" class referred to; it
  was pre-existing and unrelated to the changes under test, and did not occur in the 2026-11-19
  M2 runs examined here.
- **Measurement semantics:** historic commitment figures come from different builds, render
  percentages and MSAA settings and are not directly comparable to M2.
- **Provenance:** `RENDERSTAT vram_mb`/`vram_blocks` in the 2026-11-19 runs;
  `_build/PASSES-AND-PACING.md` §4, §6 (the `Protect: Unreachable` crash);
  `docs/assignment01/PERF-BASELINE.md` (quarantined items).
- **Dependencies:** overlaps GPU-01 (the 74.65 MB surface) and CPU-03 (allocation churn); a
  candidate explanation for stalls that could otherwise be misattributed to alias copies.
- **Prior experiments / negative results:** none isolated. The historic observations were never
  reproduced under a controlled cell.
- **Safety concerns:** memory pressure can produce silent corruption or device loss; do not
  "fix" it by pre-allocating or caching without an ownership story.
- **Open question:** *Is memory pressure causally contributing to current M2 stalls, independent
  of alias-copy cost?*
- **Next action:** correlate the existing `vram_mb`/`vram_blocks` series and stall markers across
  M2 cells before designing anything.
- **History:**
  - 2026-11-17 — historic multi-GB/stall/protection observations recorded (uncontrolled).
  - 2026-11-19 — M2 steady ~3.37 GB VRAM recorded, with no observed growth or
    `Protect: Unreachable` crash in the examined runs; causality still untested.

### VR-01 — OpenXR presentation and pacing

- **Status:** `blocked` · **Priority:** P2 · **Last update:** 2026-11-19
- **Measured / current state:** delivery/show, wait, handover, `xrEndFrame`, head tracking and
  pose-correction reporting exist in `main`; the recently added dropped/repeated/"never shown"
  frame counters (commit `3278c75`) were compile-checked only. A headless M2 run cannot enter the
  full XR path, and the headset acceptance checklist is **not executed**. In the headless
  configuration `xr_begin_end` has a median period of ~199.6 ms while the render loop reports
  42.8 fps, i.e. **presentation is not in the frame budget at all** when nobody wears the headset
  — every measurement of the XR path taken that way is a property of the unattended path, not of
  the renderer.
- **Provenance:** commit `3278c75`; `docs/integration/HEADSET-ACCEPTANCE.md` (status: not
  executed; §1, §3, §7); `_build/PASSES-AND-PACING.md` §13, §15, §16, §19, §20;
  `src/src/video_core/renderer_vulkan/vk_presenter.cpp`.
- **Dependencies:** independent of the M2 headless FPS work; must not be conflated with it.
  Also the gate for measuring anything above 60 fps, because the title cannot reach gameplay
  headlessly (§20 of `PASSES-AND-PACING.md`).
- **Prior experiments / negative results:** `steady=` / `SHADPS4_VR_STEADY_FPS` and the
  `ad7238d` vblank pacing changes were **rejected** — the governor never acted with the headset
  asleep (Mpx stayed 824–876 and fps 33–37 over 170 s), and `steady=120` was removed from
  `settings.txt`. The launcher's hard-coded 60 Hz vblank reset was a real defect and was fixed.
- **Safety concerns:** presentation owns the zero-copy path; disturbing ownership risks tearing,
  wrong eye content or a device loss. Headset work requires explicit user approval and the
  machine-wide GPU/headset lock.
- **Open question:** *In an actual headset, what portion of frame-to-display latency is
  readiness, copy, compositor or pacing?*
- **Next action:** once a worn-headset session is authorized, decompose frame-to-display latency
  with the existing counters. Do not disturb zero-copy ownership.
- **History:**
  - 2026-11-17 — `steady=`/vblank pacing rejected (governor inactive with the headset asleep).
  - 2026-11-19 — `3278c75` landed the never-shown/repeated frame naming (compile-checked only);
    headset acceptance still not executed; recorded as `blocked`.

### LUT-01 — 3D LUT array/volume conversion and sampling

- **Status:** `closed` (both halves positive; the measurement run was read-only) · **Priority:** P2 correctness · **Last update:** 2026-11-19
- **Measured:** guest **32³ LUT at `0x2008f80000`**, 128 KiB (`0x20000`), tile 19,
  `A2B10G10R10UnormPack32`. Producer: a colour-target pass `32×32×1`, **32 layers (2D array)**,
  `vtx=0x4496c84956245785`. Consumer: fragment shader `0x29090b93e88b0b98`, sampling a 3D image
  at binding 6 with `Fma(31/32, c, 0.5/32)` and carrying the ST 2084 PQ tonemap constants —
  a UE4-style colour-grading LUT. The cache re-creates and copies between the 2D array and the
  3D volume **twice per frame** (`ExpandImage` + `CopyImage`, no persistent dual view);
  ~2 `ExpandImage` events/frame in the old profile, `expand_ms` ≈ **0.08 ms/frame**
  (`3.068 / 36`), 0.25 MB/frame.
- **Current state on `main` @ `314a2cb` (added 2026-11-19).** The ordering half of the question is
  **closed in the positive**: the 2D-array ↔ 3D-volume conversion at this resource is an identity
  mapping over all 32 slices (`src.layerCount = 32`, `dst.layerCount = 1`, `extent = (32,32,32)`,
  `baseArrayLayer = 0` both sides, and `base_depth` taking the 3D side's depth in both directions),
  `SanitizeCopyLayers` fires **no** coercion for this resource (the classroom run's 11
  `Coercing copy` warnings are all same-type array/cube cases, none 2D↔3D at depth), and
  `ExpandImage` refreshes before copying so GPU contents win. Conclusion recorded upstream: the
  conversions "are 1:1 and ordered … not leaving stale or reordered slices".
- **Sampler half measured, and it is also positive (2026-11-19 04:52–04:54).** One headless
  classroom front-end run of `SHADPS4_DBG_SAMPLER=0x2008f80000` on `main` @ `314a2cb` produced
  exactly one line, for this binding: `shader=0x29090b93e88b0b98 stage=Fragment binding=10
  origin=sgpr:48 aniso_off=0 raw0=0xfff00000000092 raw1=0x6500000 clamp_x=2 clamp_y=2 clamp_z=2
  names=ClampLastTexel/ClampLastTexel/ClampLastTexel
  vulkan=ClampToEdge/ClampToEdge/ClampToEdge tex=addr=0x2008f80000 type=Color3D 32x32x32 layers=1
  levels=1 volume=1`. `addressModeW` is therefore **`ClampToEdge`, not `Wrap`** — an out-of-range Z
  lookup clamps to the last slice and never wraps. All three axes are the same
  exactly-representable mode (0 `Unimplemented clamp mode` lines in the run), `origin=sgpr:48`
  matches the consumer's `fs_sampsgpr_48`, and the pairing is the volume itself: the 3D view where
  the third coordinate is the one `addressModeW` transforms. **The wrap/clamp modes are applied
  correctly for this 3D texture**, so the red patches are not explained by them.
- **Measurement semantics:** `ExpandImage` is **not a cost centre** in this title — the cross-fork
  report measured 4.5–5.9 ms total for 116–118 expands in one profile, i.e. ~0.08 ms/frame.
  Do not assign it significant FPS potential without evidence.
- **Provenance:** `docs/assignment01/LUT-SAMPLER-STATE.md` (conversion verdict, diagnostic,
  what is still needed — added by `314a2cb`); `docs/assignment01/LUT-CLOSEOUT.md`;
  `docs/assignment01/3D-VIEW-MIP-LEVELS.md` (related, separate — see below);
  `docs/assignment01/REVISED-CONCLUSIONS.md` addendum (18 Nov);
  `translation-cost-2026-11-19` (`expand_ms`);
  `src/src/video_core/renderer_vulkan/sampler_state_diag.h` (called from `Rasterizer::BindTextures`
  in `vk_rasterizer.cpp`); `src/src/video_core/texture_cache/image.cpp` (`CopyImage` region math
  ~882-892, `SanitizeCopyLayers` ~698-743), `texture_cache.cpp` (`ExpandImage` ~796-808),
  `image_info.cpp`;
  `docs/assignment01/PERF-BASELINE.md` "Established bottlenecks" 4;
  `diag/lut-dump` @ `ae702aa`, `wt-lut-diag`, `wt-lut-bisect` (all experimental, nothing enabled).
- **Dependencies:** shares the alias/re-creation machinery with CPU-03 and GPU-01, but its
  question is correctness, not frame time. It is **not** a substituted performance target.
- **Prior experiments / negative results:**
  - **The investigation is closed without a confirmed root cause or validated fix**, by decision
    after it did not converge. No LUT code is enabled by default and no fix is proposed.
  - The **2D-array ↔ 3D-volume copy chain was verified lossless**: a full dump gave 32/32 unique
    smooth slices, no truncation, duplication or reordering (adjacent-slice diff 0.046).
  - The **recycler is not the cause** of the classroom red patches: OFF vs ON gave byte-identical
    LUT slice 0 and a 0.58/255 floor diff.
  - `SHADPS4_DBG_LUT_AXIS=flip` was built and run: it inverted the scene colours as expected for
    the probe but **did not isolate** the red patches, was stopped by the user and is not a fix.
  - Bisect builds of `543f255` and `9bdf2bf`: the clean side crashed in a later-fixed libc path
    before reaching the classroom, so the comparison never completed.
  - Colour comparison with the diagnostic PNGs was invalidated by an **R/B swap in the dump
    path** — an evidence defect that also affects the benchmark's reference PNG. The classroom
    is warm/sepia, never blue.
  - **`9bdf2bf` ("match small mips and multi-layer subresources") is the only renderer change in
    the golden → `acda49e` window** and is a **hypothesis**, not a confirmed cause.
- **Safety concerns:** a corrupted frame is never a valid performance result; do not optimise the
  LUT by changing its contents; the patches may be the game's intended grade.
- **Open question (answered 2026-11-19):** *"What does the LUT's own sampler state say — the guest
  `clamp_z` and the `addressModeW` it becomes — and does that explain the red patches?"* The answer
  is **no**: `clamp_z` is `ClampLastTexel`, i.e. `ClampToEdge`, on every axis, so there is no wrap
  to get wrong. With 32/32 slices intact and a 1:1 ordered conversion, this resource's contents,
  transport and sampling are all measured clean. If the classroom patches are a defect rather than
  the title's intended warm grade, the cause is **not** here and has to be looked for elsewhere.
- **Next action:** none — the measurement is done and was read-only. **Do not re-run** the closed
  probes unchanged (`SHADPS4_DBG_SAMPLER` for this resource, `SHADPS4_DBG_LUT_AXIS=flip`, the
  `543f255`/`9bdf2bf` bisect).
- **Measurement provenance:** private lab `_run/sampler-diag/lab` (a portable copy of
  `lab/pc-vr-quality` on its own shadow game; `pc-vr/` and the shared game dump untouched), runner
  `_run/sampler-diag/run.ps1`, log `_run/sampler-diag/lab/user/log/shad_log.txt` (12 MB), exe
  SHA-256 `8991510F…`, `PrepareHmdFrame` at 5760x3240 (2880x3240 an eye, `render_percent=300`),
  0 validation errors. Recorded caveat: the run **overlapped the GPU-01 depth-pass campaign**
  (`_out/gpu01-depthpass-2026-11-19`, arms 04:47–05:06), so its timing must not be used as a
  measurement — the state read is unaffected, and the one arm it overlapped (`arm0-s1`) is
  `valid:false` for scene reasons, so no accepted GPU-01 cell is contaminated.
- **Related item recorded on `main`, not a seeded target ID.** `docs/assignment01/3D-VIEW-MIP-LEVELS.md`
  (`314a2cb`) records a **separate, latent** issue: a `Color2D`/`Color2DArray` view over a
  *mipmapped* `Color3D` image asks for more than one level, which
  `VUID-VkImageViewCreateInfo-image-04970` forbids (`IsViewTypeCompatible` answers true at
  `image_view.h:35`, and `ImageViewInfo` never knows the backing is a volume). Status: **recorded,
  not fixed; latent — no title has been shown to hit it.** It is explicitly **not** the 32³ LUT
  (single-mip). It is listed here rather than as a new target ID because it is a correctness item,
  not a performance target, and the seeded queue must stay stable; the fix direction is a clamp to
  one level plus a GPU-free unit test on `ImageViewInfo`.
- **Another related observation, outside this target's scope.** Both the historical classroom log
  and the 2026-11-19 run record **12 `Coercing copy` warnings**, every one the *same-type*
  "source layers 1 and destination layers 2 … to minimum" through "6 … 8" variant
  (`image.cpp:771`) — small array/cube resources, never this 32↔32 resource. A copy coerced **down**
  to the smaller layer count leaves the destination's remaining layers to the refresh, which is a
  plausible stale-layer mechanism for those resources. Recorded here only so it is not lost with
  this thread; it needs its own bounded look and is not part of LUT-01.
- **History:**
  - 2026-11-17 — LUT identified as the UE4 32³ grading LUT; recycler trial produced the red
    overlay that opened the correctness question.
  - 2026-11-18 — full 32-slice dump verified lossless; recycler cleared; R/B dump defect found;
    axis probe built and abandoned; bisect incomplete.
  - 2026-11-18 — investigation **closed without a confirmed cause** (`LUT-CLOSEOUT.md`); nothing
    enabled by default.
  - 2026-11-19 — recorded as `blocked` with an explicit re-entry condition.
  - 2026-11-19 — `314a2cb` landed the `SHADPS4_DBG_SAMPLER` diagnostic and
    `LUT-SAMPLER-STATE.md`: the ordering/copy half of the question closed **in the positive**
    (identity mapping over 32 slices, no coercion for this resource), and the sampler state was
    left as the open half. Status moved to `queued` for that single run.

---

## 7. Recommended first investigation (not started)

**Target: GPU-01**, in the coupled `CPU-02 / CPU-03 / GPU-01` chain.

**Why this one.** It is P0; the copies are **confirmed present in current M2** (≈3.9 copies and
≈139 MB/frame in the selected window); a correct replacement **already exists in `main`** but is
off by default, so this needs no new renderer code and no new instrumentation — only a matched
measurement; it simultaneously labels CPU-02/CPU-03's cost as critical-path or not, and it is the
only member of the chain whose intervention is already integrated and validated for correctness
on a render-target-dump basis.

**Exact falsifiable question.** *In a matched M2 gameplay cell on `main`, does enabling the
already-integrated depth-write representation pass (`SHADPS4_DBG_DEPTH_PASS=1`) remove the
per-frame representation churn — RENDERSTAT `copies`/`copy_mb`/`depth_ms`/`overlap_ms`/`create_ms`/
`destroy_ms`, `XFER_GPU` full-surface copies — and does it change unique rendered FPS beyond the
noise floor?*

**How to bound it.** One binary, one scene, `SHADPS4_DBG_DEPTH_PASS=0` vs `=1`, interleaved
(A–B–A–B), ≥3 repeats per arm, warm-up excluded; unique rendered FPS from the emulator's own
frame-stats series (never compositor cadence); render-target dumps compared
(`mean abs / 255`); VUID/SYNC status recorded; same vblank, render scale and MSAA in both arms.
`docs/AUTONOMOUS_ENGINEERING.md` and `tools/benchmark/` apply.

**Expectation discipline.** The M2 frame is CPU-side bound (command thread ≈23.7 ms against
≈15 ms of GPU execution, GPU ~50–57 % busy). The pass removes GPU-side detile/decompress cost, so
a large FPS gain is **not** implied and must not be projected; a structural removal with little
FPS movement is a legitimate and useful result, and it would redirect effort to the CPU-side
targets. Report whichever happens.

**Then update this ledger** with the run IDs, the arm-by-arm numbers, the dumps and the verdict,
and set GPU-01's status accordingly before selecting the next target.

---

## 8. Closed experiments register — never rediscover unchanged

| Experiment / finding | Verdict |
|---|---|
| `FindImage` cache capacity 1,024 → 4,096 | No meaningful clean-gameplay improvement; **reverted**. Do not retry capacity unchanged. |
| NHL-24-style conditional full-surface preservation skip (`SHADPS4_DBG_NHL_COND=1`) | 48.3 fps (47.9 / 46.7 / 50.2) vs 33.5 baseline, but **incorrect**: skipped copies were the exact contents the next `loadOp=LOAD` depth pass read. **Rejected.** An earlier unsafe skip reached 73 fps and also corrupted rendering. **Do not revive the NHL predicate.** |
| Conservative depth-preservation predicate | Safety maintained (34.7 fps); **no material FPS improvement**. |
| Depth-pass and in-pass-barrier alternatives | Already integrated **opt-in**, both off by default; workload changes but **matched FPS gain unproven** (barrier: +2.6 % on a 400 % front end, not reproduced in gameplay). |
| `SHADPS4_DBG_LUT_AXIS=flip` | Built and run; inverted colours as expected, did not isolate the patches; not a fix. Nothing enabled. |
| `9bdf2bf` LUT bisect (`wt-lut-bisect` / `wt-lut-revert`) | Clean side crashed in a later-fixed libc path; comparison never completed. Experimental only. |
| `CoveringCopyOf` / covered-refresh upload skip (`SHADPS4_DBG_SKIP_COVERED_REFRESH`) | No useful classroom steady-state skip opportunities; predicate cannot be satisfied by an expansion, so it never fires. Off. |
| Historic image recycler (`6170df6` / `4d244d1`) | Potential CPU create/destroy savings; **not** a proven gameplay win, not in the accepted runtime; later local port crashed at startup even with the recycler disabled. |
| `steady=` / `SHADPS4_VR_STEADY_FPS` / `ad7238d` vblank pacing | **Rejected**: the governor never acted with the headset asleep (Mpx 824–876, fps 33–37 over 170 s). |
| `7e90afb` foveation-any | **Rejected as a default**; measuring aid only (matches any ≥2048×1024 pass and changes the picture by design). |
| `render_percent=210` preset inference | **Rejected.** Not integrated. |
| Vulkan submission profiler + labelled pass/dispatch/copy + query reset | **Already integrated** (`5ce2cd0`, `6037acb`, `f2e2831`); do not rebuild or delete. `f2e2831` fixed the timestamp-reset regression. |
| CPU translation step timers | **Already integrated** at `9029c0d`. |
| PR #5453 3D mip-count cap | **Already integrated** in `src/src/video_core/amdgpu/resource.h` (line 248, `Color3D` depth term). Not an open target. |
| Launcher-forced 60 Hz vblank | **Fixed** (real defect): the launcher reset `GPU.vblank_frequency` to 60 on every launch. |
| `633d212` / `fc2fb55` as whole commits | **Rejected as merges** (mixed unrelated work). Useful pieces were reconstructed individually. |
| Pad/Move/hand-pose/touchpad routes to the Déraciné menu | All fail headless: the menu needs a *located* controller on a *worn* headset. See §4.4 and `PASSES-AND-PACING.md` §20. |

---

## 9. Evidence index

**Documentation on `main`**

- `docs/assignment01/PERF-BASELINE.md` — objective (120 unique FPS at ~3036×3036/eye), measurement
  path, trustworthy cells, unestablished noise floor, established bottlenecks, quarantined items.
- `docs/assignment01/REVISED-CONCLUSIONS.md` — headset trial; recycler verdict; 32³ LUT addendum;
  the R/B dump-path defect.
- `docs/assignment01/RECYCLER-REVIEW.md`, `docs/assignment01/LUT-CLOSEOUT.md` — the two closured
  investigations.
- `docs/assignment01/LUT-SAMPLER-STATE.md`, `docs/assignment01/3D-VIEW-MIP-LEVELS.md` — added by
  `314a2cb`: the LUT sampler-state diagnostic and conversion verdict, and the separate latent
  2D-view-over-mipmapped-3D-image VUID issue.
- `docs/assignment01/BENCHMARK-RECONCILIATION.md`, `docs/assignment01/EVIDENCE.md`,
  `docs/assignment01/CANDIDATE-REVIEW.md`, `docs/assignment01/INTEGRATION-PLAN.md` — benchmark
  provenance and guardrail evidence.
- `docs/integration/INTEGRATION-MATRIX.md` — what is integrated, modified, retained or rejected.
- `docs/integration/HEADSET-ACCEPTANCE.md` — the released headset gate (status: not executed).
- `docs/AUTONOMOUS_ENGINEERING.md` — the mandatory contract for benchmark/protected-path work.
- `docs/vr-presentation-resolution.md`, `docs/vr-projection-geometry.md`,
  `docs/deracine-version-compatibility.md` — feature records, not performance targets.

**Evidence on other branches / in the local workspace (not on `main`)**

- `docs/research/cross-fork-renderer.md`, `docs/research/source-digest.md`,
  `docs/research/psvr-portability.md` — on `investigation/cross-fork-renderer` (`e676a43`).
- `_build/PASSES-AND-PACING.md` — rounds 56–69: passes, barriers, the 60 fps front-end lock, the
  400 % GPU-bound A/Bs, VRS, the governor, presentation size.
- `_build/ALIASING-REPORT.md` — the depth/storage representation copy and its replacement.
- `_build/SETUP.md`, `_build/MERGE-EVAL.md`, `_build/AUDIT-STAGES-1-2.md`,
  `_build/INTEGRATION-UNREAL.md`, `_build/PAK-CONFIG-VR-REPORT.md`, `_build/UNREAL-PHASE1.md`.
- `_handoff/2026-11-17/(dead) Framerate Investigation #1.md` — the pre-ledger investigation log
  (NHL condition, preservation skips, the 73/48/35 fps sequence, front-end baselines).
- Run artifacts with `run.json` provenance under
  `wt-frame-time-2026-11-18/_out/`: `translation-cost-2026-11-19`,
  `findimage-profile-2026-11-19-0128`, `image-cache-4096-clean-b1`,
  `image-cache-4096-diagnostic-b1`, `gpu-ops-verify-2026-11-19/*`,
  `query-reset-verify-2026-11-19/gameplay`, `gpu-timing-verify-2026-11-19/*`,
  `profile-baseline-2026-11-18`, `profile-depth-pass-on-2026-11-18`, `clean-a2-2026-11-18`,
  `clean-b2-nodel-20261118-2245-lead-01`.
- Evidence store: `_run/pc-vr-recycler-try/evidence/` (red screenshot, corrected classroom and
  floor crops, LUT contact sheet and 32³ raw dump, shader disassembly, preserved logs).

**Code**

- `src/src/video_core/renderer_vulkan/vk_rasterizer.cpp` — `BindResources`, draw/dispatch path.
- `src/src/video_core/renderer_vulkan/vk_scheduler.{h,cpp}` — `FrameStats::Call`,
  `PassKind`, pass/copy marking, `EndFrame` reporting.
- `src/src/video_core/renderer_vulkan/vk_gpu_timing.cpp` — GPU timestamp intervals.
- `src/src/video_core/renderer_vulkan/sampler_state_diag.h` — `SHADPS4_DBG_SAMPLER` guest
  sampler-state report (default off), called from `Rasterizer::BindTextures`.
- `src/src/video_core/renderer_vulkan/vk_runtime.cpp` — `Runtime::CopyImage` (line 227),
  `CopyImageWithBuffer` (line 325).
- `src/src/video_core/texture_cache/texture_cache.cpp` — `FindImage`, `ResolveOverlap`,
  `ResolveDepthOverlap` (line 310), `ExpandImage`, register/unregister.
- `src/src/video_core/texture_cache/image.cpp` — `CopyImage` (line 810),
  `CopyImageWithBuffer` (line 957), `Transit`, `XFER_GPU` (`ReadCopyTimings`, line 799).
- `src/src/video_core/texture_cache/image_stats.h` — `RENDERSTAT` counters.
- `src/src/video_core/texture_cache/blit_helper.cpp` — `CopyColorToDepth32` and the
  `PassKind::Blit` passes.
- `src/src/video_core/amdgpu/resource.h` — mip-count cap, `Color3D` depth term (line 248).
- `src/tests/` — `test_image_coverage.cpp`, `test_mip_levels.cpp`,
  `test_vr_projection_math.cpp`, plus the benchmark harness tests under `tools/benchmark/tests/`.

**Commits**

| Commit | Subject |
|---|---|
| `314a2cb` | diag(video_core): report the guest sampler state a shader is bound with |
| `9029c0d` | perf(video_core): say where the command thread's translation time goes |
| `f2e2831` | fix(video_core): reset a submission's queries before writing them |
| `6037acb` | perf(video_core): mark the blit helper's passes as copies, not passes |
| `3278c75` | feat(vr): name the frames that were never shown and the displays that repeated |
| `5ce2cd0` | perf(video_core): label and cost the GPU's render passes and dispatches |
| `58346e9` | experiment(video_core): the depth-write copy is opt-in, not the default |
| `56a6f8a` | experiment(video_core): depth-write representation copy (salvaged) |
| `267d200` | perf(video_core): record a barrier for a non-attachment image inside the pass |

---

## 10. Ledger changelog

### 2026-11-19 — created

Created from the handoff inventory seed (`deracine-performance-ledger-handoff.md`) reconciled
against `main`. The seed's verified base was `9029c0d`; `main` advanced to `314a2cb` while this
ledger was being written, and the ledger was rebased onto that tip. **Documentation only, in both
commits:** no renderer change, no new instrumentation, no benchmark run, no game launch and no
experiment was performed for this commit.

**Reconciliation notes (all corrections are explicit):**

1. **Target count.** The assignment referred to 16 target IDs; the seed table actually contains
   **17**: `CPU-01`…`CPU-08`, `GPU-01`…`GPU-05`, `MEM-01`, `MEM-02`, `VR-01`, `LUT-01`. All 17 are
   preserved here; none was dropped to match the stated count.
2. **"Translation time" corrected to `command thread`.** The seed's "28.81 ms/frame translation"
   and "~113 ms CPU translation under validation" are the log's **`command thread`** field, not a
   sum of the translation-step timers. Re-derived: `28.81 − 16.85 (steps) − 2.94 (driver calls)
   = 9.02 ms`, recorded as an **attribution gap**, not a measured unclassified cost.
3. **Source path corrected.** The seed's `amdgpu/resource.h` is
   `src/src/video_core/amdgpu/resource.h`; the PR #5453 `Color3D` depth term is verified there at
   line 248.
4. **Per-frame normalization documented.** `RENDERSTAT` counters are per report window (36 frames
   in the 2026-11-19 M2 runs). Re-deriving CPU-03/CPU-04 from the raw counters gives ≈0.72 ms
   create + ≈2.14 ms destroy per frame at ≈3.9 creations/frame, and 13,639 lookups/frame with
   1,651 slow/frame, of which 49,875/59,437 = **83.91 %** are generation mismatches. The seed's
   0.68 / 1.91 / ~4 and 83.92 % are the same counters in a neighbouring window.
5. **GPU-01 updated with current-M2 evidence.** The representation churn is still present on
   `main` with the depth-write pass off by default: the selected M2 window shows `copies=140` and
   `copy_mb=4992.1` over 36 frames (≈3.9 copies, ≈139 MB per frame) with ≈3.7 ms `depth_ms` and
   ≈3.8 ms `overlap_ms` per frame. The seed's `XFER_GPU` "1.99 ms average" is confirmed
   (1.949–1.991 ms, `copies=148`) and is still an upper bound including queue wait.
6. **M2 baseline framed as CPU-bound.** The gpu-ops cells measure ~34.4–35.5 unique FPS at
   ~1,867 draws with the GPU only ~50–57 % busy and a ~23.7 ms command thread. This is recorded
   because it changes what any GPU-side win can be worth, and it was not explicit in the seed.
7. **GPU-05 source-verified.** `PassKind::Blit` labelling is present (`6037acb`,
   `vk_scheduler.cpp:998`) while `Runtime::CopyImage` (`vk_runtime.cpp:227`) emits no interval
   mark — the seed's "direct `Runtime::CopyImage` still unmarked" is confirmed.
8. **GPU-04 provenance expanded.** "NHL" identified as the **NHL 24 fork's** conditional
   preservation skip (`SHADPS4_DBG_NHL_COND=1`): 48.3 fps (47.9 / 46.7 / 50.2) against 33.5 fps
   baseline, incorrect because each skipped copy was read by a `loadOp=LOAD` depth pass; the
   conservative predicate measured 34.7 fps with no gain. Explicitly marked do-not-revive.
9. **LUT-01 qualified.** The LUT investigation is closed **without a confirmed root cause**; the
   R/B swap is a **dump-path evidence defect** affecting the benchmark reference PNG, and the
   recycler was cleared. Re-entry against the closed probes is gated on new evidence, so the old
   probes must not be re-run unchanged.
10. **VR-01 scope corrected.** It is not only the dropped/repeated counters that are outstanding:
    the whole headset acceptance checklist is **not executed**, and the 120 FPS target is recorded
    there as a target, not a gate.
11. **`main` advanced mid-assignment, and LUT-01 was updated for it.** After the initial fetch
    (`9029c0d`), `main` moved to **`314a2cb`** ("diag(video_core): report the guest sampler state a
    shader is bound with"), which added `sampler_state_diag.h` (`SHADPS4_DBG_SAMPLER`, default off),
    `docs/assignment01/LUT-SAMPLER-STATE.md` and `docs/assignment01/3D-VIEW-MIP-LEVELS.md`. The
    ledger was rebased onto `314a2cb` rather than pushed over it, and LUT-01 was revised:
    - The **ordering/copy half of LUT-01 closed in the positive** — the 2D-array ↔ 3D-volume
      conversion at `0x2008f80000` is an identity mapping over all 32 slices, `SanitizeCopyLayers`
      fires no coercion for that resource, and `ExpandImage` refreshes before copying. The old
      phrasing that slice ordering was unresolved is superseded.
    - The **sampler state is the remaining open half**, now instrumented but unmeasured; LUT-01's
      status moved from `blocked` to `queued` for exactly one read-only diagnostic run
      (`SHADPS4_DBG_SAMPLER=0x2008f80000`). No optimization changed hands.
    - The separate **latent** `VUID-VkImageViewCreateInfo-image-04970` issue (2D/2DArray view over a
      mipmapped 3D image) is recorded under LUT-01 as a **related item, not a new target ID**, so
      the seeded 17-item queue stays stable while the item stays discoverable.

**Not done, by instruction:** no optimization work, no additional benchmarks, no new
instrumentation, no renderer change, and no completed investigation reopened.
