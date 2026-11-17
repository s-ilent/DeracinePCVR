# Track B — upstream texture-cache staging

Branch `integration/upstream-texture-staging`, worktree `M:\Development\Deracine-work\wt-upstream-stage`,
cut from `integration/cross-fork-renderer` at `1a53ceb`. Source of every port: `wt-sync` at
`24b8620`, verified clean before use. This branch is a staging area, not a candidate for the runtime
until Track A's baselines exist — the two tracks must never be compared against each other.

## What landed, in the reconciled order

| # | commit here | source | what it fixes | applied |
|---|---|---|---|---|
| 1 | `a487b11` | `7ec75b2` (#5453) | guests may declare more mip levels than the extent's chain; the extra 1×1 levels are illegal in Vulkan and uploads ran past the image | clean |
| 2 | `42c6ba4` | `24b8620` | an image view's base layer was not clamped to the image's layers | clean (only applies after 1) |
| 3 | `3e97a0c` | `09c7ecc` (#5495) | `CarvePhysArea` asserted when a range spanned several physical areas; `Free` never advanced past skipped areas | clean |
| 4 | `b6f90d2` | `6be27a6` (#5599) | only the image starting exactly at the GPU-written address was marked dirty; planes after it showed an old frame | clean |
| 5 | `c437dc1` | `e94f26d` (#5433) | a subresource bound as a render target was freed instead of being carried into the parent image | clean |
| 6 | `79379ce` | `8ccb420` | the hazard 5 introduces: a held image reference across `SlotVector::insert` dangles | clean (only applies after 5) |
| 7 | `048303c` | `9af68f2` (#5444) **+ both guards** | a CPU-dirty (stale) source image was copied over the fresh upload | one conflict, resolved below |
| 8 | `2f6464b` | new companion | the parent #5433 fills from GPU-written data was not marked `GpuModified`, so a later expansion under #5444's gate would drop what #5433 preserved | new |
| 9 | `0c89560` | `4610df7` | the #5453 regression tests | test file clean, registration merged |

## The two guards that had to land with #5444

`#5444` makes the copy in `ExpandImage` conditional on the source holding GPU-written data. This
branch also skips *uploads* where that copy lands, so the two decisions are now one:

```cpp
const bool src_has_gpu_data = src_image.SafeToDownload();
const bool carries_contents = src_has_gpu_data || src_image.info.num_samples > 1;
const CopyCoverage covered = covered_refresh && carries_contents ? CoveringCopyOf(...) : CopyCoverage{};
...
ASSERT(carries_contents || covered.CoversNothing());
if (carries_contents) { new_image.CopyImage(src_image); }
```

- The coverage is derived from `carries_contents`, so when the copy is skipped the upload skip
  collapses to "skip nothing". Without it, destination levels would be written by nothing — and
  silently, because the skip is opt-in.
- The multisampled term exists because `RefreshImage` returns before clearing `Dirty` for
  `num_samples > 1`: `SafeToDownload()` is false for every MSAA image, and the copy is then the only
  writer it will ever get.
- The new image inherits `GpuModified` from the same condition, not from the raw source flag,
  because the copy also runs for multisampled sources and those images do hold GPU data.

## Conflicts and how they were resolved

1. **`9af68f2` in `ExpandImage`** — an insertion-point collision, not a disagreement: the patch
   inserts its gate where this branch inserts its tracing and coverage block, and both guard the
   same copy call. Resolved by keeping the branch's tracing and coverage code and putting the
   patch's condition in front of both decisions (above). Resolution script:
   `_build\crossfork\resolve-5444.py`.
2. **`4610df7` in `src/tests/CMakeLists.txt`** — both commits append a test target at the same
   anchor. Both are kept: `shadps4_image_coverage_test` first, `shadps4_mip_chain_test` second, each
   complete with its own include directories, links and discovery block. The test file itself
   applied cleanly. Resolution script: `_build\crossfork\resolve-mip-cmake.py`.

No patch was applied partially, and nothing is hidden behind a disabled switch.

## Tests (this branch)

- Emulator: clean Windows build (clang-cl, LLVM 23), `shadps4.exe` links.
- Test tree: `ctest` → **596 tests, the same 10 documented pre-existing failures** as the base
  (7 targets that cannot build on this platform, `ReadConstSnapshotDiag.IsolationDisabledAfterTestB`,
  `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`). No new failure.
- New suites: `MipChain` 6/6 and `ImageCoverage` 19/19 pass (`ctest -R "MipChain|ImageCoverage"` →
  25/25, 0.19 s).
- Both recycler configurations compile and pass the CPU-side tests: the switch is a runtime
  environment variable (`SHADPS4_DBG_IMAGE_RECYCLER=0|1`), not a compile-time one.

## What this branch does *not* establish

It has never been run under the GPU. Everything here is a compile-and-unit-test result: the runtime
behaviour of #5433/#5444/#5599 (image contents, dirty-state transitions, dangling references under
slot-vector growth) needs the same kind of scene/gameplay run Track A is waiting for. Do not quote
Track A's numbers for this branch or the reverse; they are different renderer baselines.

## Recommended merge order into the runtime, once validated

1. `a487b11`, `42c6ba4`, `3e97a0c`, `b6f90d2` — independent correctness, no interaction with the
   rest (take them alone if the rest stalls).
2. `c437dc1` + `79379ce` — they must travel together.
3. `048303c` — only with the guards, never separated.
4. `2f6464b` — the companion; without it #5433's benefit evaporates on the next expansion.
5. `0c89560` — tests.
