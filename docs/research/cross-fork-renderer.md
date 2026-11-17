# Cross-fork renderer work for Déraciné PCVR

Investigation branch `investigation/cross-fork-renderer`, worktree `M:\Development\Deracine-work\wt-crossfork`,
cut from the live runtime branch `integration/r58-presentsize` at **`de9c78c`**. Nothing here is merged,
nothing in `pc-vr/` was touched, and the live executable and its settings were not replaced.

| | |
|---|---|
| Base (fixed local baseline) | `de9c78c` — `test(devtools): hold the stick somewhere, to drive a menu that a pointer drives`, branch `integration/r58-presentsize`, the branch the shipped `pc-vr/shadps4.exe` is built from |
| External source A | `cuesta4/shadPS4` @ `52a244684ffc77c8a57cef2c2d1f03babbb04309` |
| External source B | `AbdelmoumenDeghbouche/wipeout_pc` @ `ca4662a60762cec2d5d119f62a7c6075ba7a3f00` |
| Upstream correctness reference | `integration/upstream-sync-2026-11` @ `24b8620` (worktree `wt-sync`, **not merged** into the runtime line) |
| Benchmark scene | classroom + floating menu, guest `r.ScreenPercentage=300` (5760×3240 packed stereo), 2× MSAA, `SHADPS4_VR_SCALE=0.5`, emulated vblank 60 |
| Harness | `_build/bench/bench.ps1` (unmodified) driven by `_build/crossfork/run-cell.ps1`, runs under the HMD slot |

Full source quotes for every external reference are in [source-digest.md](source-digest.md); the
PSVR port comparison is in [psvr-portability.md](psvr-portability.md).

---

## 1. What the live renderer already has

Checked in the base itself, not in a release note.

| Behaviour | Where | State in `de9c78c` |
|---|---|---|
| Depth/storage representation copy as a **depth-write pass** instead of `vkCmdCopyImage` | `fc2fb55`, `blit_helper.cpp` `CopyColorToDepth32` | **default on** (`SHADPS4_DBG_DEPTH_PASS=0` restores the copy) |
| Barrier inside a render pass for non-attachment images | `33ebbf8`, `Image::Transit` | **default on** (89→65 passes measured on the front end) |
| Whole-refresh skip when a copy is believed to cover everything | `ExpandImage`, `SHADPS4_DBG_SKIP_COVERED_REFRESH` | present, **off**, and its predicate (`src >= dst` in every dimension) cannot be satisfied by an expansion, so it never fires |
| Unconditional contents-skipping in `ResolveDepthOverlap` | `SHADPS4_DBG_NHL_COND` | present, **off**; the black screen this produced is why it is gated |
| Per-draw GPU timestamps with host-work attribution | `vk_gpu_profile.cpp` (`SHADPS4_GPU_PROFILE`) | present |
| Copy timestamps | `Image::CopyImage`, `SHADPS4_DBG_XFER` | present, **accounting was lossy** (see §7) |
| Alias/representation trace | `alias_trace.h` (`SHADPS4_TRACE_ADDR`) | present |
| Per-second operation counters | `NoteOverlap`, `SHADPS4_DBG_OVERLAP` | depth/expand/branch counts only, no bytes, no timings, no image counts |
| Recycling of images | `vk_resource_pool.*` | **absent** — it pools command buffers and descriptor sets only |

### Reference C: upstream correctness fixes

The five fixes named in the assignment are **not in the runtime base**, and all but one are in the
unmerged sync branch:

| PR | Local state in `de9c78c` | In `wt-sync` `24b8620` |
|---|---|---|
| #5444 stale CPU-dirty copy on expand | **absent** — `ExpandImage` copies unconditionally | `9af68f2` |
| #5433 keep subresource data when a parent image is created | **absent** — `if (merged_image_id)` guard still present | `e94f26d`, hardened `8ccb420` |
| #5453 mip-chain bounds | **absent** — `NumLevels()` returns `last_level + 1` unclamped | `7ec75b2` + test `4610df7` |
| #5599 adjacent GPU-written image invalidation | **absent** — `InvalidateMemoryFromGPU` still matches the base address only | `6be27a6` |
| #4953 coherent cropped-image backings | absent | deferred there too (high risk for this fork's cache) |

