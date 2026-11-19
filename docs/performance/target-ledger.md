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
| `closed` | Accepted synonym for `completed`, used by some records. Same meaning and same freeze. |
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
>
> **The same trap applies to `gpu timeline` and `gpu operations`.** They are printed only when
> **`SHADPS4_GPU_TIMING=1`** (`vk_scheduler.cpp:730`, `:760`, `:850`; the gate lives in
> `vk_gpu_timing.cpp`). `SHADPS4_FRAME_STATS=1` alone prints the `frame stats` line and *not* the
> timeline or the operation attribution — verified 2026-11-19, when a run with `FRAME_STATS=1` and
> no `GPU_TIMING` produced no `gpu timeline` line. Every M2 cell that quotes a command thread, GPU
> execution or pass/dispatch attribution was run with `SHADPS4_GPU_TIMING=1`; none of the M2
> manifests records it. **Record both variables explicitly in the manifest of every new cell.**

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

### 4.5 The game configuration a cell runs, and the Morpheus 120 Hz tweak (kept separate)

**Provenance of the Morpheus 120 Hz tweak — recorded separately from every target.** The
authoritative configuration record for the game's Morpheus/Unreal settings, the frame modes and what
has been measured is **`docs/psvr-morpheus-ue4-settings.md`** (added by another session, with
`tools/tests/morpheus-config-test.ps1` covering the configuration controls). This ledger does **not**
duplicate it; the note below is only the origin of the tweak that the M2 shadow was missing. As the
user corrected on 2026-11-19, `PSVRFrameSequence=Render120Scanout120` is **not part of the game**. It
is a tweak **added by agents during the earlier frametime investigation**, living in the game-config
path `gameproject\saved\config\ps4\engine.ini` of `D:\Temp\Deracine\1.0\Image0`, together with
`bSmoothFrameRate=False`, a smoothed range to 250 and `t.MaxFPS=0`. An earlier draft of this ledger
described it as game-shipped; that was wrong and is withdrawn. It has **no** target ID and belongs to
no target — it is a configuration artifact of the benchmark campaign, recorded here only so its
origin cannot be confused with a game setting or a renderer change. Its properties:

| item | value |
|---|---|
| file | `gameproject\saved\config\ps4\engine.ini` under the game config path |
| origin | added by agents, frametime investigation — **not shipped by the game** |
| key | `[/Script/MorpheusEditor.MorpheusRuntimeSettings] PSVRFrameSequence=Render120Scanout120` |
| also in the file | `bSmoothFrameRate=False`, `SmoothedFrameRateRange` to 250, `t.MaxFPS=0`, `r.ScreenPercentage=300`, `r.FinishCurrentFrame=0` |
| in the M2 shadow used by every earlier cell | **absent** (that shadow's file carries only `r.ScreenPercentage=300` / `r.MSAACount=2`) |
| private reproduction prepared 2026-11-19 | `_out/gpu01-depthpass-2026-11-19/shadow-morpheus120/` — 53 assets hard-linked, `engine.ini` written fresh (`26C43971…`), source shadow untouched (`78C0D8AC…`) |

The emulator log shows every run opening `/app0/gameproject/saved/config/ps4/engine.ini`, i.e. the
**shadow's** copy, so this key reaches the game only if the shadow carries it. Every new arm manifest
now records the engine.ini path and hash (`game_config` in `arm-manifest.json`).

### 4.6 M2 re-baseline on the Morpheus 120 configuration (2026-11-19)

Two cells, same binary as GPU-01 (`8991510F…`), same scene, `SHADPS4_DBG_DEPTH_PASS=0`,
`SHADPS4_DBG_OVERLAP=1` + `SHADPS4_GPU_TIMING=1`, on the private shadow above:

| cell | unique FPS | draws | Mpx | passes | GPU busy | samples |
|---|---|---|---|---|---|---|
| `arm0-b1` | 32.80 | 1,868.3 | 876.8 | 56.7 | 48.9 % | 226 (100 % in band) |
| `arm0-b2` | 34.45 | 1,868.5 | 875.8 | 56.8 | 51.0 % | 211 (100 % in band) |

**Do not read this as a matched A/B against GPU-01's arms.** It differs in configuration (the
Morpheus key) and it is **not** a controlled comparison; the paired arms for GPU-01 ran on the other
shadow, and §3 forbids mixing configurations across compared cells. It records the new configuration's
own cells and its run-to-run spread (**1.65 fps, 4.8 %**), which is wide — wider than the copy arm's
2.3 % spread on the old shadow.

Standalone observations, labelled as such: the same ~71.19 MiB/frame copy is present here
(`copy_mb` 138.03 / 138.56 MiB/frame, `copies` 3.87 / 3.88 — the GPU-05 target exists in this
configuration too), and the Morpheus key **did not raise front-end pacing** in this headless,
headset-off session (menu-band max 55.3 / 57.3 fps here, against 60.6 / 61.7 fps on the old shadow),
which is consistent with `PASSES-AND-PACING.md` §11 — with nobody wearing the headset the XR present
path stalls, so a 120 Hz clock paces the unattended loop *worse*, not better. A worn-headset session
is the only place this key can be judged.

The band in `tools/benchmark/reference/scene-gameplay.json` was derived without this key; both cells
above still satisfied it, so no re-derivation is forced — but a re-derivation must come from live
cells, never projection.

### 4.7 Standing constraints

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
| CPU-03 | Vulkan image create/destroy | `implemented` | P0 | **3.149 ms/frame** (0.852 create + 2.297 destroy), 3.89/frame — all of it the two 74.75 MiB `0x2068000000` shapes | Merged to `main` (source `534e44d0`). Opt-in only: `SHADPS4_DBG_IMAGE_REUSE` default OFF, read at process start. Runtime validation partial — see `image-reuse-pool-validation.md` |
| CPU-04 | `FindImage` slow path / generation | `queued` | P1 | 13,639 lookups/frame, 1,651 slow, 83.9 % generation mismatches | Quantify wasted-but-exclusive work |
| CPU-05 | Vertex/index binding | `queued` | P1 | 3.23 ms/frame over 1,867 calls | Split cache search vs binds vs barriers |
| CPU-06 | Render-state preparation | `queued` | P1 | 2.21 ms/frame over 2,060 calls; ~193 draws skipped after it | Test safe reordering after the empty-draw decision |
| CPU-07 | Driver queue submission | `queued` | P1 | `vkQueueSubmit` 1.77 ms/frame, 3 calls | Determine driver work vs back-pressure vs dependency |
| CPU-08 | Unclassified translation | `queued` | P1 | ~9.0 ms/frame command-thread gap (selected window) | Find the largest missing exclusive cost with existing timers |
| GPU-01 | Depth/scene preservation copies | `completed` | P0 | 2 × 71.19 MiB + 2 × 0.125 MiB copies/frame; pass removes one (GPU exec −4.07 ms) | Closed — no FPS benefit on a CPU-bound frame; a second ~71 MiB copy is unidentified |
| GPU-02 | Guest compute `799062d096fa2314` | `queued` | P1 | **0.21 ms/frame** dispatch — corrected 2026-11-19; the earlier ~5 ms was copy time | Identify shader, geometry, I/O; it is now a *cheap* operation, so re-rank it before investing |
| GPU-03 | Render passes and load/store | `queued` | P1 | ~4.35 ms/frame pass total with copies marked (top pass 1.9 ms); the older 8.44 / 5.9 ms figures were **inflated** by copy time | Re-measure on a copy-marked build, then attribute load/store vs barriers |
| GPU-04 | Depth-pass and barrier-in-pass switches | `queued` | P1 | opt-in, both off by default; matched-workload benefit unproven | Re-test the *safe* options only, in matched M2 |
| GPU-05 | Copy attribution coverage | `completed` | P1 diagnostic | copies now named: 4.0/frame, 142.3 MiB/frame, **6.98 ms/frame** attributed | Closed (`2b665fd`); follow-up is the reverse `D32 → R32` transition, as a new target |
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
  - 2026-11-19 — **cross-reference GPU-01**: the depth-pass A/B measured `depth_ms` 3.85 → 4.14 ms
    and `overlap_ms` 3.97 → 4.26 ms (overlapping ranges) and `images_created` unchanged, so the
    depth pass does **not** remove this work. CPU-02's cost is therefore independent of GPU-01 and
    remains open on its own; if the new ~71 MiB/frame copy turns out to feed the depth alias, these
    two must be re-measured together.

