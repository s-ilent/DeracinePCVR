# Image reuse pool Phase-2 validation

Status: **VALIDATED (headless M2 gameplay, system GPU)** — build, focused unit tests, one A/B gameplay
cell pair, a render-target pixel A/B with its own same-arm control, and a clean-shutdown check with
images still pooled. No headset acceptance. No FPS gain is claimed; see *What this establishes*.

This replaces the earlier interim record, which was blocked before any GPU gate could run.

## Identity and build

- Worktree: `M:\Development\Deracine-work\wt-perf-reuse`
- Branch: `perf/aliased-image-reuse-2026-11-19`
- Emulator build: **PASS**, exit 0 (`_build/build.ps1`). `build\shadPS4.exe` SHA-256
  `AF1936720C44EF8C196773DB861322F96A03AD7775A300F8F1AABDFA0116FC6F`. One private copy per arm,
  hash-pinned; both arms ran that identical binary, differing only in `SHADPS4_DBG_IMAGE_REUSE`.
- `ctest --test-dir build-tests -R ImageReusePool --output-on-failure`: **7/7 passed**.
- The full test tree still fails on the pre-existing Windows portability error
  (`src/tests/libraries/test_usbd_event_loop.cpp:6`, `<sys/time.h>`); unrelated to this change.

## Gameplay cells

Tracked entry point `tools/benchmark/headless-gameplay.ps1`, unchanged; same isolated-lab template,
eboot, save slot 1 (`m00_00_10_10`), input script and profile in both arms; 300 % / MSAA 2 / vblank
120. `SHADPS4_DBG_OVERLAP=1` and `SHADPS4_GPU_TIMING=1` in both arms. Values below are means over the
last 60 one-second `RENDERSTAT` windows of each run, i.e. steady gameplay.

| Arm | Valid cell | Unique FPS | Draws / Mpx / passes | GPU busy | images_created | create_ms | destroy_ms | reuse hits | reuse bytes | vram_mb | blocks | exit |
|---|---|---:|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `arm0-g1` reuse=0 | yes | 34.32 | 1871.6 / 909.7 / 59.2 | 58.8 % | 135.8/s | 25.05/s | 64.20/s | 0 | 0 | 3333.6 | 1675 | 0 |
| `arm1-g1` reuse=1 | yes | 39.79 | 1866.3 / 950.9 / 60.7 | 68.2 % | 96.8/s | 6.43/s | 17.66/s | 68.2/s | ≤219.4 MiB | 3422.8 | 1672 | 0 |

Both cells are `valid: true, phase: gameplay`, `save_ok: true`, `reuse` arm 190/195 samples in band.

What the counters say:

- The two 74.6 MB create shapes are served from the pool: 68.2 hits/s ≈ 1.7 of the 4.1 image
  creations a frame, and `images_created` falls by 39/s. `reuse_misses` (96.8/s) counts every
  poolable-shaped request that found no entry, including the sub-16 MiB shapes that can never be
  pooled, so it tracks the small-image traffic rather than a shortfall — read it next to
  `images_created`, not on its own.
- `create_ms + destroy_ms` falls from **89.25 ms/s to 24.09 ms/s**. That is the direct, within-run
  measurement of the cost CPU-03 attributed to the recurring shapes; it is ~6.5 % of wall time and
  bounds the achievable frame-rate gain at about +7 % in this scene.
- Allocation ownership is bounded: `reuse_bytes` peaks at 219.4 MiB = three retained allocations
  against the source-enforced 256 MiB / 6-entry cap, and `vram_blocks` does not grow (1675 → 1672).
- GPU busy rises 58.8 % → 68.2 %, consistent with the renderer feeding the GPU better after the
  CPU-side allocation work is removed.

**The FPS pair is not evidence of an FPS gain.** The reuse arm's 39.79 sits on top of the whole
existing distribution of valid gameplay cells (34.0–40.8), and main at `3621ff29` — before this
feature — already measured 39.17 in `_out/p2/baseline-main-3621ff29-300`. The 34.32 baseline is
therefore a slow run, and the +16 % pair delta is mostly run-to-run variance above the +7 % bound
the timers allow. What is established is the removed allocator time, not a frame-rate win.

## Render-target pixel A/B