**Interaction that decides how A1 may be written.** Upstream #5444 makes the copy in `ExpandImage`
conditional on the source holding GPU-written data (`src_image.SafeToDownload()`). Any optimisation
that skips the upload *because a copy will overwrite it* becomes unsound the moment that copy is
gated. The implementation therefore expresses both with one condition (`carries_contents`) so the
port of #5444 is a one-line change in the right place rather than a silent corruption.

---

## 2. What the workload actually does — measured, not inferred

Scene gate `draws ≥ 400 && Mpx ≥ 500`, two consecutive 0.5 s samples; warmup 8 s, window 20 s;
`SHADPS4_FRAME_STATS` windows are the timing source (never XR-present delimiters).

Reproducibility note: `SHADPS4_DBG_AUTOPRESS=1` **alone is not enough** in this base — its press
bound defaults to 45 s and the run stops on the calibration screen (measured: 26 draws, 190 Mpx for
300 s). With `SHADPS4_DBG_AUTOPRESS_TO=150000` the scene marker lands at 37.1 s on sample #63 with
**551 draws / 559.8 Mpx**, which is the classroom-plus-menu signature from
`_handoff/2026-11-17/BENCHMARK-VALIDATION.md`.

One second of that scene (run `xf-diag2`, `diag2-65E40A39.exe`), and the same column divided by the
58 fps the run measured:

| counter (`RENDERSTAT`) | per second | per frame |
|---|---|---|
| `ExpandImage` entries (`expands`), all `exp_3d` | 116–118 | **2.0** |
| of which a covering copy could skip an upload (`covered_expands`) | **0** | **0** |
| guest bytes uploaded by `RefreshImage` | 14.5 MB | 0.25 MB |
| Vulkan images created (`images_created`) | 229–236 | **4.0** |
| image bytes created | 12.5–13.0 GB | 222 MB |
| whole-image copies recorded (`copies`) | 172–177 | 3.0 |
| copy bytes | 4.07–4.22 GB | 72 MB |
| `vmaCreateImage`/`vkCreateImage` time | 40–56 ms | **0.78 ms** |
| `vmaDestroyImage`/`vkDestroyImage` time | 124–178 ms | **2.5 ms** |
| `ResolveOverlap` total (inclusive) | 197–261 ms | **3.7 ms** |
| … of which `ResolveDepthOverlap` | 191–255 ms | **3.5 ms** |
| `ExpandImage` total | 4.5–5.9 ms | 0.08 ms |
| `RefreshImage` total | 5.2–5.8 ms | 0.09 ms |

Reading it:

1. **The two `ExpandImage` events per second-frame are tiny reinterpretations of a 32×32 image**
   (`0x2008f80000`, `A2B10G10R10UnormPack32`, one side 32 layers, the other 1). They move 0.25 MB
   of guest data a frame. `ExpandImage` is not a cost centre in this title at all.
2. **The expensive image work is the depth/storage recreation**, which does not go through
   `ExpandImage`: `ResolveDepthOverlap` re-creates the 5760×3240 `R32Sfloat` ↔ `D32SfloatS8Uint`
   representation about twice a frame (74.6 MB each), and the depth-write pass already carries the
   contents across.
3. **§2's real finding is the image-object churn**: four Vulkan images are created and four
   destroyed per frame, and the create/destroy calls alone cost **3.3 ms of the GPU thread's
   frame** (19 % of the 17.2 ms frame), with 12.9 GB/s of allocation churn behind them.
4. `ResolveOverlap` costs 3.7 ms a frame *inclusively*; 0.78 ms of that is the image creation it
   triggers, and the rest is bookkeeping plus barriers around the two recreations.

---

## 3. A1 — overwrite-aware image refreshing

### What the base does

`TextureCache::ExpandImage` creates the requested image and then

