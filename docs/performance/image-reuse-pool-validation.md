# Image reuse pool validation (CPU-03)

Status: **VALIDATED for correctness and mechanism; frame-rate effect measured on same-executable
pairs only, magnitude not settled.** The implementation builds, its focused tests pass, it runs in
Déraciné gameplay with the pool live, its memory is bounded, it does not change the rendered image
beyond run-to-run noise, and it tears down cleanly with allocations still pooled. It also removes the
allocator time CPU-03 attributed, and both same-executable pairs moved the rendered frame rate the
same way by a similar amount. No headset acceptance.

This record **replaces** the earlier "no FPS gain is claimed" conclusion. That conclusion was wrong:
it compared the reuse-ON cell of one executable against the reuse-OFF cell of a *different* one, and
treated the mismatch as noise. The pairs below are same-executable and agree in direction.

## Identity and build

- Worktree `M:\Development\Deracine-work\wt-perf-reuse`, branch `perf/aliased-image-reuse-2026-11-19`,
  rebased onto current `main`; `src/` is byte-identical across the rebase (`git diff 80bfc9c2 HEAD
  -- src` is empty).
- Build: **PASS**, exit 0. Two binaries are involved, and every comparison below is *within* one of
  them:
  - `AF1936720C44EF8C196773DB861322F96A03AD7775A300F8F1AABDFA0116FC6F` — pre-rebase build.
  - `E4AD38248B21AFE60F74E987DBBE3B79BD90AF86E81D096208F457D83CAD4C51` — the pushed commit's
    rebuild (the rebase forced a full relink; `src/` identical, so the same code).
- `ctest --test-dir build-tests -R ImageReusePool --output-on-failure`: **7/7 passed**.
- **What these cells validated:** the source tree at `534e44d0`, i.e. the binary
  `E4AD38248B21AFE6…`. That tree is exactly what this integration branch carries.
- A later teardown-only change, `5c9abe34` (*safely clean up reused images*: `device.waitIdle()`
  before the pool drain, a `UniqueImage` self-assignment guard, and `Destroy()` on move-assign),
  appeared on the working branch and is **deliberately excluded here**. It compiles cleanly — it
  produced `634225B4CAA901D306FDEDB146FDD1307205B87E17D90BD74AF2D73FF4F6C940` — but it was never
  run, so it is preserved unmeasured on `perf/image-reuse-teardown-unmeasured` and must not ride
  along with this feature. Its runtime reach was reviewed anyway: `UniqueImage::operator=`
  *is* reachable during gameplay (`Image::EnsureViewFormat`, `Image::SetBackingSamples` from
  `vk_rasterizer.cpp` and the `vk_runtime.cpp` copy paths), but every current site assigns onto a
  freshly `emplace_back`'d `BackingImage` whose `UniqueImage` is empty, so the added `Destroy()` is a
  no-op there and the self-assignment guard never fires; there are no whole-`Image` move-assignment
  sites. Only the `waitIdle` in `Instance::~Instance` changes behaviour, and only at teardown.
- The full test tree still fails on the pre-existing Windows portability error
  (`src/tests/libraries/test_usbd_event_loop.cpp:6`, `<sys/time.h>`); unrelated to this change.

## Runtime cells

Tracked `tools/benchmark/headless-gameplay.ps1`, unmodified; isolated lab per cell from the same
`pc-vr` template; same eboot and game shadow (300 % / MSAA 2 / vblank 120); save slot 1
(`m00_00_10_10`) provisioned from the manifest-pinned source; same scripted gameplay entry.
`SHADPS4_DBG_OVERLAP=1` and `SHADPS4_GPU_TIMING=1` in every cell. Allocator figures are means over the
last 60 one-second `RENDERSTAT` windows (steady gameplay); fps/draws/mpx/passes are the gate's
gameplay window.

| Cell | Executable | reuse | Valid | Unique FPS | Draws / Mpx / passes | GPU busy | images_created | create_ms | destroy_ms | pool hits |
|---|---|---|---:|---:|---|---:|---:|---:|---:|---:|
| `arm0-g1` | `AF193672…` | off | yes | **34.32** | 1871.6 / 909.7 / 59.2 | 58.8 % | 135.8/s | 25.05/s | 64.20/s | 0 |
| `arm1-g1` | `AF193672…` | on | yes | **39.79** | 1866.3 / 950.9 / 60.7 | 68.2 % | 96.8/s | 6.43/s | 17.66/s | 68.2/s |
| `backslash1` | `E4AD3824…` | off | yes | **27.12** | 1866.0 / 894.9 / 57.7 | 44.0 % | 109.0/s | 25.81/s | 66.58/s | 0 |
| `arm1-g2` | `E4AD3824…` | on | yes | **30.99** | 1866.1 / 913.2 / 58.6 | 50.0 % | 65.1/s | 1.68/s | 2.82/s | 59.6/s |

