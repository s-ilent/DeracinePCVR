# Cross-fork integration candidate — state, review and gates

Branch `integration/cross-fork-renderer`, worktree `M:\Development\Deracine-work\wt-crossfork-int`,
cut from the live runtime branch `integration/r58-presentsize` at **`de9c78c`**. The investigation
branch `investigation/cross-fork-renderer` (`e676a43`) is preserved untouched; this branch is a
candidate, not a merge. Nothing was pushed, nothing was merged into the runtime, and no executable
in `pc-vr/` was replaced.

## 1. The commit set

| commit | what | status |
|---|---|---|
| `f0d5f98` | **diagnostics**: per-second `RENDERSTAT` (images created/destroyed + bytes, copies + bytes, refresh mips/bytes, recycler hits/misses/pool, wall time in the texture cache's entry points), plus honest copy-timestamp accounting (requested / recorded / retrieved / dropped / not-ready) | ready |
| `de64a7a` | **diagnostics**: the counters' timers cost nothing when the counters are off; `RENDERSTAT` also reports the allocator's own device memory | ready |
| `275d654` | **A1**: `CoveringCopyOf` + per-level upload skip, opt-in, off by default | kept, **not enabled** |
| `d3123a8` | build fix for the A1 test target's include paths | ready |
| `2498976` | **recycler**: `vk_image_recycler`, keyed on the whole create info + view-format list | in, **off by default** |
| `3d375a4` | **RenderDoc**: scene-gated capture request (`SHADPS4_DBG_RDOC_CAPTURE_SCENE=<draws>,<mpx>`) | ready, not exercised live |
| `117a3bb` | **fixes from the independent review**: FSR's ungated destroy no longer pools (R1), OOM drain-and-retry (R2), move-assignment no longer leaks (R10), every destroy counted (R4), `=false` means off (R13) | ready |
| `0a09829` | recycler **default off** until it has a matched-scene measurement | ready |
| `7a2b042` | assertion that a skipped upload is covered by the copy that replaces it, and the exact shape the #5444 port must take | ready |

Build: clean, `shadps4.exe` links. Tests: **580/590 pass, the 10 failures are the documented
pre-existing Windows set** (7 targets that cannot build on this platform,
`ReadConstSnapshotDiag.IsolationDisabledAfterTestB`, `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`;
`_build/SETUP.md`), and the new `ImageCoverage` tests (19) pass inside the suite.

## 2. Independent review of the recycler

Full document: [recycler-review.md](recycler-review.md) (523 lines, adversarial, verdict **approve
with conditions**; no blocker, 3 majors, 11 minors). What changed as a result:

| finding | what it was | what this branch does |
|---|---|---|
| **R1** (major) | `FsrPass::CreateImages` destroys its images with `UniqueImage::Destroy()`, outside the scheduler and with no tick gate, and its next images have *identical* create info — so a still-in-flight image would have been handed straight back | `Destroy()` no longer pools; it destroys on the spot (`DestroyNow`). Pooling is the destructor only, which the texture cache reaches through the deferred erase |
| **R2** (major) | the pool could turn a working allocation into `VK_ERROR_OUT_OF_DEVICE_MEMORY` and the `ASSERT_MSG` after it into an abort | drain the pool and retry once; the drain logs what it freed |
| **R3** (major, unproven) | tracked contents are never read as valid, but passes use `loadOp=eLoad`, so a validity mistake would read the previous owner's pixels | **not closed** — needs a pixel A/B (see §5) |
| **R4** (minor) | `images_destroyed` missed the bare `vmaDestroyImage` sites, so the A/B could not be read | every destruction path counts now |
| **R6** (minor) | teardown is never exercised (`std::quick_exit` skips `~Presenter`), so `Shutdown()` + `vmaDestroyAllocator` never run | recorded, not changed: if teardown never runs, neither does the allocator's destruction, so there is no ordering hazard — only an exit-time leak the process would reclaim anyway |
| **R10** (minor) | `UniqueImage` move-assignment leaked the image it overwrote ([container.reqmts] requires releasing it) | move-assignment releases first |
| **R11, R12, R14** | one pooled slot per shape; a fixed 768 MiB bound rather than a fraction of the device budget; a sixth `Create` caller in dead code | recorded in the review; not changed (see §5) |

The review's core claims held: the key covers every scalar field plus the view-format list (and
`pNext` is null at all five live create sites — an assumption now documented rather than checked);
a recycled image always lands in a fresh `BackingImage` whose layout is `eUndefined`; release is
gated on `MasterSemaphore::IsFree` inside `PopPendingOperations`; the bound is enforced on every
growth path; the mutex covers all state.

## 3. Defaults

| switch | default | why |
|---|---|---|
| `SHADPS4_DBG_IMAGE_RECYCLER` | **off** | its only measurement compared a classroom arm with a calibration arm, and R3 is open. Promote it only when §5's gates pass |
| `SHADPS4_DBG_COVERED_REFRESH` | **off** | measured zero applicable events in the classroom; kept for the next title or a gameplay trace |
| `SHADPS4_DBG_OVERLAP`, `SHADPS4_DBG_XFER`, `SHADPS4_DBG_RDOC_CAPTURE[_SCENE]` | off | diagnostics |

**Promotion criteria for the recycler** (all of them, in this order):

1. Matched-scene front-end cells (one binary, on/off/on, scene gate enforced with
   `-RequireScene`) showing the allocation and CPU-thread saving, with both arms' `draws_mean` and
   `mpx_mean` within 5 %.