```cpp
RefreshImage(new_image);   // detile guest memory -> staging buffer -> vkCmdCopyBufferToImage
new_image.CopyImage(src_image);  // then overwrite part of it with the old image
```

`Image::CopyImage` (`image.cpp`) writes, for every mip level below `min(src.levels, dst.levels)`,
the **source's extent** of that level at destination offset (0,0), for the layers the two share
(`SanitizeCopyLayers`), and for 2D↔3D pairs a layer/depth swap. It does not write the destination's
extra levels, extra layers or larger extent.

So the upload is redundant exactly where the copy lands, and nowhere else. The pre-existing
`SHADPS4_DBG_SKIP_COVERED_REFRESH` experiment asked for the whole image to be covered
(`src >= dst` in width, height, levels and layers) — which is the opposite of what an expansion is
for, so it could never fire. That is why it was safe and useless at the same time.

### What was implemented

`image_coverage.h` (new) answers the question per level: `CoveringCopyOf(src, dst)` returns
`{mips, layers}` — the destination levels `[0, mips)` that the copy writes whole, and the layers
`[0, layers)` within them. `RefreshImage(image, covered)` then skips building an upload for a level
that `CopyCoverage::CoversLevel(level, num_layers)` says is written whole.

The predicate is deliberately narrower than "the copy probably reaches it":

- same Vulkan dimensionality and not 1D (a 1D image's height must be 1, and the 2D↔3D pair swaps
  layers for slices — neither shape is the source's extent);
- same texel size, sample count and block-compressedness;
- **colour only**: a depth image's contents are not only the texels — HTILE metadata and the
  stencil aspect sit beside them and no copy here carries those across. Déraciné's depth aliasing
  is `ResolveDepthOverlap`'s job, so declining costs an upload and not correctness. (cuesta4's
  equivalent is colour-only for the same reason; see the digest, risk 1.)
- destination not larger than the source at level 0 (extents halve per level, so that proves it for
  every level the copy reaches).

Every other answer is `{0, 0}`, which means "upload everything", i.e. the previous behaviour.

Guarded by `SHADPS4_DBG_COVERED_REFRESH=1`, off by default, with the shapes it decides on logged
once each (`COVERED_REFRESH …`).

### Result: correct, tested — and no workload to apply it to

`covered_expands=0` in every measured second of the classroom scene. The two expansions per frame
are 2D↔3D reinterpretations of a 32×32 image, which the predicate correctly refuses. The guest
bytes the skip could save are 0.25 MB a frame, i.e. **nothing**.

This is the assignment's "if full overwrite cannot be proven, retain original behaviour" branch:
the change is kept as a proven, switchable fast path with regression tests (`shadps4_image_coverage_test`,
18 cases) and is **not** proposed for default-on. Its value is that the next title (or the same
title in gameplay) whose expansions *are* same-shape mip-chain growth gets it for free.

---

## 4. A2 — Vulkan image recycler

### Why it is justified

§2: four images created and destroyed per frame, 0.78 ms creating and 2.5 ms destroying, all on the
GPU command processor thread, which cannot submit while it is inside `vmaCreateImage` /
`vmaDestroyImage`. 3.3 ms is 19 % of a 300 % front-end frame and 40 % of the 8.3 ms a 120 fps frame
would have.

The two big shapes alternate (`R32Sfloat` → `D32SfloatS8Uint` and back) and are otherwise identical,
so a pool keyed on the exact creation parameters hands each one straight back.

### Design and invariants

New `renderer_vulkan/vk_image_recycler.{h,cpp}`, wired into `UniqueImage::Create` (acquire),
`~UniqueImage`/`UniqueImage::Destroy` (release) and `Instance::~Instance` (shutdown before the VMA
allocator dies).