All four cells: `valid: true`, `phase: gameplay`, exit 0, clean presenter shutdown.

## Same-executable comparisons

| Pair | reuse OFF | reuse ON | Δ fps | Δ % | `create_ms + destroy_ms` |
|---|---:|---:|---:|---:|---|
| `AF193672…` | 34.32 | 39.79 | +5.47 | **+15.9 %** | 89.25 → 24.09 ms/s |
| `E4AD3824…` | 27.12 | 30.99 | +3.87 | **+14.3 %** | 92.39 → 4.50 ms/s |

Both pairs move the same way by a similar amount, on two binaries, with the OFF-side allocator cost
nearly identical between them (89.25 vs 92.39 ms/s) — which is what the mechanism predicts.

**Limitations of these pairs, stated plainly:**

1. Each pair is **one OFF and one ON**, not interleaved. Ordering, time of day, thermal state and
   GPU clock drift are confounded with the arm; only the sign is robust.
2. The within-configuration spread is large. The two OFF cells, same executable class and same
   configuration, measured 27.12 and 34.32 — a 27 % swing — and the ON cells 30.99 and 39.79. With
   n=1 per arm per executable, the **magnitude** (+14…+16 %) is not established by these pairs.
3. Every cell builds its own isolated lab, so the scene is not bit-identical run to run (draws/mpx
   medians differ by up to ~5 %). The gate confirms the band, not the exact frame.
4. The ON cell with the least allocator work left (`arm1-g2`, 4.50 ms/s) was the slowest ON cell, so
   these numbers must not be read as a dose–response between allocator time and fps.
5. No 100 % or 200 % cells were completed, so it is **not** established whether the gain is confined
   to GPU-limited conditions or also appears where the CPU is the limiter.
6. `unique_fps` is the emulator's own rendered-frame series, not compositor cadence, and each cell is
   a single 40 s window.
7. A ~44 FPS gameplay observation was reported from a separate session. Nothing here reproduces it;
   it sits above every cell measured in this workspace and is consistent with the upper end of the
   spread, not with a specific cell.
8. No headset acceptance was performed and no latency/comfort claim is made.

## Allocator mechanism (solid, independent of the fps question)

- The two recurring large create shapes are served from the pool: 59.6–68.2 hits/s ≈ 1.7–1.9 of the
  ~4 image creations a frame, and `images_created` falls from 109–135.8/s to 65–96.8/s.
- `create_ms + destroy_ms` falls from 89.25–92.39 ms/s to 1.68–17.66 ms/s — the cost CPU-03
  attributed to the R32F/D32S8 pair. This is a within-run measurement and does not depend on
  cross-run comparability.
- `reuse_misses` counts every poolable-shaped request that found no entry, including the sub-16 MiB
  shapes that can never be pooled; it tracks small-image traffic and is not a shortfall.
- Retention is bounded: `reuse_bytes` peaks at 219.4 MiB (three allocations) against the 256 MiB /
  6-entry cap, and `vram_blocks` does not grow (1675 → 1672).

## Render-target pixel A/B

`SHADPS4_DBG_DUMP_HMD_AT_SCENE` gated on the gameplay band (`1600,700`), so the menu ran at full
speed and dumping began once gameplay was reached. Dumping collapses the frame rate by design; those
runs are correctly invalid as measurements and used only for pixels. `compare-dumps.py` compares every
sampled cross-capture pair and reports the best match, because the scene animates.

| Comparison | Shared targets | ≤ 0.133/255 | scene colour `0x2060000000` | `0x2002800000` noise atlas |
|---|---:|---:|---:|---:|
| reuse=0 vs reuse=1 | 21 | 19 | **0.125/255** | 92.95/255 |
| control: reuse=0 vs reuse=0 | 19 | 18 | **0.474/255** | 91.55/255 |