### CPU-03 — Vulkan image creation/destruction

- **Status:** `implemented` · **Priority:** P0 · **Last update:** 2026-11-20
- **Implementation — complete, merged, opt-in.** The scoped two-entry reuse is in `main`, source tree
  `534e44d0`, behind `SHADPS4_DBG_IMAGE_REUSE` which is **off by default** and read once at process
  start (changing it needs a restart). With the switch unset, allocation ownership and rendered
  contents are the unchanged pre-CPU-03 path. Mechanism measured in steady gameplay: `create_ms +
  destroy_ms` 89.25–92.39 → 1.68–24.09 ms/s, 59.6–68.2 pool hits/s, retention bounded at 219.4 MiB
  against the 256 MiB cap, `vram_blocks` flat, clean teardown with allocations still pooled, and no
  rendered-image change attributable to reuse.
- **Runtime validation — opt-in and partial, deliberately not claimed as a win.** Two
  same-executable OFF/ON pairs measured +15.9 % and +14.3 % rendered frame rate, but each pair is one
  OFF and one ON rather than interleaved, the within-configuration spread is ~27 %, and no 100 % or
  200 % cells were run — so the magnitude is unsettled and nothing here shows the feature helps a
  user who leaves it off. No headset acceptance. Full record and its limitations:
  `docs/performance/image-reuse-pool-validation.md`; harness defects found on the way:
  `docs/performance/harness-defects.md`. The unmeasured teardown cleanup `5c9abe34` is excluded from
  `main` and preserved on `perf/image-reuse-teardown-unmeasured`.
- **Experiment:** `cpu03-alloc-classes-2026-11-19`, cell `arm0-a1` (valid gameplay: 32.66 unique FPS,
  1,867 draws, 933 Mpx, 59.8 passes, GPU busy 48.9 %, 173 windows 99.4 % in band), on the existing
  instrumentation only (`SHADPS4_DBG_TRACE_IMAGES=1` + `SHADPS4_DBG_OVERLAP=1` +
  `SHADPS4_GPU_TIMING=1`), binary `597C2609…` whose renderer tree is identical to `main`'s.
  **The classification and the double-counting question are answered; implementation is gated on the
  user's review of the lifetime model.**
- **Measured:** M2 **`create_ms` 0.852 + `destroy_ms` 2.297 = 3.149 ms/frame** (medians over the last
  30 RENDERSTAT windows; windows total 86,041 ns of create and 73.2 ms of destroy per report
  interval), at **`images_created` 3.886 and `images_destroyed` 3.882 per frame**. This is the
  "~3 ms" the question names, and it reproduces the historical pair to 0.01 ms (0.851 / 2.292).
  Earlier windows: ~0.72 + ~2.14 ms; earlier front-end: ~0.78 + 2.5 ms.
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
- **Classification (measured, 2,923 gameplay frames).** Of **12,305 creations** in the gameplay
  span (**4.21/frame**), **95.0 % are four configurations that recur exactly once per frame**; the
  other **620 configurations are one-off or long-lived** scene textures (BC3 blocks, a 1024² R8Unorm,
  980×5 UI strips, icons) at ≤0.001/frame. The four recurring configurations, each **0.999 new AND
  0.999 gone per frame**:

  | guest address | extent | Vulkan format | levels | layers | guest size | initiating operation |
  |---|---|---|---|---|---|---|
  | `0x2068000000`+`0x4ac0000` | 5760×3240 | **`R32Sfloat`** | 1 | 1 | 74.75 MiB | `ResolveDepthOverlap` re-creation (`texture_cache.cpp:360-410`) |
  | `0x2068000000`+`0x4ac0000` | 5760×3240 | **`D32SfloatS8Uint`** (depth) | 1 | 1 | 74.75 MiB | the same re-creation, depth direction |
  | `0x2008f80000`+`0x20000` | 32×32 | `A2B10G10R10UnormPack32` | 1 | **32** (2D array) | 0.125 MiB | `ExpandImage` 2D↔3D conversion (`texture_cache.cpp:796-810`) |
  | `0x2008f80000`+`0x20000` | 32×32 | `A2B10G10R10UnormPack32` | 1 | **1** (3D volume) | 0.125 MiB | the same conversion, the other direction |

  **These are four shapes but only two logical resources**, and they are **the same two resources
  GPU-05's copy log identified** — the create/copy/destroy cycle is one resource alternating between
  two representations. **Lifetime is a median of 0 frames for all four** (max 18 near transitions):
  each is created and destroyed inside the same frame, every frame. Allocation churn is
  **~2 × 74.75 + 2 × 0.125 ≈ 149.8 MiB/frame** by guest size; `RENDERSTAT image_mb` uses the VMA
  allocation size (alignment included) and is larger — do not conflate the two.
- **Which resources carry the 3.149 ms — the `0x2068000000` pair.** Two independent bounds, both
  from existing instrumentation:
  1. **`expand_ms` = 0.099 ms/frame** contains the LUT pair's two creates and two destroys, so that
     pair costs **≤0.1 ms/frame** (~3 % of the churn cost) despite being half the calls.
  2. The historical recycler measured create/destroy going **0.851/2.292 → 0.019/0.008 ms/frame with
     only 2.0 hits/frame** — the two reused shapes carried ~99 % of the cost, which a 128 KiB shape
     cannot plausibly do.
  So the ~3.15 ms is the **2 × 74.75 MiB `0x2068000000` pair**, and it sits inside `depth_ms`
  4.316 / `overlap_ms` 4.453 ms (71–73 % of that window). **Stated uncertainty:** the per-shape split
  is *not* directly instrumented; the two bounds above constrain it but neither names the recycled
  shapes. Splitting it needs either the recycler's own hit accounting or a per-shape timing
  experiment — not a new framework.
- **Double-counting (task 3) — CPU-03 is a sub-cost of CPU-02/CPU-01, never additional.**
  `create_ms`/`destroy_ms` time **only `vmaCreateImage`/`vmaDestroyImage`** (`image.cpp:139-146`; the
  destructors at `:94-100` and `:104-110`). Those calls happen while `ResolveDepthOverlap`,
  `ResolveOverlap` or `ExpandImage` is on the stack, so they fall **inside `depth_ms`/`overlap_ms`/
  `expand_ms`**, which in turn fall inside CPU-01's `bind resources`. The measured relationship
  agrees: 3.149 ms inside 4.316/4.453 ms. **Do not add 3.149 ms to CPU-02 or CPU-01** — removing it
  *reduces* their totals.
- **Recurring or genuinely different (task 4):** recurring-identical, not different resources. Four
  immutable create shapes, re-created once each per frame, median lifetime 0 frames, keyed to a fixed
  guest address and a fixed extent. Nothing about the configuration varies frame to frame.