| Invariant | How |
|---|---|
| Reuse only for identical creation configuration | `SameShape`: flags, type, format, extent, mip levels, array layers, samples, tiling, usage, sharing mode, queue family indices, initial layout, **and the `view_formats` list** (a difference there is a driver reservation the image would not have) |
| Not reused until consuming submissions completed | release happens in the image destructor, and the texture cache erases images from `scheduler.DeferOperation`, which the scheduler runs only when the master semaphore reports that tick complete |
| Old contents never treated as valid | a recycled image is handed to a fresh `BackingImage`, whose state is `layout = eUndefined`; the first barrier discards, and `UniqueImage::Create` stamps the new create info and view list over the object |
| Old descriptor/view state not inherited | image views belong to the `BackingImage` that is being destroyed; `UniqueImage` carries only the image and its allocation |
| Bounded memory | at most 8 images / 512 MiB pooled; anything else is destroyed instead of kept |
| Thread safety | one mutex around the pool; `Acquire`/`Release` are short |
| Independently switchable | `SHADPS4_DBG_IMAGE_RECYCLER=0` restores the driver's create/destroy path, default on |

It is not an image-coherence shortcut: nothing that decides *what an image contains* (validity
flags, mip hashes, `GpuModified`, layout state, meta surfaces) is pooled or reused.

### Measured (classroom, 300 %, same binary, `SHADPS4_DBG_IMAGE_RECYCLER` the only difference)

Binary `rec3-A02FB23F.exe` (sha256 `A02FB23F7FC15B81B0F2C081DBBD9F9851F9CC3BD1D9A5D4B7761859BF50B1CB`),
interleaved A-B-A under one HMD slot hold, `-RequireQuiet` (processor time 10.8 % / 18.9 % / 10.8 %
before the three cells).

| cell | arm | scene reached | fps p50 | frame p50 / p90 / p99 | >20 ms frames | images created / frame | create+destroy per frame | hits |
|---|---|---|---|---|---|---|---|---|
| `xf-a2-1` | **on** | classroom, 537 draws / 572.6 Mpx | 58.75 | 17.02 / 17.42 / 18.25 ms | 0 | 0.51 | **0.29 ms** | 85 % |
| `xf-a2-0-r2` | off | **calibration** — 176 draws, scene gate not met | 58.85 | 16.99 / 17.48 / 36.85 ms | 3 | 4.20 | **4.15 ms** | 0 % |
| `xf-a2-1-r3` | **on** | classroom, 527 draws / 526.0 Mpx | 58.65 | 17.05 / 17.48 / 18.44 ms | 0 | 0.55 | **0.32 ms** | 85 % |

What this does and does not say:

- **The CPU-side saving is large, directly attributed and repeatable**: 4.15 ms → 0.29/0.32 ms of
  GPU-thread time per frame, from 4.2 to ~0.5 driver image creations a frame at an 85 % hit rate.
  Both arms are the same executable; the timers wrap the exact calls.
- **No frame-rate claim.** The classroom cells sit at the title's 60 Hz front-end cap (58.7 fps,
  GPU 53–57 % busy), so there is nothing for a CPU-side saving to buy there — p50 17.02 and 17.05 ms
  in the two ON cells, no long frames. The off arm landed on the calibration screen, so even a
  capped-pair comparison is not available. **This change is not presented as an FPS win.** It
  removes work from the thread that has to submit the frame; at the 120 fps objective that work was
  3.9 ms of an 8.3 ms budget.
- VRAM cost of the pool: ~250 MB held (the meta files record 6.92–6.94 GB in use either way, inside
  run-to-run noise). The pool is bounded at 768 MB.
- **Correctness evidence for this cell set**: same passes/frame (64–66), same draws (535), same
  Mpx (481) in both classroom cells and in the earlier non-recycler classroom run, no new error or
  warning classes, and the images the title reads are the ones it wrote (a recycled image carries
  no contents; its layout starts at `eUndefined`).

Two cells were **discarded rather than reported**, and they are worth knowing about:

1. `xf-diag1` and `xf-rec2-on` never reached the classroom (`meta.json`:
   `"fallback: no sample matched the scene threshold"`). The scene gate is flaky even with the
   longer autopress window — roughly half the runs get there. A cell without the scene marker is
   not a classroom measurement.