The two flagged targets are the same two that differ between two **baseline** runs by the same
magnitude. `018_2002800000` is a per-session random 1024×1024 noise atlas, stable to 0.002/255 within
a run and different between runs; at 4 MiB it is also below the 16 MiB pool minimum, so it can never
be pooled. On the animated scene colour the reuse arm is *closer* to the baseline than a second
baseline run is. No rendering corruption attributable to reuse was found. Reuse was live while these
frames were captured (`reuse_hits` 30–37/s in the dumped windows).

## Lifetime and teardown

- Pooled images are released only from `TextureCache::DeleteImage`'s `Scheduler::DeferOperation`
  callback — the same point that previously called `vmaDestroyImage`, so reuse inherits that path's
  GPU-completion proof rather than adding a weaker one.
- Destruction order: `Presenter` declares `Instance instance` before
  `std::unique_ptr<Rasterizer> rasterizer`, and `Rasterizer` owns the `TextureCache`, so the cache and
  its images are destroyed before `Instance::~Instance` runs `ImageReusePool::Shutdown()` and then
  `vmaDestroyAllocator`. `GnmDriver::ResetPresenter` pauses guest threads and stops the GPU/present
  threads first.
- Both validation cells reached `SHADPS4_CLEAN_EXIT` with allocations still pooled and logged *the
  presenter is destroyed*, exit 0. No VUIDs, no device loss, no new error class versus baseline.

## Toggle, defaults and rollback

- Enable: `SHADPS4_DBG_IMAGE_REUSE` set to a non-empty value whose first character is not `0`.
  **Default is off** — unset, empty or leading `0` keeps the original immediate
  `vmaCreateImage`/`vmaDestroyImage` path, with changed allocation ownership nowhere.
- Bounds: `SHADPS4_DBG_IMAGE_REUSE_MIN_MB` (16), `SHADPS4_DBG_IMAGE_REUSE_MAX_ENTRIES` (6),
  `SHADPS4_DBG_IMAGE_REUSE_MAX_MB` (256). Invalid values fall back to the defaults.
- Rollback at runtime: unset the switch. Rollback in the tree: revert the feature commits; nothing
  outside `video_core` and `src/tests` is touched, and no on-disk state is created.
- **The switch is read once, at process startup** (`ImageReusePool::Enabled()` caches it, and the pool
  latches its bounds and its shutdown state for the life of the process). Setting or clearing it
  therefore has **no effect on a running instance: the emulator must be restarted to change it.**
  There is no in-game or menu control for it, and it is inert unless explicitly set.
- A build with the feature present but the switch unset behaves as the unmodified code does for
  allocation ownership, which is the intended deployment: ship the code, leave the switch to the
  user's environment until the runtime results warrant otherwise.

## Integration record

- Branch: **`perf/cpu03-integration-2026-11-20`**, based on current `origin/main` (`9e8034c0`) and
  fast-forwarded onto the validated tree; pushed for review, **not merged into `main`**.
- **Source commit carried: `534e44d0`** — the tree every runtime cell above was measured on, and the
  tree the recorded binary `E4AD3824…` was built from. `git diff 534e44d0 HEAD -- src` is empty on
  this branch.
- The unmeasured teardown change is preserved separately on `perf/image-reuse-teardown-unmeasured`
  (`5c9abe34`). It is **not** on this branch and not part of this integration.
- Feature commits: `4a42dfeb` reuse Vulkan images by exact create shape, `c74b83b6` implementation
  report, `85332841` avoid reuse bookkeeping for direct destroys, `7ea6958e` pool follow-up record,
  `2db4d41f` keep the RENDERSTAT recycler fields beside the reuse counters, `ba2c6189` include VMA
  headers for the reuse test suite, then this validation record.
- Tree scope: `src/src/video_core/renderer_vulkan/vk_image_reuse_pool.{h,cpp}`,
  `texture_cache/{image.cpp,image.h,image_stats.h,texture_cache.cpp}`, `renderer_vulkan/vk_instance.cpp`,
  `src/CMakeLists.txt`, `src/tests/{CMakeLists.txt,video_core/test_image_reuse_pool.cpp}`, plus docs.
  No descriptor, frame-pacing or D32→R32 change.
- Integration was additive: no conflicts, and no source modification was needed to integrate — the
  branch is the validated tree plus documentation.
- Ancestry verified before integrating: `534e44d0` → `5c9abe34` → `c4362abd` is a linear chain on
  `perf/aliased-image-reuse-2026-11-19`, and `origin/main` (`9e8034c0`) is an ancestor of
  `534e44d0`, so the integration branch could be fast-forwarded onto the validated tree exactly
  instead of being reconstructed. `5c9abe34` is therefore **not** an ancestor of this branch.
