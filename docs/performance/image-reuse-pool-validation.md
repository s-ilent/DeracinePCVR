# Image reuse pool Phase-2 validation (interim)

Status: **PARTIAL / BLOCKED** — build, focused unit tests, and static review complete. GPU correctness/measurement gates were not run because the lead explicitly reserved the GPU lock for baseline and diagnostic work and has not released it. Do not interpret this interim record as a completed validation. No unmeasured gate is represented as passed.

## Identity and build

- Worktree: `M:\Development\Deracine-work\wt-perf-reuse`
- Branch: `perf/aliased-image-reuse-2026-11-19`
- Starting HEAD: `9618549e453f44709c0901a0e70dc43237efd427`
- Build command: `pwsh -NoProfile -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-perf-reuse`
- Emulator build: **PASS**, exit 0. `build\shadPS4.exe` SHA-256: `AF1936720C44EF8C196773DB861322F96A03AD7775A300F8F1AABDFA0116FC6F`.
- Test-tree command: `pwsh -NoProfile -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-perf-reuse -Tests`
- Full test-tree build: **FAIL**, unrelated existing Windows portability error: `src/tests/libraries/test_usbd_event_loop.cpp:6` cannot find `<sys/time.h>`.
- Focused test target initially exposed a missing VMA include directory. Fixed narrowly in `src/tests/CMakeLists.txt`, committed as `d6c9b455` (`fix(tests): include VMA headers for image reuse suite`). Then `ninja -C build-tests shadps4_image_reuse_pool_test` succeeded. `ctest --test-dir build-tests -R ImageReusePool --output-on-failure`: **7/7 passed** (exact create shape, each key field, queue-family list, view-format order, pNext/alias rejection, size threshold, bounds).
- The CMake-only test change does not alter the emulator binary; no post-code-change emulator rebuild was required.

## Static gates and defect review

- **A1 PASS (static):** `git grep -n 'slot_images.erase' -- src/src/video_core/texture_cache` returns exactly one hit, `texture_cache.cpp:1597`, inside `TextureCache::DeleteImage`'s `scheduler.DeferOperation` lambda.
- **Sticky pool after teardown:** no mid-session Vulkan `Instance` recreation found. `Presenter` owns `Instance` by value; the only `Presenter` construction is in `GnmDriver::RegisterLib`; teardown via `GnmDriver::ResetPresenter` stops GPU/present threads and is the `SHADPS4_CLEAN_EXIT` terminal shutdown path. `Instance::~Instance` drains the pool before destroying its allocator. Kept sticky shutdown unchanged; no dead-allocator allocation can be handed out. A hypothetical reinitialization during one process remains unproven, but no such path is present in current call sites.
- **UniqueImage move assignment:** operator still overwrites existing members without destroying them. Its four current assignment sites all assign to a `BackingImage` created immediately by `backing_images.emplace_back()`, whose `UniqueImage` is default-empty; no call site overwrites a live allocation. Left unchanged as unreachable in the current tree.
- **Eviction:** no eviction change. On bounds overflow, reject-and-destroy avoids arbitrary churn and retains the hard cap. Eviction would require a recency policy and could replace an exact-shape allocation with a mismatched one without proven allocation-pressure benefit; no evidence justifies adding it here.
- **A2 render-target dump compare:** PENDING GPU authorization.
- **A3 device loss/sync anomalies, long run:** PENDING GPU authorization.
- **A4 VRAM plateau:** PENDING GPU authorization.
- **A5 RENDERSTAT counters/legacy recycler fields:** source format has additive `reuse_*` and retains `recycler_hits`, `recycler_misses`, `pool_images`, `pool_mb`, `recycler_destroys`; runtime validation PENDING.
- **A6 PASS:** 7 device-free focused tests passed as above.

## A/B/A benchmark and correctness-only run

Not yet run. Lead is using the GPU lock for a baseline and focused diagnostic run; I will not acquire it until explicitly released. Planned workload: M2 gameplay, 300%, MSAA 2, vblank 120, same verified eboot/save/input/profile for all arms; `SHADPS4_GPU_TIMING=1`; `SHADPS4_DBG_IMAGE_REUSE=0`, then `1`, then `0`. No `SHADPS4_DBG_DUMP_HMD_AT_SCENE` / `-EyeDump` on measuring cells, and never combine `SHADPS4_DBG_CALL_TIMES=1` with `SHADPS4_DBG_XFER=1`.

| Arm | Valid samples | Unique FPS | Draws / Mpx / passes | GPU busy | Command / GPU executing | create_ms / destroy_ms | images_created / images_destroyed | reuse hits / misses / bytes / destroyed | VRAM MB / blocks |
|---|---:|---:|---|---|---|---|---|---|---|
| A1 reuse=0 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| B reuse=1 | pending | pending | pending | pending | pending | pending | pending | pending | pending |
| A2 reuse=0 | pending | pending | pending | pending | pending | pending | pending | pending | pending |

Executable SHA-256 and exact environment, arguments, output paths, raw `RENDERSTAT`/frame logs, and sample counts will be added per cell after runs. Correctness-only capture comparison, plus long-run VRAM and sync/device-loss evidence, will be added separately.

## Verdict (provisional)

The implementation compiles, the focused device-free pool suite passes, and A1 passes static inspection. The option remains off by default and bounds are source-enforced. **Not yet validated as runtime-safe, not yet a measured win.** A2–A5 runtime proof and the matched A/B/A remain unproven pending GPU lock availability. Headset acceptance is also not performed by this headless phase.