2. `xf-rec-on` (the first recycler cell) measured **41.3 fps / GPU 37 %** on the same scene that
   measured 58 fps twenty minutes earlier. Nothing was wrong with the binary: another agent's Ninja
   build was running in `wt-autonomous-guardrails` and starving the GPU thread. That is why the
   runner now refuses to start while build tools exist (`-RequireQuiet`) and why the processor time
   is recorded next to each cell.

### The first version of the pool did not work, and why

With 8 slots and plain FIFO eviction the pool held **eight 4 KB images totalling 16.9 MB** and
scored **zero hits** while still paying for the pool: each frame's 74.6 MB depth image was thrown out
to make room for another 4 KB expansion. Eviction by size (keep the expensive shape, drop the
cheapest entry) plus 16 slots took the hit rate from 0 % to 85 %. That is recorded here because
"the recycler was slower" was almost concluded from it — the run was 40 % slower, but for the
contention reason above, not because of the pool.

---

## 5. A3 — shared-block image allocations: analysis only, deferred

Not implemented, because the measurement in §2 makes it the wrong lever:

- The cost is the **image object's create/destroy**, not the memory block behind it: `image_mb`
  shows 12.9 GB/s of allocation churn, yet `create_ms` is 0.78 ms and the recycler removes the
  churn entirely without changing how memory is allocated.
- VMA already suballocates; a dedicated allocation is only forced where the driver requires one, and
  forcing images into shared blocks would trade this cost for aliasing rules the fork cannot enforce
  (memory requirements and alignment per format, image granularity, the guest-visible memory model).
- cuesta4's shared-block path depends on its alias-identity machinery, which this fork does not have.

Revisit only if, after the recycler, `create_ms`/`destroy_ms` remain material — with a VMA
allocation dump rather than by inference.

---

## 6. B — mip-chain growth: present in the title, but not in the steady-state frame

The trace found exactly one image type whose expansions are compatible mip growth:
`0x201f600000` and `0x2025000000`, `128x128`, `R16G16B16A16Sfloat`, 8 layers, growing one mip level
per expansion (1→2→3…→8). `CoveringCopyOf` reports `mips=1, layers=8` for each step, so a
preallocated backing (`CanGrowMipLevels`) would apply — each step currently re-creates the image and
copies the levels it had.

But it happens during **loading**, not in the classroom frame: `covered_expands=0` in every measured
second of the scene, and the `COVERED_REFRESH` lines for that image appear only in the early part of
the log. Deferred, with two preconditions if it is picked up: port #5453 first (an over-declared mip
chain makes the grown image illegal), and re-trace in gameplay to see whether the classroom itself
grows mip chains that the front end does not.

---

## 7. D — measurement accounting

Two things were wrong, and both mattered for judging the work above.

1. **Function counters and GPU queries disagreed** because the query path dropped work silently.
   `Image::CopyImage`'s `SHADPS4_DBG_XFER` block kept a 512-timestamp pool, wrote a pair per copy,
   and when the read came back `eNotReady` it reset the pool anyway. A copy could therefore be
   recorded, never read, and never counted. It now counts pairs **requested, recorded, retrieved,
   dropped (no room) and not ready**, reads the previous second's pairs *before* asking for room for
   the new one (so a slow read no longer discards measurements), and prints all five counters with
   the average. The interval is bottom-of-pipe to bottom-of-pipe, i.e. an **upper bound** that
   includes any wait the copy had for earlier work — the comment says so, because reading it as
   intrinsic copy time is exactly the mistake this instrument invites.
2. **"N calls a frame" is not a cost.** `ImageStats` (new, `texture_cache/image_stats.h`) reports
   per second: expand entries and how many had a covering copy, refreshes with **uploaded and
   skipped mip counts and bytes**, images created and their bytes, whole-image copies and bytes,
   recycler hits/misses/pool, and **wall time** in `vmaCreateImage`, `vmaDestroyImage`,
   `ResolveOverlap`, `ResolveDepthOverlap`, `ExpandImage` and `RefreshImage`. One `RENDERSTAT` line,
   `SHADPS4_DBG_OVERLAP=1`.