2. Gameplay cells per [gameplay-benchmark-procedure.md](gameplay-benchmark-procedure.md), same rules.
3. A pixel A/B in one scene (R3): render-target dumps from both arms compared with
   `_build\bench\capture-compare.py`; every image both runs dumped must be bit-identical or inside
   the measured noise floor.
4. No new validation error class in either arm (the lab config keeps core validation on).

Until then the branch ships the recycler as an opt-in measurement tool, which is what "safe default
behaviour" means for a change whose benefit is CPU-side and whose risk is another image's pixels.

## 4. Upstream reconciliation (#5444, #5433, #5453, #5599, #5495)

Full document: [upstream-reconciliation.md](upstream-reconciliation.md). Summary:

- **None of the five are in `de9c78c`.** All are in `wt-sync` `24b8620` (unmerged). Applied to this
  candidate: `7ec75b2`, `09c7ecc`, `6be27a6` and `e94f26d` apply cleanly; `8ccb420` and `24b8620`
  apply cleanly once their predecessors are in; `9af68f2` conflicts in one hunk (an insertion-point
  collision with this branch's `carries_contents` block in `ExpandImage`) and `4610df7`'s CMake
  block collides with the `shadps4_image_coverage_test` block at the same anchor (its test file
  applies cleanly). `8ccb420` is worth noting as *not* an upstream port: it fixes a hazard that
  `e94f26d` itself introduces (a slot-vector reallocation invalidating a held image reference).
- **#5444 must not be adopted without moving the coverage gate with it.** The reconciliation works
  through every dirty/GPU-modified state combination; with the copy made conditional and the
  coverage not, destination levels are never written. The failure would be invisible today (the skip
  is opt-in), which is the worst kind. `7a2b042` adds the assertion and spells out the expression the
  port needs: `src_image.SafeToDownload() || src_image.info.num_samples > 1` (the second term because
  `RefreshImage` declines multisampled images, making the copy the only writer there). A1's predicate
  and #5444 are complementary, not contradictory: A1 answers *where* the copy writes, #5444 *whether*
  it runs. The brief's premise that upstream #5444 carries "overwrites reasoning" does not hold —
  upstream's change is 11 lines and a `HasGpuValidData()` gate, with no coverage logic at all.
- **#5433 needs a companion edit**: after `CopyMip` into a freshly created parent, the parent holds
  GPU-authored texels but `GpuModified` is clear, so a later expansion under #5444's gate would drop
  what #5433 just preserved. Mark it (or gate that copy) in a separate, labelled commit.
- **Recommended order** (each step's reason is in the reconciliation's table): `7ec75b2` → `24b8620`
  → `09c7ecc` → `6be27a6` → `e94f26d` → `8ccb420` → `9af68f2` **with both guards in the same
  commit** → the optional #5433 `GpuModified` companion → `4610df7`. Only real constraints:
  `7ec75b2` before `24b8620`, `e94f26d` before `8ccb420`, `6be27a6` before `9af68f2` (it keeps the
  new gate from copying planes whose truth is in the buffer cache), `7ec75b2` before any enabled
  coverage path. #4953 stays deferred.
- Not adopted here: adopting behaviour-changing renderer fixes needs the same matched-scene and
  gameplay gates, and the machine is unavailable for them. Duplicating them would also violate the
  "do not duplicate existing local fixes" instruction — they live in `wt-sync` and must be taken from
  there.

## 5. Outstanding validation requirements

1. **Matched front-end cells** (blocked: headset in use). Command in
   [gameplay-benchmark-procedure.md](gameplay-benchmark-procedure.md), last section.
2. **Gameplay cells** (blocked: needs a headset session). Procedure and acceptance rules in the same
   document. The 33–36 fps classroom has **not** been measured with the recycler; the front end's
   ~59 fps says nothing about it and is not quoted as if it did.
3. **Pixel A/B for R3** (blocked: same session). No capture comparison exists for the recycler in a
   matched scene; the earlier attempt compared 9 dumpable images bit-identically across two runs
   that had reached different content phases, which is not enough.
4. **RenderDoc capture of the classroom** (blocked: same session). Provenance is recovered and the
   scene-gated trigger is implemented, but no capture has been taken with it —
   [renderdoc-reproducibility.md](renderdoc-reproducibility.md) has the procedure and the
   classification of the earlier failures (the demonstrated one is a trigger delay longer than the
   process lifetime, not a RenderDoc or hardware limitation).
5. **Recycler teardown path** (R6) and **pool sizing** (R11/R12): decide after (1)–(3), with numbers.
6. **An independent report of UE4 colour-grading LUT corruption must be resolved before
   promotion.** The workstream holding the HMD slot is running a controlled recycler off/on
   comparison on a build for exactly that symptom. If LUT contents are disturbed with the
   recycler on, it is R3 with a concrete case - a pass that reads contents the cache considers
   irrelevant - and the recycler stays opt-in regardless of its CPU-side saving. Their build
   (A02FB23F, default-on, before the four lifecycle fixes) and the current candidate
   (7B658961, opt-in) are not the same thing, so the comparison has to be repeated on the
   candidate before either conclusion is kept.


## 6. Isolation

- `pc-vr/` and its `shadps4.exe` were not touched; the live branch is still at `de9c78c`.
- `investigation/cross-fork-renderer` was not modified after `e676a43`.
- No other worktree was written. The shared bench harness, its binaries and its runs were not
  modified; this branch's runs use `_build\crossfork\` and unique labels.
- No GPU work was performed in this phase: the headset is in use. Every number quoted above was
  measured earlier, and every cell that would decide the remaining questions is listed in §5 with
  its command.