`SHADPS4_DBG_DUMP_HMD_AT_SCENE` gated on the gameplay band only (`1600,700`), so the menu phase ran at
full speed and dumping started once gameplay was reached. Dumping collapses the frame rate by
design; those runs are correctly invalid as measurements and are used only for pixels.
`compare-dumps.py` compares every sampled cross-capture pair per target and reports the best match,
because the scene animates and two runs never share a frame.

| Comparison | Shared targets | Targets ≤ 0.133/255 | Scene colour `0x2060000000` | `0x2002800000` noise atlas |
|---|---:|---:|---:|---:|
| reuse=0 vs reuse=1 (`arm0-d1` / `arm1-d2`) | 21 | 19 | **0.125/255** | 92.95/255 |
| control: reuse=0 vs reuse=0 (`arm0-d1` / `arm0-d2`) | 19 | 18 | **0.474/255** | 91.55/255 |

- The two flagged targets are the *same two* that differ between two **baseline** runs, by the same
  magnitude. `018_2002800000` is a 1024×1024 noise atlas: both arms' copies are coherent per-session
  random noise, stable to 0.002/255 *within* a run and different *between* runs. It is also
  structurally outside the pool — 4 MiB, under the 16 MiB minimum — so it can never be pooled.
- On the animated scene colour the reuse arm is **closer to the baseline than a second baseline run
  is**: 0.125/255 against a 0.474/255 control noise floor. Reuse does not perturb the rendered scene.
- Reuse was live while these frames were dumped: `reuse_hits` was 30–37/s in the dumped windows.

## Lifetime and teardown

- Pooled images are released only from `TextureCache::DeleteImage`'s `Scheduler::DeferOperation`
  callback, which is the same point that previously called `vmaDestroyImage` — reuse inherits that
  path's completion proof rather than adding a weaker one.
- Destruction order verified: `Presenter` declares `Instance instance` before
  `std::unique_ptr<Rasterizer> rasterizer`, and `Rasterizer` owns the `TextureCache`, so the cache and
  its images are torn down before `Instance::~Instance` runs `ImageReusePool::Shutdown()` and then
  `vmaDestroyAllocator`. `GnmDriver::ResetPresenter` pauses guest threads and stops the GPU/present
  threads first, so no release can race the drain.
- Both cells ran to `SHADPS4_CLEAN_EXIT` with images still pooled (`reuse_bytes` was 219.4 MiB in the
  final `RENDERSTAT`) and logged *the presenter is destroyed*, exit 0. No VUIDs, no device loss, no
  new error classes versus the baseline log (only file-system noise differs).
- `vram_mb` ends where it starts in both arms (747 → ~3.37 GB, peak mid-run), so the retained pool does
  not accumulate across the run.

## What this establishes, and what it does not

Established: the implementation builds, its focused tests pass, it is exercised in Déraciné gameplay
on the system GPU, it removes the measured allocator time, its memory is bounded, it does not change
the rendered image beyond run-to-run noise, and it tears down cleanly with allocations still pooled.

Not established: headset acceptance; any frame-rate gain (see above); behaviour when an `Instance` is
recreated mid-process (none exists in current call sites, and `Shutdown` latches deliberately); the
menu-scene coverage the earlier GPU-05 dump compared.

Peak retention is three allocations (219.4 MiB) for two recurring shapes because retention does not
reject a shape already pooled. It is bounded by the byte cap and cost no hit rate here, so no
eviction policy was added; raising `SHADPS4_DBG_IMAGE_REUSE_MAX_MB` still cannot exceed that cap.

## Reproduce

```
pwsh -NoProfile -File _build\build.ps1 -Root ...\wt-perf-reuse
bash tools/headless-vr/gpu-lock.sh run --owner <you> --kind headless --require-quiet -- \
     pwsh -NoProfile -File _out\cpu03-reuse-2026-11-20\run-arm.ps1 -Reuse 1 -Seq g1
```

Arms `g1` are the measurement cells; `d1`/`d2` are the pixel captures (add
`-SceneGate '1600,700,rt_dump-gameplay' -ExitAfterSeconds 180`). Evidence, raw logs, `run.json` per
arm and both comparison JSONs are under `_out\cpu03-reuse-2026-11-20\`.