---

## 8. Benchmark results

All cells: `_build/bench/bench.ps1` (unmodified), `-Vblank 60`, `SHADPS4_DBG_AUTOPRESS=1` with
`SHADPS4_DBG_AUTOPRESS_TO=150000`, `render_percent=300`, `msaa=2`, `SHADPS4_VR_SCALE=0.5`,
`SHADPS4_DBG_OVERLAP=1`, scene gate `draws ≥ 400 && Mpx ≥ 500` on two consecutive 0.5 s samples,
timing from `SHADPS4_FRAME_STATS` windows (never XR-present delimiters). Runs live under
`M:\Development\Deracine-work\_build\bench\runs\xf-*`; each has its `meta.json`, `timings.csv`,
log, and the executable hash.

| label | binary | arm | scene | n | fps p50 | frame p50 / p90 / p99 | >20 ms | GPU busy | create+destroy |
|---|---|---|---|---|---|---|---|---|---|
| `xf-diag2` | `diag2-65E40A39` | baseline + counters | classroom 551/560 | 16 | 58.55 | 17.08 / 17.73 / 18.07 | 0 | 60 % | 4.15 ms* |
| `xf-a2-1` | `rec3-A02FB23F` | recycler on | classroom 537/573 | 60 | 58.75 | 17.02 / 17.42 / 18.25 | 0 | 53 % | 0.29 ms |
| `xf-a2-1-r3` | `rec3-A02FB23F` | recycler on | classroom 527/526 | 60 | 58.65 | 17.05 / 17.48 / 18.44 | 0 | 57 % | 0.32 ms |
| `xf-a2-0-r2` | `rec3-A02FB23F` | recycler off | calibration 176 | 60 | 58.85 | 16.99 / 17.48 / 36.85 | 3 | 30 % | 4.15 ms |

\* the baseline classroom cell did not run with the recycler, so its create+destroy is the same
figure the off arm reports (the calls are unconditional in that arm), and it agrees with the
`xf-diag2` counters: 0.78 ms create + 2.5 ms destroy at 58 fps = 3.3 ms, versus 4.15 ms measured
for the off arm at 56 fps.

**Honest reading of the table**

1. **Unique FPS is unchanged**, and the reason is structural, not measurement noise: every classroom
   cell sits at the title's own 60 Hz front-end cap with the GPU 53–60 % busy. There is no frame-rate
   difference to find on this scene, in either direction. Nothing in this branch is claimed as an
   FPS improvement.
2. **What did change is measurable and reproduced**: 3.9 ms per frame of GPU-thread time removed
   (create+destroy 4.15 → 0.29/0.32 ms, 4.2 → 0.5 image creations a frame, 85 % hit rate), with the
   same scene, passes, draws and Mpx.
3. **A1 (overwrite-aware refresh) is a measured no-op for this title**: `covered_expands=0` in
   every second of every classroom cell, and the guest bytes it could skip are 0.25 MB a frame.
4. The confidence in (2) is high because the attribution is direct (timers around the two calls,
   counts of the calls) and the two ON cells agree to 10 %; the confidence in any fps statement here
   would be low, which is why there is none.
5. **Not measured, and needed before a claim about the goal**: the user's gameplay classroom at
   ~33–36 fps cannot be reached headless (the menu is driven by a located controller of a worn
   headset — every route is documented in `_build/PASSES-AND-PACING.md` §20). The same per-frame
   work applies there, so the CPU-side saving should carry over, but "should" is not "measured".
   One session with the headset worn, walking into the classroom, would settle both: the fps at
   33–36 (uncapped, GPU-bound) and whether the recycler's 3.9 ms matter when the frame is 28 ms.

**Reproduce one cell**