- **Recycler review (task 5) — reviewed, not ported and not enabled.** `docs/assignment01/RECYCLER-REVIEW.md`
  (source `6170df6`; standalone candidate `4d244d1` cut from `acda49e`, build never run on this
  machine). Invariants mostly sound: key = the whole create shape; the format-list `pNext` is
  excluded from the key and any release with a non-null `pNext` is destroyed rather than pooled;
  nothing *about the contents* is pooled (a recycled image starts at layout `eUndefined`, which
  Vulkan defines as discard); pool bounded at 16 images / 768 MB, measured ~250–293 MB VRAM.
  **Recorded failures and gaps:** the advertised **4.15 → 0.29 ms must not be quoted** — its "off"
  arm was the calibration screen, not the classroom; **no unit test** for the key, eviction or the
  `pNext` guard; **GPU-completion safety is structural, not asserted** (sound only because the sole
  `slot_images.erase` lives inside `DeferOperation`); allocator identity unchecked; `SameShape`
  reads `pQueueFamilyIndices`; `static_vector<Format,4>` capacity; `Acquire` never checks `shutdown`;
  and a stale descriptor would read **recycled pixels instead of faulting**, which makes the
  completion invariant more important rather than less. A later local port
  (`fix/image-recycler-startup-2026-11` @ `248e30b`, not on `origin`) **crashed at startup even with
  the recycler disabled**; cause unproven. **No recycler code was ported or enabled.**
- **Safety assessment for any reuse design.** Required invariants, each with its failure mode:
  (a) the key must be the **entire** create shape, so a recycled image is never handed to a different
  request — a mismatch would alter image contents, which this investigation must not do;
  (b) release must stay on the scheduler's **deferred** path, or the GPU may still be reading the
  image being pooled; (c) the pooled object carries **only** the allocation — validity, hashes,
  `GpuModified`, layout and meta must be freshly initialised, exactly as a new allocation;
  (d) the pool must be **bounded and flushed before `vmaDestroyAllocator`**; (e) destruction must
  remain a real `vmaDestroyImage` for everything not pooled, so a bug leaks rather than corrupts.
  **Preservation semantics are untouched by construction**: reusing an allocation changes *when* the
  memory is obtained, not *what* is copied into it, and the `0x2068000000` preservation logic and the
  reverse depth copy stay exactly as they are (this investigation explicitly does not optimize them).
- **Smallest safe candidate (task 6).** Not the general recycler. The minimal scoped change is a
  **shape-keyed reuse pool restricted to the two `0x2068000000` create shapes with a two-entry
  bound** (one live instance of each shape, ≈149.5 MiB total), with the general eviction policy and
  the 16-slot/768 MB budget dropped. It is the smallest change that addresses the measured 3.149 ms,
  because that cost is those two shapes. An even smaller, lower-risk **first step** exists — pooling
  only the two 128 KiB LUT shapes (≈0.25 MiB bound, a resource with no preservation semantics) —
  which is worth doing only to prove the invariants cheaply before touching the alias pair.
