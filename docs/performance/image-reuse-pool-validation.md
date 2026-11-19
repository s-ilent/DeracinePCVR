# Image reuse pool Phase-2 validation

Status: **VALIDATED (headless M2 gameplay, system GPU)** — build, focused unit tests, three gameplay cells, a render-target pixel A/B with its own same-arm control, and a clean-shutdown check with
images still pooled. No headset acceptance. No FPS gain is claimed; see *What this establishes*.

This replaces the earlier interim record, which was blocked before any GPU gate could run.

## Identity and build

- Worktree: `M:\Development\Deracine-work\wt-perf-reuse`
- Branch: `perf/aliased-image-reuse-2026-11-19`
- Emulator build: **PASS**, exit 0 (`_build/build.ps1`). Cells `g1` ran
  `build\shadPS4.exe` SHA-256 `AF1936720C44EF8C196773DB861322F96A03AD7775A300F8F1AABDFA0116FC6F`;
  rebasing onto current `main` (docs-only) forced a full relink, and cell `g2` re-validated that
  artifact, `E4AD38248B21AFE60F74E987DBBE3B79BD90AF86E81D096208F457D83CAD4C51`. `src/` is
  byte-identical across the rebase (`git diff 80bfc9c2 HEAD -- src` is empty), so the two binaries
  are the same code. One private copy per arm, hash-pinned; both arms differ only in
  `SHADPS4_DBG_IMAGE_REUSE`.
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
| `arm1-g2` reuse=1 | yes | 30.99 | 1866.1 / 913.2 / 58.6 | 50.0 % | 65.1/s | 1.68/s | 2.82/s | 59.6/s | ≤219.4 MiB | 3374.9 | 1531 | 0 |

`arm1-g2` is the confirmation cell run on the rebuilt binary of the pushed commit (see *Identity*).
Three cells are enough to see what the frame rate does and does not track.

What the counters say:

- The two recurring large create shapes are served from the pool: hits are 60–68/s ≈ 1.7–1.9 of the
  ~4 image creations a frame, and `images_created` falls from 135.8/s to 65–97/s. `reuse_misses`
  counts every poolable-shaped request that found no entry, including the sub-16 MiB shapes that can
  never be pooled, so it tracks the small-image traffic rather than a shortfall — read it next to
  `images_created`, not on its own.
- `create_ms + destroy_ms` falls from **89.25 ms/s to 24.09 ms/s (`g1`) and 4.50 ms/s (`g2`)**. That
  is the direct, within-run measurement of the cost CPU-03 attributed to the recurring shapes, and it
  is the real result of this work.
- Allocation ownership is bounded: `reuse_bytes` peaks at 219.4 MiB = three retained allocations
  against the source-enforced 256 MiB / 6-entry cap, and `vram_blocks` does not grow (1675 → 1672).

**No frame-rate gain is claimed, and these cells show why:** the same configuration measured 39.79
and 30.99, and the low-FPS cell is the one where the allocator time was almost entirely gone
(4.50 ms/s). Frame rate here does not track the allocator cost — it varies by ~9 FPS run to run,
the reuse cells span the whole existing distribution of valid gameplay cells (30.99–40.76), and main
at `3621ff29` — before this feature — already measured 39.17. The three-cell spread is the finding:
89 ms/s of `vmaCreateImage`/`vmaDestroyImage` was **not** the frame limiter at this scene size and
render percentage, which agrees with the earlier recycler result. The optimization removes real CPU
time; it does not buy frames in this cell.

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
It also does **not** raise frame rate in this cell: the cell with almost no allocator work left
(`g2`) is the slowest of the three, so the 89 ms/s it removes was not the limiter.

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