```powershell
# holding the HMD slot, from the workspace root
bash _build/team/testslot.sh run --owner <you> --reason "crossfork cell" --ttl 1200 -- `
  pwsh.exe -NoProfile -File 'M:\Development\Deracine-work\_build\crossfork\run-cell.ps1' `
  -Exe 'M:\Development\Deracine-work\_build\crossfork\bin\rec3-A02FB23F.exe' -Label my-cell `
  -ExtraEnv 'SHADPS4_DBG_AUTOPRESS=1;SHADPS4_DBG_AUTOPRESS_TO=150000;SHADPS4_DBG_OVERLAP=1' `
  -Warmup 10 -Seconds 30 -TimeoutSeconds 220 -RequireQuiet
```

---

## 9. Integration recommendation

| Commit on `investigation/cross-fork-renderer` | Safe to integrate? | Why |
|---|---|---|
| `49ec266` diag: count images, copies and refresh uploads per second | **yes** | log only, off unless `SHADPS4_DBG_OVERLAP=1` / `SHADPS4_DBG_XFER=1` |
| `fcdf3de` perf: skip the upload of levels a covering copy overwrites | **no, not yet** | correct and tested, but no measured workload in this title (§3); keep the switch and the predicate for gameplay or the next title |
| `6170df6` perf: recycle Vulkan images released with the same creation parameters | **yes** | removes 3.9 ms/frame of GPU-thread work; default-on with `SHADPS4_DBG_IMAGE_RECYCLER=0` as the escape hatch; 250 MB of pool |
| A3 shared-block allocations | no | wrong lever, unsafe without alias identity (§5) |
| B mip-chain growth | deferred | loading-time only in this title; needs #5453 first (§6) |
| cuesta4 alias-authority state machine | no | would replace this fork's entire aliasing model; A1's predicate does not need it |
| #5444 / #5433 / #5453 / #5599 | **prerequisites**, from `wt-sync` `24b8620` | already ported and reviewed there; take them before anything that assumes them — in particular #5444 must be applied *together* with the `carries_contents` condition in `ExpandImage` |

The recycler is the only one of these that changes what a frame costs, and it is the only one worth
integrating now. It does not change a pixel: it changes who owns the Vulkan image object between two
identical requests.

## 10. Progress ledger (what was actually done, in order)

| # | Step | Result |
|---|---|---|
| 1 | Established actual git state: 14 worktrees, live branch `integration/r58-presentsize` @ `de9c78c`, `wt-sync` @ `24b8620` not merged | base fixed at `de9c78c`; new worktree `wt-crossfork`, branch `investigation/cross-fork-renderer` |
| 2 | Read the base's already-integrated work (depth-write pass default, barrier-in-pass, skip-covered-refresh experiment, GpuProfile, AliasTrace, `vk_resource_pool`) | §1: nothing to re-port; only counters and the recycler were missing |
| 3 | Built the baseline, then an instrumented build; kept every binary and its sha256 in `_build\crossfork\bin` | `baseline-de9c78c`, `diag1/2`, `rec`, `rec2`, `rec3` |
| 4 | Traced the classroom scene with the operation counters | §2: 2 tiny expansions, 2 depth recreations, 4 images created/destroyed, 3.3 ms in create/destroy |
| 5 | Implemented the A1 predicate and the per-level upload skip; wrote 19 tests | §3: correct, switchable, **zero applicable events** |
| 6 | Implemented the A2 recycler; first pool (8 slots, FIFO) scored 0 hits | eviction by size + 16 slots → 85 % hits |
| 7 | A-B-A cells | §8: 3.9 ms/frame of GPU-thread work removed; no FPS claim |
| 8 | Reworked the copy-timestamp accounting | §7 |
| 9 | Tests: `shadps4_image_coverage_test` 19/19 pass, and 2 of 19 fail with the extent check removed (negative control) | §3, §8 |
| 10 | Measured what could not be done: A3 analysis, B trace, gameplay ceiling | §5, §6, §8 point 5 |

Deliberately not done: no wholesale upstream sync (the five correctness ports already exist in
`wt-sync` and were not duplicated here), no alias-state machine, no presenter/pacing/signal changes
from cuesta4's large commit, no shared-block allocation, no mip-chain growth, no PSVR architecture
change, and nothing merged or pushed.