- Integration build: **PASS**, exit 0 (`_build/build.ps1 -Root wt-cpu03-int`), producing
  `build\shadPS4.exe` SHA-256
  `1C1D411E4D1EB2F614D79323DBAEB116750B18B264C2D00C2F8428B86E89F299`. No emulator was launched from
  this tree.
- Integration tests: `ctest --test-dir build-tests -R ImageReusePool --output-on-failure` on this
  tree — **7/7 passed**. The full test tree was not built; its pre-existing `<sys/time.h>` failure is
  unrelated to this change.

## Validation artifacts (preserved, not deleted)

Under `M:\Development\Deracine-work\_out\`:

- `cpu03-reuse-2026-11-20\` — the two `AF193672` cells (`arm0-g1`, `arm1-g1`) and the `E4AD3824` ON
  cell (`arm1-g2`): per-cell `run.json`, `shad_log.txt`, `arm-manifest.json`, `input-*.txt`;
  `run-arm.ps1`; the pixel captures under `*/lab/user/rt_dump-gameplay/capture_NN/` with their
  `compare-dumps.py`, `within-arm.py`, `pixel-ab.json`, `control.json` and the two extracted 4 MiB
  noise-atlas images in `evidence\`.
- `cpu03-sweep\` — the `E4AD3824` OFF cell (`backslash1`), the 300/200/100 game shadows (hardlinked,
  only `r.ScreenPercentage` differs), and the unfinished interleaved sweep harness:
  `sweep2.ps1`, `driver.ps1`, `analyze.py`, `fixed-menu.txt`, partial `cells.jsonl`, and the
  `probe*` runs used to isolate the harness defects.
- `_build\frame-time-profile-full-2026-11-18\` — the 300 % game shadow every cell used; unchanged.

SHA-256 prefixes of the files the numbers above are read from (so a later reader can tell whether the
evidence still matches this record):

| File | Bytes | SHA-256 (prefix) |
|---|---:|---|
| `cpu03-reuse-2026-11-20\arm0-g1\run.json` | 4668 | `5EDBE0E80EC5E684` |
| `cpu03-reuse-2026-11-20\arm0-g1\lab\user\log\shad_log.txt` | 8752670 | `485632B32ABD12CA` |
| `cpu03-reuse-2026-11-20\arm1-g1\run.json` | 4672 | `9847952A742B0A0A` |
| `cpu03-reuse-2026-11-20\arm1-g1\lab\user\log\shad_log.txt` | 10424272 | `CDECF8B57F4231DA` |
| `cpu03-reuse-2026-11-20\arm1-g2\run.json` | 4671 | `5B173EB047E43336` |
| `cpu03-reuse-2026-11-20\arm1-g2\lab\user\log\shad_log.txt` | 9496763 | `AA794BAF80B32DF0` |
| `cpu03-sweep\backslash1\run.json` | 4621 | `E3C79180F8F7B05E` |
| `cpu03-sweep\backslash1\shad_log.txt` | 8233582 | `A5E8789C3A05445B` |
| `cpu03-reuse-2026-11-20\pixel-ab.json` | 8272 | `4B7D99FEF0CC3B28` |
| `cpu03-reuse-2026-11-20\control.json` | 8159 | `E41FA8473DF3BB2D` |
| `cpu03-sweep\sweep2.ps1` (interleaved sweep, unfinished) | 5134 | `0BDD8C5F0BB6AD6E` |
| `cpu03-sweep\driver.ps1` (direct-launch driver used to isolate H1) | 6954 | `9B149CF3E0EAC082` |
| `cpu03-sweep\analyze.py` (per-cell distributions, not yet run on a full sweep) | 7232 | `051A99E8707CE822` |

## Not established

Headset acceptance; whether the gain appears at 100 %/200 %; the true magnitude of the gain under an
interleaved A/B/A at one resolution; behaviour if an `Instance` were recreated mid-process (none
exists in current call sites and `Shutdown` latches deliberately); peak memory above three retained
allocations (a second copy of one shape is retained when the cap allows it — bounded, and it cost no
hit rate here, so no eviction policy was added).

The harness defects encountered while trying to run the interleaved sweep are recorded separately in
`harness-defects.md`; none of them is a CPU-03 defect and none was fixed here.