- **Falsifiable optimization hypothesis.** *Reuse-on-release scoped to the two `0x2068000000` shapes,
  with a strict two-entry bound and release only on the deferred path, will reduce `create_ms` +
  `destroy_ms` from 3.149 ms/frame toward ≤0.5 ms/frame and cut `images_created`/`images_destroyed`
  by 2/frame, on a matched M2 cell, with render-target dumps bit-identical and 0 VUID/sync errors and
  VRAM growth bounded by the pool.* **Falsified if** the create/destroy time does not fall materially
  beyond the noise floor, or any render-target dump differs, or VRAM grows beyond the bound, or the
  build fails to start (the historical port's failure mode). **Not tested here** — implementation is
  deliberately deferred for review of the lifetime model.
- **Provenance:** `cpu03-alloc-classes-2026-11-19`/`arm0-a1` (`run.json`, `arm-manifest.json`,
  `lab/user/log/shad_log.txt`), analyzed by `_out/gpu01-depthpass-2026-11-19/alloc-classes.py`;
  the existing `SHADPS4_DBG_TRACE_IMAGES` trace (`texture_cache.cpp:37-56`, `:1327`, `:1343`) and the
  `RENDERSTAT` counters (`image_stats.h`, `create_ns`/`destroy_ns`);
  `src/src/video_core/texture_cache/image.cpp:94-146` for the exact scope of the timers and the
  nesting; `docs/assignment01/RECYCLER-REVIEW.md`; `docs/assignment01/REVISED-CONCLUSIONS.md`
  (recycler verdict); `docs/research/cross-fork-renderer.md` §A2; GPU-05's record for the copy
  identities of the same two resources.
- **Open question:** *Answered for classification and nesting.* Remaining: the **per-shape split of
  the 3.149 ms** (bounded but not directly measured), and whether the two-entry pool caps VRAM as
  expected on a long run.
- **Next action:** **stop here for review.** Present the lifetime model (four shapes / two resources,
  one-frame lifetime, reuse only on the deferred path) and the scoped two-entry candidate; implement
  only after approval, on its own branch, built and started before any claim.
- **History:**
  - 2026-11-17 — front-end ~0.78 + 2.5 ms; recycler prototype measured (MEM-01).
  - 2026-11-19 — M2 window ~0.72 + 2.14 ms per frame over 36 frames; creation classes not yet
    separated.
  - 2026-11-19 — **cross-reference GPU-01**: in the matched A/B, `images_created` 3.91 → 3.90 and
    `images_destroyed` 3.89 → 3.88 are **unchanged** by the depth pass, and `create_ms` 0.72 → 0.86
    / `destroy_ms` 1.76 → 2.49 have overlapping ranges. This confirms the assignment's correction
    (the pass replaces an eligible colour→depth **copy operation**, not the image recreations) and
    means CPU-03 is **not** addressed by GPU-01: its ~4 creations/frame and ~3.3 ms/frame of
    create+destroy remain, and it is now the larger of the two remaining members of the chain.
  - 2026-11-19 — **`cpu03-alloc-classes-2026-11-19` run and analyzed; status → `investigating`.**
    Using only the existing `SHADPS4_DBG_TRACE_IMAGES` trace plus `RENDERSTAT`, the ~3.9 creations
    per frame are classified: **four immutable shapes, two logical resources, each re-created exactly
    once per frame with a median lifetime of 0 frames**, and 95 % of all creations are those four.
    The 3.149 ms/frame of create+destroy is the **`0x2068000000` 2 × 74.75 MiB pair**; the 32³ LUT
    pair is bounded at ≤0.099 ms by `expand_ms`. The cost is nested inside CPU-02's `depth_ms`/
    `overlap_ms` and must not be added to it. The historical recycler was reviewed but **not ported
    or enabled**. A scoped two-entry reuse candidate and a falsifiable hypothesis are recorded;
    **implementation is deferred** for review of the lifetime model.

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

- **Status:** `completed` · **Priority:** P0 · **Last update:** 2026-11-19
- **Experiment:** `gpu01-depthpass-2026-11-19` (matched M2 gameplay A/B, `SHADPS4_DBG_DEPTH_PASS=0`
  vs `=1`). **Verdict: the candidate is active and does exactly what it claims on GPU copy work,
  but it is off the critical path — no unique-FPS benefit in matched M2 gameplay.**
- **Measured:** the depth alias changes `R32_SFLOAT` ↔ `D32_SFLOAT_S8_UINT`; the full surface is
  **74.65 MB** (0x4ac0000) at 5760×3240, **1 sample**, and the guest uses it as both a compute
  storage image and the scene depth/stencil target. Historical conversion copy:
  **3.6–4.9 ms per operation** (`vkCmdCopyImage` R32F → D32S8), versus **0.166 ms** for the
  depth-write pass replacement; ~2 depth representations/frame. Later `XFER_GPU` measured
  **avg 1.949–1.991 ms** per copy (`copies=148`, no drops, no not-ready) — an upper bound that
  includes queue wait.
- **Current M2 state (recorded by this ledger, superseded by the result below):** in the last
  RENDERSTAT window of `translation-cost-2026-11-19` (36 frames) the run showed `copies=140`,
  `copy_mb=4992.1`, `depth_ms=133.5`, `overlap_ms=137.9` — ≈3.9 copies and ≈139 MB of copy traffic
  per frame. **The composition is now measured, not estimated:** two full-surface 71.19 MiB copies
  per frame (two `5760×3240×4` surfaces; `5760×3240×4 = 74,649,600 B = 71.191 MiB`, and
  `2 × 71.191 + 2 × 0.125 = 142.633 MiB/frame`, which the `copy_mb` counter matches), plus two
  0.125 MiB 32³ LUT conversions. `RENDERSTAT copy_mb` is in **MiB** while `_build/ALIASING-REPORT.md`
  quotes the same surface as **74.65 MB** (decimal) — the same quantity, different units; do not
  treat them as different surfaces. **The copies do still occur on `main`**: the depth-write pass
  that replaces one of them is integrated but **off by default** (`SHADPS4_DBG_DEPTH_PASS`, commits
  `56a6f8a` + `58346e9`).
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
- **Result (2026-11-19, `gpu01-depthpass-2026-11-19`).** One binary built from current `main`'s
  renderer content (`314a2cb`; exe `8991510F22CB8D4EC376DB4CA3458C713E4B5E90780FD994A780812B144E635A`,
  whose `src` tree is byte-identical to `origin/main`), one scene
  (`deracine-headless-gameplay-300-msaa2-vblank120`: vblank 120, eye 2880, MSAA 2, 120 refresh/cap,
  save slot 1 verified), identical diagnostics in both arms (`SHADPS4_DBG_OVERLAP=1`,
  `SHADPS4_GPU_TIMING=1`).

  | cell | unique FPS | draws | Mpx | passes | GPU busy | samples |
  |---|---|---|---|---|---|---|
  | `arm0-s4` (`=0`) | 33.94 | 1,866 | 950 | 60.6 | 50.7 % | 225 (98.7 % in band) |
  | `arm0-s5` (`=0`) | 33.16 | 1,866 | 867 | 56.3 | 50.7 % | 227 (100 %) |
  | `arm1-s5` (`=1`) | **32.82** | 1,866 | 917 | 59.0 | **35.0 %** | 207 (99.5 %) |

  Per-frame medians over the last 30 `RENDERSTAT` windows (`arm0-s5` → `arm1-s5`):

  | counter | `=0` | `=1` | reading |
  |---|---|---|---|
  | `copies` (GPU) | 3.91 | **2.93** | one copy/frame removed |
  | `copy_mb` (GPU) | 138.55 MiB | **69.37 MiB** | −69.2 MiB ≈ exactly one 71.19 MiB surface copy |
  | `images_created` (CPU) | 3.91 | 3.90 | **unchanged** |
  | `images_destroyed` (CPU) | 3.89 | 3.88 | **unchanged** |
  | `create_ms` / `destroy_ms` (CPU) | 0.72 / 1.76 | 0.86 / 2.49 | overlapping ranges; no reliable change |
  | `depth_ms` / `overlap_ms` (CPU) | 3.85 / 3.97 | 4.14 / 4.26 | overlapping ranges; no reliable change |
  | GPU executing | 14.50 ms | **10.43 ms** | **−4.07 ms (−28 %)** |
  | command thread | 26.37 ms | 24.82 ms | still the frame's long pole |

  **What the pass does:** it replaces exactly **one** of the two ~71.19 MiB per-frame
  representation copies with the full-screen depth-write pass, cutting that frame's GPU execution
  by 4.07 ms — matching the independently measured 3.6–4.9 ms per copy
  (`_build/ALIASING-REPORT.md`). **What it does not do:** it does not touch image re-creation
  (`images_created`/`images_destroyed` and `create_ms`/`destroy_ms` are unchanged), it does not
  reduce the CPU-side `ResolveDepthOverlap` cost, and it does **not** improve unique rendered FPS
  (33.16 → 32.82, −1.0 %, i.e. inside the copy arm's own 33.16–33.94 spread of 2.3 %), because the
  M2 gameplay frame is CPU-bound: command thread ~25–26 ms against ~10–15 ms of GPU execution.

  This **confirms the 2026-11-17 quarantine verdict** ("$+13.4\%$ then $-7.2\%$, aggregate +2.8 %,
  inside the 10 % noise floor") and **does not reproduce** the +34–40 % headline, now on the
  workload that verdict demanded (a matched gameplay cell). It also confirms the assignment's
  correction: the pass replaces eligible colour→depth copy *operations*, not the image recreations.

- **New finding — one ~71.19 MiB/frame copy remains unidentified.** The copy arm performs **two**
  full-surface 71.19 MiB copies per frame (plus two 0.125 MiB 32³ LUT conversions = 142.6 MiB,
  which the counters match to 0.1 %). The pass is gated on `R32_SFLOAT` → `D32_SFLOAT` /
  `D32_SFLOAT_S8_UINT`, 1 sample, same extent, depth-range-unrestricted, and removes only **one** of
  them. The other ~71 MiB/frame copy is **not** the depth conversion and has no owner in this
  ledger — it is a new, unclaimed target worth ~4 ms of GPU execution per frame.

- **New evidence for GPU-05 (copy attribution).** `gpu operations` differs between the arms in a way
  that names the problem: `=0` reports `pass 8.97 ms x869, dispatch 5.43 ms x213`; `=1` reports
  `pass 9.46 ms x976, dispatch 0.69 ms x238, **copy 0.18 ms x17**`. In the copy arm the ~4 ms
  depth copy is **unmarked** and is charged to the preceding interval (the compute dispatch that
  writes the surface); once the pass marks its work as a copy, that dispatch interval falls to its
  true compute cost. This is a direct, measured demonstration of the mis-attribution GPU-05 exists
  to fix, and it also shows a blit pass does appear as `[copy]` when it is expensive enough to
  reach the costliest list.

- **Measurement-validity findings from this investigation (affect all M2 cells).**
  1. `gpu timeline` (command thread, GPU executing) and `gpu operations` require
     **`SHADPS4_GPU_TIMING=1`**; `SHADPS4_FRAME_STATS` alone prints the frame-stats line only. Any
     cell that reports a command thread was run with that variable set — record it (the earlier M2
     cells did not, the same provenance gap as `SHADPS4_DBG_CALL_TIMES`).
  2. `SHADPS4_DBG_CALL_TIMES=1` **plus** `SHADPS4_DBG_XFER=1` collapse M2 gameplay to **0.6 fps**
     (one frame per stats window), which makes the gameplay gate unsatisfiable. The two flags are
     not usable together in this scenario; §8 records the excluded pair.
  3. `SHADPS4_DBG_DUMP_HMD_AT_SCENE` **does not latch**. One `-EyeDump` run fired the scene gate
     **75 times**, dumping 26–27 full-resolution render targets per capture, and collapsed gameplay
     to 0.6 fps. It must not be enabled on a cell whose performance numbers matter until the dump
     gate is fixed (a harness change, not done here).
  4. The gameplay gate's failure text can mislead: with one frame per window it reports "no window
     reached 800 draws (peak 3,074)" even though heavy windows were seen. Read the draw series, not
     the message.
  5. Environment instability cost four arm-runs: the pre-existing
     `address_space.cpp:568 Protect: Unreachable code!` crash and one `0xc0000005` access violation
     at `0x800dfa8d0` (both hit the `=1` arm **before** gameplay, at 26 draws with `depth_ms`
     ≈ 0.015, i.e. with the depth pass never engaged), and one startup death with no log at all.
     **None is attributable to the depth pass**, because the pass was inert at those points; they
     are recorded as scene/session instability and are the reason arm-level retries are required.

- **Open questions (now narrower):**
  - *What is the second ~71.19 MiB/frame copy, and what is it for?* (New; the pass does not cover it.)
  - *Why is only one of the two per-frame conversions eligible for the pass?* (Gate conditions are
    exact; which one fails is unmeasured.)
  - *Does the pass help once the CPU side is fixed?* It removes real GPU work (14.50 → 10.43 ms); if
     the command thread ever stops being the limiter, that work would matter again.
- **Next action:** **none for this target — it is closed.** Its follow-ups are: (a) give the
  unidentified second ~71 MiB/frame copy an owner (a new target, or a GPU-05 follow-up, since
  naming it needs the direct-copy markers); (b) fix the dump gate so visual evidence no longer
  invalidates a cell; (c) record `SHADPS4_GPU_TIMING` in every future manifest. Do **not** re-run
  this A/B unchanged — the question is answered, and the same configuration cannot answer it
  differently.
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
  - 2026-11-19 — **`gpu01-depthpass-2026-11-19` run and closed.** Five arm-runs across four pairs
    (the first three pairs were invalidated by the diagnostic set and by harness/environment
    problems, all recorded above and in §8); the final pair produced two valid cells (33.16 / 32.82).
    Verdict: pass active, one 71.19 MiB copy/frame removed, GPU execution −4.07 ms/frame, **no
    unique-FPS benefit** on a CPU-bound frame. Status `queued` → `completed`; a second
    unidentified ~71 MiB/frame copy found; GPU-05's mis-attribution demonstrated directly.

### GPU-02 — Guest compute shader `799062d096fa2314`

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **✔ Attribution resolved (2026-11-19) — the ~5 ms was copy time, not shader cost.** With the
  copies given their own GPU interval by `2b665fd`, the same shader reports
  **`0.21 ms/frame` (`0.21 ms x152 [dispatch] dispatch shader 799062d096fa2314`)** in a matched M2
  cell (`arm0-g1`, 36.37 unique FPS), against the ~5 ms previously charged to its interval. The
  earlier suspicion (recorded below, from the user, after GPU-01's A/B) is confirmed by measurement.
  **Use 0.21 ms/frame; do not carry the ~5 ms figure forward.** All figures below are retained only
  as the record of the mis-attribution.
- **⚠ (Superseded) Attribution was suspect — do not quote the ~5 ms as exclusive shader execution cost.**
  Recorded 2026-11-19 by the user after GPU-01's A/B. The aggregate dispatch time attributed to
  this shader fell from **5.43 ms/frame to 0.69 ms/frame** when the depth-write pass replaced an
  **unmarked** preservation copy (`arm0-s5` → `arm1-s5`), with the intervening copy itself measured
  at only `0.18 ms`. The shader did not get cheaper; the interval it was charged for changed.
  Because a copy is recorded between two marks and therefore falls **inside the interval before it**,
  the earlier "~5 ms of shader `799062d096fa2314`" plausibly **includes preservation-copy time**.
  The number is not an exclusive shader execution cost until it is independently measured (which is
  GPU-05's parameter work plus a clean interval). Treat the figures below as an **upper bound of
  unknown composition**, not as shader cost.
- **Measured (composition unproven, see the warning above):** **8 dispatches/frame**
  (`x176`–`x256` over 22–30 frame windows), aggregate **4.86–5.45 ms/frame** in the M2
  heavy-diagnostic run; the seed records **~5.0–5.75 ms/frame** across other windows. It is the
  most expensive dispatch in the copy-path run and the second most expensive GPU interval after the
  main scene pass. Dramatically scene-dependent; SPIR-V is cached. UV scaling/offset identifiers
  suggest a resampling/warp pass but do **not** prove it.
- **Provenance:** `translation-cost-2026-11-19` (`gpu operations` costliest list);
  `query-reset-verify-2026-11-19/gameplay` (`dispatch 6.18 ms x80` and `5.32 ms x72` for the
  same shader — scene-dependent); **`arm0-s5` vs `arm1-s5` (2026-11-19) for the attribution change
  that invalidates the exclusive reading**.
- **Dependencies:** a guest workload; separate from the depth/resource chain. Informs GPU-03, and
  now **depends on GPU-05** for a trustworthy interval attribution.
- **Prior experiments / negative results:** none. No removal has been attempted or proposed.
- **Safety concerns:** this is guest code. Removing or altering it changes the picture; there is
  no evidence yet that it is off the critical path or redundant.
- **Open question:** *What is this shader, what is its dispatch geometry, what are its inputs and
  outputs, and does it lie on the frame critical path?* **Answer only after GPU-05 gives this
  interval an exclusive attribution.**
- **Next action:** identify the shader (disassembly, resource bindings, dispatch geometry from a
  capture) **without** speculative removal. Do **not** plan work against the ~5 ms figure until its
  composition is settled — and do not re-attribute it by guessing; wait for GPU-05's measured
  parameters.
- **History:**
  - 2026-11-19 — cost measured in the M2 window (4.86–5.45 ms/frame at 8 dispatches/frame);
    purpose still unknown.
  - 2026-11-19 — **attribution invalidated as an exclusive shader cost**: the same interval fell to
    0.69 ms when the depth-write pass replaced an unmarked preservation copy (5.43 → 0.69 ms,
    copy 0.18 ms), so the figure includes copy time. Recorded at the user's instruction before any
    further optimization is chosen. Cross-reference: GPU-05 (parameter work), GPU-01 (the evidence).

### GPU-03 — Render passes and load/store behaviour

- **Status:** `queued` · **Priority:** P1 · **Last update:** 2026-11-19
- **⚠ Pass figures measured without copy marks — likely inflated. Re-measure before planning.**
  Recorded 2026-11-19 from GPU-05's cell: with `Image::CopyImage` given its own interval, the same
  scene reports a pass total of **~4.35 ms/frame** with the top pass at **1.9 ms**, against the
  "~8.24 ms pass total / ~5.9 ms heaviest pass" below, which were measured while the unmarked
  ~7 ms/frame of representation copies were being charged to the intervals around them. The figures
  below are an **upper bound of mixed composition** until re-measured on a copy-marked build.
- **Measured (composition unproven, see the warning above):** historical profiler pass total
  **~8.24 ms/frame**; heaviest individual pass **~5.9 ms/frame**. Confirmed in the M2 window as pass
  attribution **8.44 ms/frame** (`x1856`) with heaviest pass **5.85 ms ×32** — `5760×3240`,
  `1 colour + depth`, vs `72abf67b5ff30cc3`, ps `49d2380cb717a18e`. The full M2 pass mix is
  ~58 passes/frame, **57 of which load their targets** and ~909 Mpx of target writes.
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
  - 2026-11-19 — **cross-reference GPU-01**: its depth-pass half is now measured in matched M2
    gameplay — one 71.19 MiB copy/frame removed, GPU execution −4.07 ms/frame, **no unique-FPS
    benefit** (33.16 → 32.82 on a CPU-bound frame). The pass is therefore **not** promotable on
    frame-rate grounds, which closes the "unproven in a matched workload" question in the negative.
    GPU-04's remaining open half is the in-pass barrier only; do not re-test the depth-pass half
    unchanged.

### GPU-05 — Copy attribution coverage

- **Status:** `completed` · **Priority:** P1 diagnostic · **Last update:** 2026-11-19
- **Experiment:** `gpu05-copy-attribution-2026-11-19`, cell `arm0-g1`, on attribution commit
  `2b665fd` (binary `597C260970C79A33B5A02C9DF34906DE8C2C7AB183C4416FE48ADFDADA192154`, same scene
  as GPU-01, `SHADPS4_DBG_DEPTH_PASS=0`). **Verdict: the remaining ~71.19 MiB/frame copy is the
  reverse `D32SfloatS8Uint → R32Sfloat` representation transition on guest `0x2068000000`, issued at
  `texture_cache.cpp:407` once per frame.** The forward direction is the one the depth pass replaces,
  because that pass is gated colour-into-depth; the reverse is not covered by that gate.
- **Method (no new profiler).** The existing attribution was extended:
  `Scheduler::MarkCopy(width, height, bytes)` mirrors `MarkDispatch` and marks `GpuTiming::Op::Copy`
  with the extent and byte count the `OpLabel` already documents for a copy; it is called at the copy
  command itself, so no command is reordered and no pass is split for a timestamp. `Image::CopyImage`
  takes an optional `std::source_location` defaulting to the caller and, with
  `SHADPS4_DBG_COPY_LOG=1`, logs one CPU-side line per copy naming both images, the byte count and
  the execution site. The `SHADPS4_DBG_XFER` timestamp pair is untouched.
- **Result — every copy in the gameplay span (3,323 frames, 179 windows), per frame:**

  | per frame | bytes | site | src → dst |
  |---|---|---|---|
  | 0.999 | 71.191 MiB | `texture_cache.cpp:407` | `0x2068000000` Color2D `R32Sfloat` 5760×3240×1 → `D32SfloatS8Uint` 5760×3240×1 |
  | **0.999** | **71.191 MiB** | **`texture_cache.cpp:407`** | **`0x2068000000` Color2D `D32SfloatS8Uint` 5760×3240×1 → `R32Sfloat` 5760×3240×1** |
  | 0.999 | 128 KiB | `texture_cache.cpp:807` | `0x2008f80000` Color2D ↔ Color3D (`A2B10G10R10UnormPack32`, 32×32×1 ↔ 32×32×32) |
  | 0.999 | 128 KiB | `texture_cache.cpp:807` | the reverse of the LUT pair above |

  Totals: **4.0 copies and 142.3 MiB of full-surface traffic per frame**, matching the `RENDERSTAT`
  counters to ~0.1 % (`copies` 3.89 median, `copy_mb` 138.88 MiB/frame median). Cell: valid gameplay,
  unique FPS 36.37, 1,866 draws, 932 Mpx, 59.8 passes, GPU busy 46.3 %, 179 windows 99.4 % in band.
- **What the copy actually costs, now attributed.** With the copies marked, `gpu operations` reports
  **`copy 6.98 ms/frame` as the single largest attributed item** (costliest row
  `6.98 ms x38 [copy] copy 5760x3240`), i.e. ~3.3–3.5 ms per full-surface copy — the same order as
  the 3.6–4.9 ms measured independently in `_build/ALIASING-REPORT.md`. Attributing them removed
  7 ms/frame from the intervals that were previously charged for them.
- **Two attributions this corrects (cross-references).**
  - **GPU-02:** the guest compute shader `799062d096fa2314` now reports **0.21 ms/frame**
    (`0.21 ms x152 [dispatch]`), not ~5 ms. The suspicion recorded earlier is confirmed: that figure
    was copy time.
  - **GPU-03:** the pass total in this cell is ~4.35 ms/frame with the top pass at 1.9 ms — far below
    the "~8.24 ms pass total / ~5.9 ms heaviest pass" measured without copy marks. GPU-03's numbers
    are therefore **inflated by the same mis-attribution** and must be re-measured before any work is
    planned against them.
- **Open follow-up (new, no owner):** the reverse transition is *required for correctness* (it
  preserves the contents the next colour consumer reads), so the question is not whether to skip it
  but whether it can be made cheaper — e.g. the mirror of the depth-write pass (depth→colour instead
  of colour→depth). That is a new investigation, not part of GPU-05, and it needs its own
  falsifiable question and matched A/B before anything is proposed.
- **History:**
  - 2026-11-19 — `PassKind::Blit` labelling landed (`6037acb`); no `[copy]` row in the costliest list
    of the tested scene; direct `Runtime::CopyImage` confirmed unmarked in source.
  - 2026-11-19 — promoted to the preferred next investigation after GPU-01 closed, on the strength of
    the dispatch interval collapsing (5.43 → 0.69 ms) when one unmarked copy was replaced.
  - 2026-11-19 — **extension implemented and merged as `2b665fd`**, and one valid cell named every
    copy in the gameplay span. The remaining copy is identified; the target is closed. The same run
    corrected GPU-02 to 0.21 ms/frame and showed GPU-03's pass figures to be inflated.
- **Deterministic answer sought:** for the remaining ~71.19 MiB/frame copy — src and dst guest
  addresses, formats, extent, aspect, mip/layer, and the execution site (which caller, and whether
  it is the forward or the reverse representation transition).
- **Provenance:** `src/src/video_core/renderer_vulkan/vk_runtime.cpp:227`,
  `src/src/video_core/texture_cache/blit_helper.cpp:100,211,324,463`,
  `src/src/video_core/renderer_vulkan/vk_scheduler.cpp:998`, `vk_scheduler.h` (`PassKind`);
  commit `6037acb`; `translation-cost-2026-11-19` (`gpu operations` has no `[copy]` row).
- **Dependencies:** informs GPU-01 and GPU-03; its absence is why GPU-01's exact copy split is
  currently unavailable.
- **Prior experiments / negative results:** one earlier attempt at labelling blits in `main` produced
  zero `[copy]` rows in the tested scene's costliest list — which read as "no copies" but only meant
  the copies were too cheap to reach that list. That misreading is why this target existed.
- **Safety concerns:** none; measurement only. Do not split render passes merely to get timestamps,
  and do not reorder a copy to place a marker.
- **Open question:** answered — see the result above. The follow-up it produced (making the reverse
  transition cheaper) is a **new** investigation with its own question, not a reopening of this one.
- **Next action:** **none — this target is closed.** Its follow-ups: (a) a new target for the reverse
  `D32SfloatS8Uint → R32Sfloat` transition, if the project wants it; (b) re-measure **GPU-03** with
  copies marked; (c) keep `SHADPS4_DBG_COPY_LOG` and `Scheduler::MarkCopy` in the tree — they are the
  attribution that makes this class of error visible. Do not repeat this investigation unchanged.

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
- **New evidence, recorded 2026-11-19 from the user's own observation.** The user watched a
  classroom render with **no red patches** minutes into the GPU-01 campaign, and reports it as
  "fixed during that benchmark run, but we can't reproduce it otherwise". The benchmark build is
  `main` @ `314a2cb` (`8991510F…`, deliberately hash-pinned in
  `_out/gpu01-depthpass-2026-11-19/run-arm.ps1`). Every other binary on this machine is a **17 Nov
  build of the older cross-fork-renderer line**: `DeracinePCVR/pc-vr/shadps4.exe` = `E6A5A165`
  (17 Nov 15:59 — the recovery audit's *experimental* install, not an approved known-good),
  `lab/pc-vr-quality/shadps4.exe` = `C17BBC8A` (06:14), and
  `_run/pc-vr-recycler-try/shadps4.exe` = `A02FB23F` (20:03, the lab that produced the red-patch
  evidence). **Hypothesis, not proven:** the patches are a property of that older renderer rather
  than of this resource — `f6510f1` (`integration/cross-fork-renderer`) and `main` differ by
  **+2562/−1881 lines in `src/src/video_core` alone** (the image recycler, foveation and the
  alias/preserve tracing are gone on `main`; blit helper, texture cache, presenter and scheduler
  are reworked). The depth-write pass is **not** the difference: `CopyColorToDepth32` is present in
  `f6510f1` *and* `main`, and absent only from the patch-free golden `1d2f393`. Settling it needs
  one same-lab A/B across the two binaries — a run plus a visual check the user owns — or a
  comparison of the captures that already exist; neither has been done, and neither belongs to
  LUT-01.
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

## 7. First investigation (executed — GPU-01 closed 2026-11-19)

> **Status: done.** This was the recommended first investigation and it is now closed as
> `gpu01-depthpass-2026-11-19`; see GPU-01's record for the measured verdict (pass active, one
> 71.19 MiB copy/frame removed, GPU execution −4.07 ms/frame, **no unique-FPS benefit** on a
> CPU-bound frame). No next target has been selected — that follows from these results, and the
> outstanding configuration change (§4.5) is a separate piece of work. The text below is kept as the
> record of what was proposed and why.

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
| `SHADPS4_DBG_CALL_TIMES=1` **together with** `SHADPS4_DBG_XFER=1` on an M2 gameplay cell | **Collapses the frame to 0.6 fps** (one frame per stats window), so the gameplay gate cannot be satisfied and no valid cell is possible. Identical in both arms, so it invalidated the whole pair rather than biasing one. Do not combine them when the cell's performance matters. |
| `SHADPS4_DBG_DUMP_HMD_AT_SCENE` (`-EyeDump`) on a measuring cell | **Does not latch.** One run fired the scene gate **75 times**, dumping 26–27 full-resolution render targets per capture; gameplay fell to 0.6 fps and the cell was invalid. Use it only when the performance numbers are being discarded, until the dump gate is fixed. |
| `SHADPS4_DBG_DEPTH_PASS=1` as a frame-rate optimization for M2 gameplay | **Measured 2026-11-19 (`gpu01-depthpass-2026-11-19`), rejected on frame-rate grounds**: it is real work removal (−71.19 MiB/frame of copies, −4.07 ms/frame of GPU execution) but unique FPS went 33.16 → 32.82 on a CPU-bound frame, inside the ±2.3 % copy-arm spread. Not a promotion candidate; do not re-run the A/B unchanged. (The pass itself is safe and integrated; only the *frame-rate* claim is rejected.) |

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
- **GPU-01 A/B (2026-11-19):** `_out/gpu01-depthpass-2026-11-19/` — `run-arm.ps1` (private arm
  wrapper; the tracked `tools/benchmark/headless-gameplay.ps1` is never edited), `pair-s4.sh` /
  `pair-s5.sh`, `analyze.py`, `compare-ab.py`, `compose-check.py`, `visual-compare.py`,
  `make-morpheus-shadow.ps1`, and per-arm `arm<N>-s<N>/` directories each containing `run.json`
  (harness verdict), `arm-manifest.json` (arm variable, shared diagnostics, exe hash and its source
  tree, runner hash, eboot hash, exact command) and `lab/user/log/shad_log.txt`. The private
  executable used by every arm is `_out/gpu01-depthpass-2026-11-19/bin/shadPS4.exe`,
  `8991510F22CB8D4EC376DB4CA3458C713E4B5E90780FD994A780812B144E635A`.
- **GPU-05 copy attribution (2026-11-19):** `_out/gpu01-depthpass-2026-11-19/copy-sites.py`
  (groups the `SHADPS4_DBG_COPY_LOG` lines by identity and per-frame rate), `gpu05-cell.sh`, and the
  cell `arm0-g1/` (`run.json`, `arm-manifest.json`, `arm0-g1.copies.json`,
  `lab/user/log/shad_log.txt`). Private binary `bin/shadPS4-gpu05.exe`,
  `597C260970C79A33B5A02C9DF34906DE8C2C7AB183C4416FE48ADFDADA192154`, built in `wt-gpu05` on branch
  `investigation/gpu05-copy-attribution`, merged as `2b665fd`.
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
| `2b665fd` | perf(video_core): attribute the emulator's own image copies to a copy interval |
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

### 2026-11-19 — GPU-01 investigation (`gpu01-depthpass-2026-11-19`)

Executed the ledger's recommended first investigation and closed GPU-01. Renderer code was **not**
modified, no instrumentation was added, and the unsafe NHL predicate was not revived.

**What was run.** A matched M2 gameplay A/B of the already-integrated opt-in depth-write pass
(`SHADPS4_DBG_DEPTH_PASS=0` vs `=1`) on one executable built from current `main`'s renderer
content, one scene, one lab template, with identical diagnostics in both arms
(`SHADPS4_DBG_OVERLAP=1`, `SHADPS4_GPU_TIMING=1`) and the runner's own `SHADPS4_FRAME_STATS=1`.

**Result.** The pass is active and real, and it is off the critical path:

- one full-surface **71.19 MiB copy/frame removed** (`copies` 3.91 → 2.93, `copy_mb` 138.55 →
  69.37 MiB/frame);
- **GPU execution 14.50 → 10.43 ms/frame (−4.07 ms)**, GPU busy 50.7 → 35.0 %, matching the
  independently measured 3.6–4.9 ms per copy;
- **image re-creation unchanged** (`images_created` 3.91 → 3.90, `images_destroyed` 3.89 → 3.88,
  `create_ms`/`destroy_ms`/`depth_ms`/`overlap_ms` all overlapping);
- **unique rendered FPS 33.16 → 32.82 (−1.0 %)**, inside the copy arm's own 33.16–33.94 spread
  (2.3 %). The frame is CPU-bound: command thread 26.37 / 24.82 ms against 14.50 / 10.43 ms of GPU
  execution.

This confirms the 2026-11-17 quarantine verdict (+13.4 % then −7.2 %, aggregate +2.8 % inside a
10 % noise floor) and does **not** reproduce the +34–40 % headline, now on the matched gameplay
workload that verdict demanded.

**New findings recorded.** (1) A second **~71.19 MiB/frame copy is unidentified** and untouched by
the pass — a new, unclaimed target. (2) Direct measured evidence for GPU-05: in the copy arm the
~4 ms copy is unmarked and charged to the preceding dispatch interval (`dispatch 5.43 ms`), and once
the pass marks its work as a copy the interval falls to its true cost (`dispatch 0.69 ms, copy
0.18 ms`). (3) `gpu timeline`/`gpu operations` require `SHADPS4_GPU_TIMING=1`, not `FRAME_STATS`
alone — a provenance correction now in §4.2. (4) Three measurement-validity traps are in §8: the
`CALL_TIMES` + `XFER` pair collapses gameplay to 0.6 fps; the HMD dump gate does not latch (75
captures in one run); and the gate's failure text misreports peak draws when windows hold one frame.
(5) The pre-existing `address_space.cpp:568 Protect: Unreachable code!` crash and one
`0xc0000005` access violation cost four arm-runs; both hit **before** gameplay with `depth_ms`
≈ 0.015, so neither is attributable to the depth pass.

**Kept honest.** Three of five arm-runs were invalid for reasons unrelated to the arm variable
(diagnostic cost, harness dump behaviour, environment crashes); only the last pair produced two
valid cells, and the copy arm has two samples while the pass arm has one. The verdict rests on the
mechanism being unambiguous (the counters move exactly as the code predicts) plus an FPS difference
that is inside the measured spread — not on a single lucky cell.

### 2026-11-19 — configuration correction and pending re-baseline

The engine.ini that `D:\Temp\Deracine\1.0\Image0\...\ps4\` carries (Morpheus
`PSVRFrameSequence=Render120Scanout120`, `t.MaxFPS=0`, smoothing off) is **an artifact added by the
earlier frametime investigation, not part of the game** — corrected here after an earlier draft of
this ledger described it as game-shipped. The M2 shadow in use does not carry it, so every M2 cell
in this ledger ran without the 120 Hz PSVR frame sequence. A private shadow reproducing the tweak is
prepared (§4.5) and **re-baselining M2 on it is a separate, deliberate change**, per the user's
decision — not folded into GPU-01's result and not comparable with the cells above.

### 2026-11-19 — GPU-01 accepted; GPU-02 attribution invalidated; GPU-05 promoted

GPU-01's measured verdict was accepted **for the tested configuration**, and its evidence and
negative FPS result stay in this ledger. Two consequences were recorded at the user's instruction
before any further optimization is chosen:

1. **GPU-02's attribution is suspect.** The dispatch interval attributed to shader
   `799062d096fa2314` fell from 5.43 to 0.69 ms/frame when the depth-write pass replaced an
   **unmarked** preservation copy, while the copy itself measured 0.18 ms. The earlier ~5 ms figure
   therefore plausibly includes preservation-copy time and **must not be treated as exclusive shader
   execution cost** until independently measured. GPU-02's record now carries that warning, and its
   next action waits on GPU-05.
2. **GPU-05 is the preferred next investigation.** One ~71.19 MiB/frame copy remains unidentified.
   Direction: review `Image::CopyImage`, `Runtime::CopyImage` and the depth/storage alias transition;
   the reverse `D32 → R32` transition is a plausible source but its identity must be established from
   the **actual copy parameters and execution site**, not assumed. Constraints: **no new profiler** —
   extend the existing copy attribution, preserving command order and valid GPU timing; no
   speculative image-recycler changes; do not repeat the completed depth-pass A/B.

**Configuration work recorded separately** (see §4.5, §4.6):

- The Morpheus 120 Hz `Engine.ini` tweak's **provenance is now its own record** and is explicitly
  *not* a game setting, *not* a renderer change, and *not* owned by any target.
- The M2 re-baseline on that configuration produced two valid cells (**32.80 / 34.45 FPS**, spread
  4.8 %) and is recorded as **its own configuration** — it is **not** a matched A/B against GPU-01's
  arms and must never be pooled with them. The ~71.19 MiB/frame copy is present in it as well.
- Every new arm manifest records the game `engine.ini` path and hash, closing the gap that let a
  cell's configuration go unrecorded.

**Sequencing honoured:** GPU-05 was **not** started before the re-baseline was recorded. It begins
now, alone.

### 2026-11-19 — GPU-05 investigated and closed (`gpu05-copy-attribution-2026-11-19`)

Executed GPU-05 alone, after the re-baseline above was recorded. No new profiler was built, no
recycler change was made, and the completed depth-pass A/B was not repeated.

**The attribution was extended, not replaced.** `Scheduler::MarkCopy` mirrors `MarkDispatch` and
marks `GpuTiming::Op::Copy` with the extent and bytes the `OpLabel` already documents for a copy,
called at the copy command itself so no command is reordered and no pass is split for a timestamp.
`Image::CopyImage` takes a `std::source_location` defaulting to the caller and, with
`SHADPS4_DBG_COPY_LOG=1`, logs one CPU-side line per copy with both images' parameters and its
execution site. `SHADPS4_DBG_XFER` is untouched. Landed as **`2b665fd`** after a valid M2 cell.

**The question is answered.** Over the gameplay span (3,323 frames), the four copies per frame are:
two 71.191 MiB copies that are the *same* representation pair on guest `0x2068000000`, 5760×3240,
1 sample — `R32Sfloat → D32SfloatS8Uint` and its reverse, each 0.999/frame, both at
`texture_cache.cpp:407` — and two 128 KiB LUT conversions at `texture_cache.cpp:807`. Totals,
4.0 copies and 142.3 MiB/frame, match `RENDERSTAT` to ~0.1 %.

**The remaining ~71.19 MiB/frame copy is the reverse `D32SfloatS8Uint → R32Sfloat` transition.** The
depth pass covers the forward direction only, because its gate is colour-into-depth. The user's
hypothesis was right, and it is now established from the copy parameters and execution site rather
than assumed.

**Two attributions corrected, both cross-referenced:**

- **GPU-02** — shader `799062d096fa2314` measures **0.21 ms/frame**, not ~5 ms. The earlier figure
  was the preservation copy.
- **GPU-03** — pass figures drop to ~4.35 ms/frame with a 1.9 ms top pass once copies are marked, so
  the recorded 8.44 / 5.9 ms were inflated by the same mis-attribution and must be re-measured.

The copy interval is now the largest single attributed GPU item at **6.98 ms/frame** — roughly
3.3–3.5 ms per full-surface copy, the same order as the 3.6–4.9 ms measured independently in
`_build/ALIASING-REPORT.md`. **No optimization was attempted:** the reverse transition is required
for correctness, and making it cheaper is a new investigation with its own question, not part of
GPU-05. No next target has been selected.

### 2026-11-19 — CPU-03 investigated (`cpu03-alloc-classes-2026-11-19`); implementation deferred

Selected by the user as the next ledger investigation and marked `investigating`. One valid M2 cell
(`arm0-a1`, 32.66 unique FPS, 1,866 draws, 173 windows 99.4 % in band) was measured with the
**existing** instrumentation only — `SHADPS4_DBG_TRACE_IMAGES=1` for per-image identity, `RENDERSTAT`
for cost — and analyzed by the new read-only `alloc-classes.py`. No new profiling framework was
added.

**Answered.** The ~3.9 creations and destructions per frame are **four immutable create shapes
belonging to two logical resources**, each re-created exactly once per frame with a **median
lifetime of 0 frames**, and 95 % of every creation in the gameplay span is one of those four: the
`0x2068000000` 5760×3240 `R32Sfloat`/`D32SfloatS8Uint` pair (74.75 MiB each) and the `0x2008f80000`
32×32 LUT's 2D-array/3D-volume pair (0.125 MiB each). They are the same two resources GPU-05's copy
log named. The **3.149 ms/frame** of `create_ms` + `destroy_ms` is the **`0x2068000000` pair**; the
LUT pair is bounded at **≤0.099 ms** because `expand_ms` contains its two creates and two destroys.
The cost is **nested inside CPU-02's `depth_ms`/`overlap_ms`** (measured: 3.149 ms inside
4.316/4.453 ms) and inside CPU-01, so it must never be added to them. The per-shape split is
constrained by two independent bounds but not directly instrumented, and that is recorded as the
remaining uncertainty.

**Reviewed, not used.** The historical recycler's invariants, its recorded failures (the
un-citable 4.15 → 0.29 ms from a scene-mismatched pair; no unit test; structural-only
GPU-completion proof; the startup crash of the later port) and the safety invariants any reuse must
satisfy are recorded in CPU-03. **No recycler code was ported or enabled**, the reverse depth copy
was not optimized, and image-preservation semantics were not altered.

**Stopped for review.** The smallest safe candidate is a reuse pool scoped to the two
`0x2068000000` shapes with a **two-entry bound** (the general 16-slot/768 MB policy dropped), with a
smaller LUT-only step offered to prove the invariants cheaply; a falsifiable hypothesis is recorded
with its falsifiers. **No implementation was started** — the lifetime model is presented for review
first, as instructed.
