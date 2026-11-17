# Upstream reconciliation: five shadPS4 fixes against `integration/cross-fork-renderer`

Read-only analysis. No builds, no emulator, no GPU, no test runs, no code changes. This document is
the only file created.

## 0. Trees, commits, method

| Role | Tree | Ref | SHA |
|---|---|---|---|
| Live runtime ("live base") | `M:\Development\Deracine-work\DeracinePCVR` | `integration/r58-presentsize` | `de9c78c` |
| Unmerged upstream sync | `M:\Development\Deracine-work\wt-sync` | `integration/upstream-sync-2026-11` | `24b8620` (base `acda49e`) |
| Integration candidate | `M:\Development\Deracine-work\wt-crossfork-int` | `integration/cross-fork-renderer` | `2498976` at the start of this analysis (see note) |

All three are worktrees of one repository (`M:\Development\Deracine-work\DeracinePCVR\.git`), so
every commit's blobs are available from any of them; the upstream shadps4 commits the sync branch
ports from (`699009c`, `d3ff0e0`, `ee0364a`, `6add101`, `851c9ff`) are also present locally under
the `upstream` remote, which is what let the *claims* be checked against the real upstream diffs
rather than against the commit messages.

**Note — the candidate branch moved during the analysis, and kept moving.** At the start of this
session `wt-crossfork-int` was at `2498976` with one uncommitted file; another agent then committed
that work and rewrote the tail of the branch repeatedly while this document was being written (tips
observed: `e813b58`, `de64a7a`+`3d375a4`, `117a3bb`, `d3123a8`, `98888e2`). All line numbers cited
here are for the three-commit state `2498976` that the brief describes, unless a passage says "at
the tip". What the later commits change in the files this document reasons about:

* `image_stats.h`, `vk_scheduler.cpp`, `vk_image_recycler.{h,cpp}` — diagnostics and the recycler,
  not the texture-cache semantics below;
* `image.cpp` / `image.h` — only the `UniqueImage` recycler-lifetime code
  (`UniqueImage::{~,Destroy,Create,operator=}`, roughly `image.cpp:111-200`, `image.h:57-95`), not
  `Image`'s `flags`, `mip_hashes`, `Upload`, `CopyImage` or `SetBackingSamples`;
* `src/tests/CMakeLists.txt` — `d3123a8` adds three include-path lines to A1's coverage-test block;
* `texture_cache.cpp` — `7a2b042` adds the `ASSERT` and the port comment in `ExpandImage` that
  §3.3/§3.6 refer to as already landed, and an earlier commit adds `ImageStats::SetAllocator` at
  line ~191.

So no quoted region is contradicted; the later commits shift line numbers and, in two places this
document points at explicitly, add exactly the invariant it recommends. Applicability was therefore
re-checked against a scratch clone pinned at `2498976`
(`%TEMP%\deracine-int-2498976`, a `--shared` clone, so no tree under `M:\` was touched or
dirtied by the checks), and the two conflicted commits were re-checked once more at the final tip
(same result, §1).

Method for item 2: `git -C <tree> show <sha> -- src | git -C <tree> apply --check --3way -`.
`--check` never writes the working tree, and the index was verified untouched by comparing
`git write-tree` before and after the whole batch (identical: `8c63985d…`). Because `--3way`
reports a *merge* rather than a failure, **exit status is not a usable signal** (all eight
patches exited 0, two of them with conflicts); the per-file message is the signal. Each commit was
also applied for real, cumulatively, in the recommended order in the scratch clone, to separate
"conflicts alone" from "conflicts in sequence".

Present/absent in `de9c78c` was decided by reading the code at the affected functions, never by
`git branch --contains`.

## 1. Summary table

| # | Fix | Commits | Present in `de9c78c`? (evidence) | Present in `wt-sync` (`24b8620`)? | Applies to candidate `2498976`? | Conflicts | Recommended order |
|---|---|---|---|---|---|---|---|
| 1 | #5453 mip-chain clamp | `7ec75b2` | **No.** `resource.h:240-245` returns `last_level + 1` unclamped (`244`); `image_view.cpp:49` uses `image.base_level` raw | Yes — `resource.h:240-251`, `image_view.cpp:49` | **Clean** (`resource.h`, `image_view.cpp`) | none | 1 |
| 1b | view base-layer clamp (extra, not in the brief) | `24b8620` | **No.** `image_view.cpp:50` uses `image.base_array` raw | Yes — `image_view.cpp:50` | **Clean after #5453**; conflicts alone | 1 hunk (`image_view.cpp`), context is #5453's rewritten line | 2 |
| 2 | #5495 fragmented physical areas | `09c7ecc` | **No.** `memory.cpp` carves once per call: `899-901`, `990-992`, `998-999`, `1372`; `Free` skips only `Free` (`340`) | Yes — chunk loops at `875-885`, `962-986`, `1381`; `Free` at `331-348` | **Clean** (`memory.cpp`) | none | 3 |
| 3 | #5599 refresh images stored after a GPU-written image | `6be27a6` | **No.** `texture_cache.cpp:343-354` flags only the image whose base address equals the written address | Yes — `texture_cache.cpp:275-293` | **Clean** | none | 4 |
| 4 | #5433 keep subresource data when the parent is created | `e94f26d` | **No.** `texture_cache.cpp:923-928` still "We need to have a larger, already allocated image to copy this one into" | Yes — `texture_cache.cpp:663-675` | **Clean** | none | 5 |
| 4b | #5433 hardening (re-fetch after insert) | `8ccb420` | **No** (same latent `CopyMip(cache_image, …)` at `926`) | Yes — `texture_cache.cpp:674` | **Clean only after #5433**; conflicts alone | 1 hunk depending on `e94f26d` | 6 |
| 5 | #5444 don't copy stale CPU-dirty contents in `ExpandImage` | `9af68f2` | **No.** `texture_cache.cpp:1000` copies unconditionally; `1009` clears `Dirty` and never sets `GpuModified` | Yes — `texture_cache.cpp:691-712` | **Conflicts** (`ExpandImage`) | 1 hunk: candidate's `covered`/`carries_contents` block vs the patch's gate | 7 |
| 6 | tests for #5453 | `4610df7` | n/a (tests did not exist) | Yes — `test_mip_levels.cpp` + CMake block | **Test file clean, `src/tests/CMakeLists.txt` conflicts** | 2 hunks, same insertion anchor as A1's `shadps4_image_coverage_test` | 8 |

"Clean after X" means: applied standalone, the 3-way merge reports conflicts; applied on top of X
in sequence, it reports "cleanly". Verified for `24b8620` after `7ec75b2` and `8ccb420` after
`e94f26d`.

## 2. Per-fix notes, with the code that decides each answer

### 2.1 #5444 — `9af68f2` — "don't copy stale CPU-dirty contents in ExpandImage"

**Live base `de9c78c`, `src/src/video_core/texture_cache/texture_cache.cpp:1000`:**
the copy is unconditional and the `GpuModified` hand-over is missing entirely:

```cpp
 997:     if (!copy_covers_all || !skip_covered_refresh) {
 998:         RefreshImage(new_image);
 999:     }
1000:     new_image.CopyImage(src_image);
...
1008:     TrackImage(new_image_id);
1009:     new_image.flags &= ~ImageFlagBits::Dirty;
1010:     return new_image_id;
```

The port's claim that "ExpandImage had dropped the GpuModified hand-over entirely" is true, and
it is a *fork regression*: the neighbouring recreate path still does it —
`texture_cache.cpp:645-650` (`ResolveDimensionOverlap`):

```cpp
 645:     // Fold any pending CPU or buffer-cache changes into the old backing before moving its
 646:     // authoritative contents through a dimension-neutral buffer copy.
 647:     RefreshImage(source_image);
 648:     new_image.CopyImageWithBuffer(source_image, copy_buffer.Handle(), 0);
 649:     new_image.flags &= ~ImageFlagBits::Dirty;
 650:     new_image.flags |= source_image.flags & ImageFlagBits::GpuModified;
```

**`wt-sync` `texture_cache.cpp:691-712`** (the port — faithful to upstream `699009c`):

```cpp
 691:     // Only carry over the old contents when they hold GPU-written data that guest memory lacks.
 692:     // A CPU-dirty source is stale (e.g. a streamed texture rewritten in a reused pool slot);
 693:     // copying it would overwrite the fresh upload with old data.
 694:     const bool src_has_gpu_data = src_image.SafeToDownload();
 695:
 696:     RefreshImage(new_image);
 697:     if (src_has_gpu_data) {
 698:         new_image.CopyImage(src_image);
 699:     }
...
 707:     TrackImage(new_image_id);
 708:     new_image.flags &= ~ImageFlagBits::Dirty;
 709:     if (src_has_gpu_data) {
 710:         new_image.flags |= ImageFlagBits::GpuModified;
 711:     }
```

The equivalence claim in the commit message is correct: upstream's
`HasGpuValidData()` (`699009c:src/video_core/texture_cache/image.h`) and the fork's
`SafeToDownload()` (`image.h:134-136` in all three trees) are the same expression:

```cpp
bool SafeToDownload() const {
    return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
}
```

Upstream `699009c` differs from the port in one respect worth recording: upstream's *pre-image*
already contained `new_image.flags |= src_image.flags & ImageFlagBits::GpuModified;`, and #5444
makes it conditional; the fork had lost the line, so the port both adds the gate and restores the
hand-over. Also, upstream clears `Dirty`/sets `GpuModified` **before** `FreeImage`, the fork after:
no semantic difference.

**Verdict for item 1: absent from `de9c78c`.** Applicability: conflicts (one hunk), see §3.6.

### 2.2 #5433 — `e94f26d` (+ `8ccb420` hardening)

**Live base `de9c78c`, `texture_cache.cpp:923-928`:**

```cpp
 923:                 // We need to have a larger, already allocated image to copy this one into
 924:                 if (merged_image_id) {
 925:                     auto& merged_image = slot_images[merged_image_id];
 926:                     merged_image.CopyMip(cache_image, mip, slice);
 927:                     FreeImage(cache_image_id);
 928:                 }
```

When `merged_image_id` is empty nothing is copied and the subresource's GPU contents are simply
dropped; the parent is later created at `texture_cache.cpp:1107-1110` from guest memory.

**`wt-sync` `texture_cache.cpp:663-675`:**

```cpp
 663:                 // If we don't have an allocated image, create one here. Otherwise it is created
 664:                 // later from guest memory, the GPU data of this subresource is lost, and reads
 665:                 // and writes end up in different images.
 666:                 if (!merged_image_id) {
 667:                     merged_image_id = slot_images.insert(instance, scheduler, blit_helper,
 668:                                                          slot_image_views, image_info);
 669:                     RegisterImage(merged_image_id);
 670:                     RefreshImage(slot_images[merged_image_id]);
 671:                     TrackImage(merged_image_id);
 672:                 }
 673:                 auto& merged_image = slot_images[merged_image_id];
 674:                 merged_image.CopyMip(slot_images[cache_image_id], mip, slice);
 675:                 FreeImage(cache_image_id);
```

`8ccb420` is the fix for a hazard the port *introduces* (`SlotVector::insert` may move every
`Image`, invalidating `cache_image`), and it is correct: upstream has the same latent hazard.
The live base does not need it because it never inserts in this block.

**Adaptation note (positive):** `RefreshImage(slot_images[merged_image_id])` compiles unchanged
against the candidate, whose declaration is
`texture_cache.h:151: void RefreshImage(Image& image, const CopyCoverage* covered = nullptr);`
— the one-argument call resolves to `covered == nullptr`, i.e. the pre-A1 behaviour.
`ResolveOverlap` is entered with `mutex` held (`FindImage`, candidate `texture_cache.cpp:1068`);
`RefreshImage` does not take the mutex itself, so the port introduces no self-deadlock.

**Verdict: absent from `de9c78c`.** Applies cleanly (and its hardening applies cleanly after it).

### 2.3 #5453 — `7ec75b2` (+ `4610df7` test) — and the extra `24b8620`

**Live base `de9c78c`, `src/src/video_core/amdgpu/resource.h:240-245`:**

```cpp
 240:     u32 NumLevels() const {
 241:         if (GetType() == ImageType::Color2DMsaa || GetType() == ImageType::Color2DMsaaArray) {
 242:             return 1;
 243:         }
 244:         return last_level + 1;
```

**`wt-sync` `resource.h:240-251`:**

```cpp
 244:         // Guests may declare more levels than the extent's full mip chain has (the extra ones
 245:         // would be 1x1), which Vulkan does not allow and makes uploads run past the image.
 246:         const u32 mip_width = width + 1;
 247:         const u32 mip_height = height + 1;
 248:         const u32 mip_depth = GetType() == ImageType::Color3D ? depth + 1 : 1;
 249:         const u32 max_levels = std::bit_width(std::max(std::max(mip_width, mip_height), mip_depth));
 250:         return std::min<u32>(last_level + 1, max_levels);
```

The live base already clamps one consumer, the view *extent* (`resource.h:334-336` in `de9c78c`;
the same three lines sit at `wt-sync resource.h:340-342` because the clamp above pushes them down),
but not the chain itself, and not the view's base level/array index
(`de9c78c image_view.cpp:49-50`):

```cpp
 49:     range.base.level = image.base_level;
 50:     range.base.layer = image.base_array;
```

versus `wt-sync` after `7ec75b2` + `24b8620`, `image_view.cpp:49-50`:

```cpp
 49:     range.base.level = std::min<u32>(image.base_level, image.NumLevels() - 1);
 50:     range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
```

`24b8620` is **not in the brief's list** but is in the branch, and it is the natural partner of
`7ec75b2`: both clamps read the same descriptor's bitfields (`resource.h:150-169`: `width`,
`height`, `depth`, `last_level`, `base_level`, `base_array`), so a view is only fully validated
once both are clamped. The fork also already had `NumViewLevels` clamp its *extent* against
`NumLevels()` (`resource.h:334-336`), so `7ec75b2` closes the last unclamped consumer of the same
value. It is worth landing (two lines, one theory), but it is a *hardening* commit, not a port of
an upstream PR — treat it as an extra if branch scope matters.

**Why the clamp matters to the candidate specifically:** `ImageInfo::resources.levels` is
assigned straight from `NumLevels()` (`image_info.cpp:178: resources.levels = image.NumLevels();`)
and `UpdateSize()`/`mips_layout` iterate that count (`image_info.cpp:201-203`). So the clamp is
upstream of `RefreshImage`'s per-mip loop (`texture_cache.cpp:1453`), `CopyImage`'s region count
(`image.cpp:895`), and A1's coverage (`image_coverage.h:108`). This is the one ordering constraint
that is genuinely semantic: **#5453 should precede the coverage work it feeds.** In the actual
topology A1 is already committed, so #5453 lands *after* it; the final state is still correct
(see §4), but any measurement taken with `SHADPS4_DBG_COVERED_REFRESH=1` before #5453 would be
measuring coverage counts derived from inflated level counts.

**Portability note:** the patch uses `std::bit_width` and `std::min` without adding
`<bit>`/`<algorithm>`; `resource.h:6-10` includes neither directly. It builds in `wt-sync` on
transitive includes; the candidate includes the same headers, so it will almost certainly build,
but that is a compile fact, not a read-a-file fact (see §7).

**Verdict: absent from `de9c78c`.** `7ec75b2` applies cleanly; `24b8620` applies cleanly after it.

### 2.4 #5599 — `6be27a6` — "refresh images stored after a GPU-written image"

**Live base `de9c78c`, `texture_cache.cpp:343-354`:**

```cpp
 343: void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
 344:     std::scoped_lock lock{mutex};
 345:     ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
 346:         // Only consider images that match base address.
 347:         // TODO: Maybe also consider subresources
 348:         if (image.info.guest_address != address) {
 349:             return;
 350:         }
 351:         // Ensure image is reuploaded when accessed again.
 352:         image.flags |= ImageFlagBits::GpuDirty;
 353:     });
 354: }
```

**`wt-sync` `texture_cache.cpp:275-293`** walks forward over the images whose base address is
exactly the next address in the written range:

```cpp
 279:     const VAddr end = address + max_size;
 280:     VAddr next = address;
 281:     while (next < end) {
 282:         VAddr next_end = 0;
 283:         ForEachImageInRegion(next, end - next, [&](ImageId image_id, Image& image) {
 284:             if (image.info.guest_address != next) {
 285:                 return;
 286:             }
 287:             // Ensure the image is reuploaded when accessed again.
 288:             image.flags |= ImageFlagBits::GpuDirty;
 289:             next_end = std::max<VAddr>(next_end, next + image.info.guest_size);
 290:         });
 291:         next = next_end > next ? next_end : end;
 292:     }
```

Only two callers exist (`buffer_cache.cpp:362`, `vk_rasterizer.cpp:1251`), so there is no other
place in the live base that flags trailing planes.

**Interaction with the dirty flags (this is the reason for its position in §4):** the flag it sets,
`GpuDirty`, is *inside* `ImageFlagBits::Dirty` (`image.h:37`), and `SafeToDownload()` requires
`False(flags & Dirty)`. So #5599 is what keeps a Y/U/V-plane image that the GPU wrote through the
buffer cache from looking "safe to download" after the fact. #5444 consumes exactly that
predicate; landing #5444 before #5599 would leave a window in which #5444's new gate answers
"yes, copy" for a plane whose authoritative contents are in the buffer cache.

**Verdict: absent from `de9c78c`.** Applies cleanly.

### 2.5 #5495 — `09c7ecc` — "carve fragmented physical areas slice by slice"

**Live base `de9c78c`** carves the whole range as one area at each site and checks only `Free`:

| Site | `de9c78c` | `wt-sync` |
|---|---|---|
| `Free` | `340: if (dmem_area.dma_type == PhysicalMemoryType::Free) {` … `347: continue;` (no `remaining_size` advance) | `331-348`: computes `size_in_dma` first, skips `Free \|\| Pooled \|\| Committed`, `346: remaining_size -= size_in_dma;` |
| `PoolDecommit` | `899: const auto new_dmem_handle = CarvePhysArea(dmem_map, phys_addr, size_in_dma);` | `875-885`: `while (chunk_addr < chunk_end)` … `CarvePhysArea(dmem_map, chunk_addr, chunk)` |
| `UnmapBytesFromEntry` (direct) | `990: CarvePhysArea(dmem_map, phys_addr, size_in_dma)` | `962-972`: same chunked loop |
| `UnmapBytesFromEntry` (flexible) | `998: CarvePhysArea(fmem_map, phys_addr, size_in_dma)` | `976-986`: same chunked loop |
| `SetDirectMemoryType` | `1372: CarvePhysArea(dmem_map, phys_handle->second.base, phys_handle->second.size);` | `1378-1386`: chunked loop (`phys_addr`/`phys_end`) |
| `DirectMemoryQuery` | `1271-1276`: skips `Free` only; rejects only `Free` | `1255-1268`: skips `Free \|\| Pooled \|\| Committed`; rejects anything that is not `Mapped`/`Allocated` |
| `GetDirectMemoryType` | `1443: if (dmem_area.dma_type == PhysicalMemoryType::Free)` | `1455-1481`: same three-way reject, plus `in_run` and the walk forward/backward to report the whole run |

All four claims in the sync commit message are borne out by the diff: (a) `CarvePhysArea` is
called once per range, which asserts if `dmem_map` has been split inside it; (b) `Free` could
release `Pooled`/`Committed` area (only `Free` was skipped); (c) `Free` did not advance
`remaining_size` while skipping, so a free/pool area in the middle silently shortened the walk
rather than consuming the request; (d) the two query functions report one slice, not the whole
contiguous run of one memory type.

**Verdict: absent from `de9c78c`.** Applies cleanly. Independent of the video-core work (only
`src/src/core/memory.cpp`).

### 2.6 What is *not* in the branch and would be easy to assume is

`de9c78c`'s `ExpandImage` already contains a *different*, env-gated experiment
(`SHADPS4_DBG_SKIP_COVERED_REFRESH`) that asks whether the copy covers *everything*
(`texture_cache.cpp:990-999`), and A1's commit message records that this predicate "could never
fire" for an expansion. It is not #5444 and it is not the coverage skip; A1 left it in place and
the candidate still has it at `texture_cache.cpp:994-1000`. Do not read its presence as partial
integration of anything.

## 3. Semantic composition: #5444 against A1's copy coverage

### 3.1 The mechanism, as the code actually orders it

`TextureCache::ExpandImage` (`texture_cache.cpp:940-1059` in the candidate) does, in order:

1. `slot_images.insert(...)` — a **new** image `N` with `flags = ImageFlagBits::Dirty`
   (`image.h:189`), i.e. all three dirty bits set, contents undefined.
2. Compute `carries_contents` (`1005`, currently the literal `true`) and
   `covered = covered_refresh && carries_contents ? CoveringCopyOf(src, dst) : CopyCoverage{}`
   (`1013-1015`). `covered_refresh` is `SHADPS4_DBG_COVERED_REFRESH=1`, **off by default**
   (`1009-1012`).
3. `RefreshImage(N)` or `RefreshImage(N, &covered)` (`1039-1045`).
4. `if (carries_contents) new_image.CopyImage(src_image);` (`1046-1048`).
5. `TrackImage(N); N.flags &= ~Dirty;` (`1056-1057`) — no `GpuModified` today.

And inside the called code:

* `RefreshImage` bails before doing anything if `False(flags & Dirty) || num_samples > 1`
  (`texture_cache.cpp:1416-1418`).
* The coverage skip is `if (covered != nullptr && covered->CoversLevel(m, num_layers)) continue;`
  (`1460-1464`); `CoversLevel` is `level < mips && layers >= dst_layers`
  (`image_coverage.h:37-39`), i.e. a level is skipped only when the copy writes **all** of that
  level's destination layers.
* If every level was skipped, `image_copies` is empty and `RefreshImage` just clears `Dirty`
  (`1502-1505`) — **nothing is uploaded**.
* `Image::Upload` writes the copies and clears `Dirty` (`image.cpp:693-702`).
* `Image::CopyImage` (`image.cpp:860-1037`) performs only the blit; **it never touches either
  image's `flags`**. It returns early only for `src == this`.
* `Image::SetBackingSamples` is a no-op when the sample count already matches
  (`image.cpp:1293-1295`: `if (!backing || backing->num_samples == num_samples) return;`), so the
  copy cannot silently re-create the destination backing and drop the levels that were just
  uploaded.

So the two mechanisms interact through exactly one variable, and A1 built the seam for it on
purpose (`texture_cache.cpp:1001-1005`):

```cpp
1001:     // Whether the new image takes the old image's contents at all. Written as one condition
1002:     // because the coverage below is only sound while the copy that follows really runs: upstream
1003:     // #5444 gates this copy on the source holding GPU-written data (src_image.SafeToDownload()).
1004:     // If that is ported, gate this condition with it and the skip stays sound.
1005:     const bool carries_contents = true;
```

### 3.2 State combinations

Preconditions for the table: the geometry permits a coverage (`CoveringCopyOf` returns non-empty:
same kind, colour only, same `num_bits`/`num_samples`/blockedness, source extent ≥ destination
extent) and `SHADPS4_DBG_COVERED_REFRESH=1`. "Skip" = `RefreshImage` omits the guest-memory upload
for the levels the copy covers. "Guarded" = `carries_contents` taken from
`src_image.SafeToDownload()`; "unguarded" = #5444's copy gate adopted while `carries_contents`
stays `true` (the literal today).

| # | Source `S` flags | Copy (with #5444) | Skip | Result, **guarded** | Result, **unguarded** |
|---|---|---|---|---|---|
| a | `GpuModified=1`, `Dirty=0` | runs | skips covered levels | covered levels hold `S`'s GPU data, the rest guest memory — **correct** | same — correct |
| b | `GpuModified=1`, `MaybeCpuDirty=1` only | skipped | guarded: `covered` is empty → full upload. unguarded: skips covered levels | `N` = guest memory everywhere — correct (conservative) | covered levels written by **nothing**: `N` is undefined there — **broken** |
| c | `GpuModified=1`, `CpuDirty=1` (the #5444 bug) | skipped | as (b) | `N` = the fresh guest upload, which is the whole point of #5444 — correct | **broken**, as (b) |
| d | `GpuModified=1`, `GpuDirty=1` | skipped | as (b) | `N` = guest memory as written by the GPU through the buffer cache — correct | **broken**, as (b) |
| e | `GpuModified=0`, `Dirty=0` (created, uploaded, never GPU-written) | skipped | as (b) | `N` = guest memory — correct | **broken**, as (b) |
| f | `GpuModified=0`, `CpuDirty=1` | skipped | as (b) | `N` = guest memory — correct | **broken**, as (b) |
| g | `num_samples > 1` (any flags) | **skipped always**: `SafeToDownload()` is always false for such an image, because `RefreshImage` returns at `1416` for `num_samples > 1` and therefore never lets `Upload` clear `Dirty` | `RefreshImage(N)` also returns at `1416` if `N` is multisampled, so the coverage is never even consulted | **broken**: nothing writes `N` at all — no upload, no copy | **broken**, same way |
| h | `SafeToDownload()=1`, destination has **more layers** than the source (within a level) | runs | no skip: `CoversLevel` requires `layers >= dst_layers` | all levels uploaded, then the copy overwrites the shared layers — correct | same — correct |
| i | `SafeToDownload()=1`, destination has **more levels** (the normal expansion) | runs | skips levels `< min(src.levels, dst.levels)` | copied levels = `S`'s GPU data, extra levels = guest memory — correct | same — correct |
| j | any of a–f, `SHADPS4_DBG_COVERED_REFRESH` unset | either | no skip (`covered` empty) | full upload always, so `N` is always written — correct | same — correct, which is why the latent bug is invisible today |

Two facts make row (g) sharper than it looks: `RefreshImage`'s early return is *before* any
`Dirty` clear, so a multisampled image can never become download-safe; and `CoveringCopyOf` only
declines on a *sample-count mismatch* (`image_coverage.h:85-87`), so two multisampled images of
the same sample count do get a coverage — which then cannot help, because the refresh it is
supposed to replace never uploads anyway. `ExpandImage` is reachable for a multisampled image
through the equal-address "size/resources are greater" path (`de9c78c texture_cache.cpp:770-774`),
which only requires the same `type` and strictly greater `resources`; that is narrow (it needs an
array type, since `NumLayers()` for `Color2DMsaa` is 1) but not excluded by any assertion.

### 3.3 Verdict

**Needs a guard — two of them, both small. Not sound as-is; not unsound once guarded.**

1. **The coverage guard (one line).** Gate `carries_contents` on the same predicate #5444 gates
   the copy with. Because `covered` is already computed from `carries_contents`
   (`texture_cache.cpp:1013-1015`), this single change makes the skip collapse to "skip nothing"
   exactly when the copy will not run, so rows (b)–(f) degrade to a full refresh rather than an
   undefined destination. This is the edit A1's own comment asked for.
2. **The multisampled guard (one condition).** Even with the coverage guard, row (g) leaves `N`
   never written, because `RefreshImage` refuses multisampled images and the copy is now
   conditional. The cheap fix keeps today's behaviour for that case:
   `const bool carries_contents = src_has_gpu_data || src_image.info.num_samples > 1;`
   The alternative (make `RefreshImage` upload multisampled images) is a different, larger change
   and outside this reconciliation.

The invariant is no longer hypothetical: while this analysis was being written, commit `7a2b042`
added `ASSERT(carries_contents || covered.CoversNothing());` to `ExpandImage` and spelled the
required port expression out in the comment above `carries_contents` (now
`texture_cache.cpp:1011` at the tip, which still reads `const bool carries_contents = true;`). The
assert is inert while `carries_contents` is `true`; it exists to turn an unguarded adoption of
#5444 into a debug abort instead of the silent undefined destination described by rows (b)–(g).

### 3.4 Why the unguarded adoption is not caught today

`covered_refresh` is opt-in (`texture_cache.cpp:1009-1012`), so with the switch off `covered` is
always empty and every row of §3.2 is a full upload. Adopting #5444 without the guard would
therefore ship green and only corrupt contents when someone turns the measurement switch on — the
worst failure mode for a "one-line port". This is the strongest argument for landing the guard in
the same commit as the port rather than after it.

### 3.5 Second-order finding: #5433's parent plus #5444's gate

`e94f26d` creates the parent `P`, refreshes it from guest memory, and then copies one subresource
into it (`wt-sync texture_cache.cpp:666-675`). Neither `RegisterImage`, `RefreshImage` nor
`CopyMip` sets `GpuModified` (`image.cpp:1122-1164` touches no flags; `RefreshImage` only clears
`Dirty` via `Upload`). So immediately after #5433:

* `P.flags`: `GpuModified = 0`, `Dirty = 0` → `SafeToDownload(P) == false`,
* but `P` demonstrably holds GPU-authored texels for that one mip/slice, which guest memory does
  not have (that is the whole reason #5433 exists).

If `P` is later passed to `ExpandImage` — reachable, because `P` is now a cache image that can
itself be expanded — #5444's gate answers "false", the copy is skipped, and the subresource data
that #5433 just preserved is dropped. This is **not a regression against the live base** (there,
the data was already lost when the parent was created), but it means #5433's benefit evaporates
on the next expansion unless the parent is marked. The minimal companion edit, after the
`CopyMip`:

```cpp
                merged_image.flags |= ImageFlagBits::GpuModified;
```

That is also what makes the copy authoritative for later refreshes: `RefreshImage`'s
GPU-modified hash guard (`texture_cache.cpp:1472-1479`) compares *guest memory* against
`mip_hashes`, so marking `P` keeps the copied subresource from being overwritten by a later
re-upload of unchanged guest memory. A stricter variant, which matches #5444's invariant at this
second copy site, gates the copy itself:

```cpp
                const bool src_has_gpu_data = slot_images[cache_image_id].SafeToDownload();
                if (src_has_gpu_data) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(slot_images[cache_image_id], mip, slice);
                    merged_image.flags |= ImageFlagBits::GpuModified;
                }
                FreeImage(cache_image_id);
```

This is a deliberate deviation from the upstream port (upstream copies unconditionally), so it
should be a separate, labelled commit rather than folded into the cherry-pick. Note that the
`slot_images[cache_image_id]` re-fetch is required either way — that is what `8ccb420` fixes.

### 3.6 The `ExpandImage` conflict, and the exact resolution

Applied alone, `9af68f2` conflicts in one hunk. The patch inserts its gate immediately after
`auto& new_image = slot_images[new_image_id];`, which is exactly where the candidate has since
inserted `ReprTrace`/`AliasTrace`/`copy_covers_all`/`carries_contents`; the other conflict edge is
the copy call itself, because the candidate's copy is already guarded by `carries_contents`.
Nothing in the conflict is a semantic disagreement — it is an insertion-point collision. Keep the
candidate's block and make three edits, in the candidate's own layout (line numbers at `2498976`):

1. At `1001-1005`, replace the comment and the literal — this is the whole of #5444's gate, and
   because `covered` is computed from it, no separate `RefreshImage` change is needed:

```cpp
    // #5444: only carry the old contents over when they are GPU-written data guest memory lacks.
    // A CPU-dirty source is stale, and copying it would overwrite the fresh upload with old data.
    // RefreshImage declines multisampled images (texture_cache.cpp:1416-1418), so for those the
    // copy is the only writer of the new image and it must run regardless.
    const bool src_has_gpu_data = src_image.SafeToDownload();
    const bool carries_contents = src_has_gpu_data || src_image.info.num_samples > 1;
```

2. Before the copy at `1046`, make the composition checkable — **already landed**, as `7a2b042`:

```cpp
    ASSERT(carries_contents || covered.CoversNothing());
    if (carries_contents) {
        new_image.CopyImage(src_image);
    }
```

3. At `1056-1057`, hand over `GpuModified` under the same condition as the copy:

```cpp
    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    if (carries_contents) {
        new_image.flags |= ImageFlagBits::GpuModified;
    }
```

Using `carries_contents` (rather than the patch's `src_has_gpu_data`) for the `GpuModified`
hand-over matters: in the multisampled case the copy does run and `N` does hold GPU data, so
leaving the flag clear would make the new image permanently un-downloadable *and* would let a later
`RefreshImage` overwrite the copied contents from guest memory.

### 3.7 A minor `mip_hashes` consequence

After #5444, `N` is `GpuModified` with `Dirty` clear, so `RefreshImage`'s hash guard
(`1472-1479`) starts applying to it. Levels the coverage skip left un-uploaded never had
`mip_hashes[m]` recorded (it is zero-initialised at construction, `image.cpp:261`), so on the first
later refresh those levels see a mismatch and are re-uploaded. That is conservative and correct —
a refresh only runs when a dirty bit says something changed — it just means the skip's saving is
not inherited. Worth knowing before reading a profile, not a correctness issue.

## 4. Recommended cherry-pick order

All five fixes land **after** the three candidate commits; A1 (`275d654`), the diagnostics
(`f0d5f98`) and the recycler (`2498976`) are already the base. The recycler is orthogonal: it
recycles `UniqueImage` backings only, while `flags`/`mip_hashes` belong to the per-image `Image`
object, which is constructed fresh on every insert (`image.cpp:252-261`, `image.h:189`), and at
`2498976` the backing is pooled only from the destructor, which the texture cache runs from
`scheduler.DeferOperation`, i.e. after the GPU is done with the previous owner (the later `117a3bb`
keeps pooling on that path and destroys immediately, without pooling, where completion cannot be
proved). Nothing in the five fixes needs a guard against it.

| Step | SHA | Reason |
|---|---|---|
| 1 | `7ec75b2` | Descriptor-level, no dependencies, and it is upstream of every consumer of `resources.levels` — `ImageInfo` (`image_info.cpp:178`), the `mips_layout` loop (`201-203`), `RefreshImage`'s per-mip loop, `CopyImage`'s region count, and A1's `CoveringCopyOf`. Semantically this should have preceded A1; in this topology it can only follow it, and the final behaviour is the same. |
| 2 | `24b8620` | Its context is `7ec75b2`'s rewritten line, so it is clean only here. Same descriptor-clamp theory; two lines. |
| 3 | `09c7ecc` | Independent file (`src/src/core/memory.cpp`), clean, and unrelated to the texture cache — landing it early keeps the memory-manager change out of the texture-cache review. |
| 4 | `6be27a6` | Must precede #5444: it is what makes `SafeToDownload()` honest for images the GPU wrote through the buffer cache (it sets `GpuDirty`, which is inside `Dirty`), so #5444's gate cannot be fooled into copying a stale plane. |
| 5 | `e94f26d` | Clean, and independent of #5444 in code, but settle the copy sites and their flags before gating copies. Its `RefreshImage(slot_images[…])` call needs no adaptation (default `covered = nullptr`). |
| 6 | `8ccb420` | Clean only after step 5, and it repairs a hazard step 5 introduces. Never land it alone. |
| 7 | `9af68f2` + guard edits (§3.2/§3.6) | Last code change; the only conflicted code hunk; the guard edits are meaningless without it and dangerous apart from it. |
| 8 | companion edit (§3.5) | Optional, separate commit: `GpuModified` on the parent created by #5433 (and, if wanted, the `SafeToDownload` gate on that copy). |
| 9 | `4610df7` | Tests for step 1; the file applies cleanly, the CMake block does not (§5). Land with or after step 1. |

The only true ordering constraints are (a) `7ec75b2` before `24b8620`, (b) `e94f26d` before
`8ccb420`, (c) `6be27a6` before `9af68f2`, and (d) `7ec75b2` before any *enabled* use of the
coverage path. Everything else is about keeping conflicted or semantically entangled work last.

## 5. Tests

`4610df7` carries two files and they behave differently against the candidate:

* `src/tests/video_core/test_mip_levels.cpp` — a **new file, applies without conflict** (created
  cleanly during the check). It includes only `video_core/amdgpu/resource.h` and exercises
  `AmdGpu::Image::NumLevels()`; it does not touch `ImageInfo`, the cache, or Vulkan, so nothing in
  the candidate's A1/recycler work is in its way. I verified the six expectations against the
  clamp formula read from `wt-sync resource.h:246-250` (all consistent: 64→7, declared 3→3,
  100→7, 1×1→1, 3D 64→7, MSAA→1 through the pre-existing early return), but the commit's claim
  that "four of the six cases fail against the pre-clamp `NumLevels()`" is a claim about a run I
  cannot perform here.
* `src/tests/CMakeLists.txt` — **conflicts, and should not be cherry-picked as-is.** The patch
  inserts its block after `gtest_discover_tests(shadps4_image_subresource_test …)`, which is
  exactly where A1 already inserted `shadps4_image_coverage_test`
  (`src/tests/CMakeLists.txt:1091-1121` at `2498976`; `d3123a8` later added three include-path
  lines to that same block, which does not move the anchor). Two merge regions result (the header +
  `add_executable` block, and the link/`TEST_TARGETS` block), and re-checking at the final tip
  gives the same two-region conflict. The resolution is mechanical: keep A1's block and add the
  mip block after it; both use the same `list(APPEND TEST_TARGETS …)` + `gtest_discover_tests`
  pattern, so the merged file is a plain concatenation.

What needs adapting: essentially nothing beyond that insertion point. Every library the patch
links (`GTest::gtest_main`, `Vulkan::Headers`, `fmt::fmt`, `spdlog::spdlog`,
`magic_enum::magic_enum`, `tsl::robin_map`, `half::half`) is already used by a sibling target in
the same file (`shadps4_image_subresource_test`, `src/tests/CMakeLists.txt:1070-1082`), so the
targets exist in this tree. The target name does not collide with A1's
`shadps4_image_coverage_test`. `half::half`/`tsl::robin_map` are not obviously needed by a test
that only calls `NumLevels()`, so trimming to A1's shorter link list (`GTest`, `Vulkan::Headers`,
`magic_enum`) is optional hygiene, not a requirement.

What the tests do **not** cover: the composition of §3. `test_image_coverage.cpp` (A1) tests the
predicate in isolation — it answers "where would a copy write", never "would a copy run". No test
in either branch would fail if `carries_contents` and the copy gate disagreed. Closing that gap
needs either a cache-level test (out of reach of the current header-only test targets) or the
`ASSERT(carries_contents || covered.CoversNothing())` from §3.3, which at least turns a silent
undefined destination into a debug abort.

## 6. What must not be duplicated

The brief asks whether A1's predicate (`image_coverage.h`) and "part of upstream #5444's
`overwrites` reasoning" are complementary or contradictory. **They are complementary, and the
premise that #5444 contains overwrite reasoning is not supported by the commit as it exists in
this repository.** Upstream `699009c` is 11 added lines plus a comment; the only occurrence of
"overwrite" in the file is in that comment, and the function it changes has no coverage logic:

```
src/video_core/texture_cache/texture_cache.cpp (upstream 699009c):
+    // Only carry over the old contents when they hold GPU-written data that guest memory lacks.
+    // A CPU-dirty source is stale (e.g. a streamed texture rewritten in a reused pool slot);
+    // copying it would overwrite the fresh upload with old data.
+    const bool src_has_gpu_data = src_image.HasGpuValidData();
     RefreshImage(new_image);
-    runtime.CopyImage(&src_image, &new_image);
+    if (src_has_gpu_data) {
+        runtime.CopyImage(&src_image, &new_image);
+    }
```

`upstream/main` (2026-11-16) has the same shape and still no coverage predicate. So:

* A1 answers **where** the copy writes — `image_coverage.h:24-39`: "How much of a destination
  image a whole-image copy is going to write", `CoversLevel(level, dst_layers)`.
* #5444 answers **whether** the copy happens at all — `SafeToDownload()` /
  `HasGpuValidData()`, identical expressions (`image.h:134-136`, `699009c image.h`).
* Neither can replace the other, and neither contradicts the other: the coverage is sound *only
  while* the copy runs, which is precisely the variable #5444 introduces. A1 wrote the seam for
  it (`texture_cache.cpp:1001-1005`: "the coverage below is only sound while the copy that follows
  really runs: upstream #5444 gates this copy … If that is ported, gate this condition with it and
  the skip stays sound").

Nothing to deduplicate. The one thing not to do is re-implement "does the copy overwrite this
level" inside the #5444 port — that reasoning already exists, once, in `image_coverage.h`, and the
ported commit does not contain it.

## 7. Not determined from files alone

1. **Whether `ExpandImage` is ever reached with `num_samples > 1` in Déraciné** (§3.2 row g). The
   path exists in code and no assertion excludes it; whether this title takes it needs the
   `SHADPS4_DBG_MSAA`/`ALIAS` traces or a debugger.
2. **Whether the coverage path has ever run on a real frame with `SHADPS4_DBG_COVERED_REFRESH=1`**
   with these state combinations. A1's commit message reports zero covering shapes at 300% on the
   classroom scene; that is a claim about a run, not something the files show.
3. **Whether `std::bit_width`/`std::min` in the `7ec75b2` port resolve without adding
   `<bit>`/`<algorithm>`** in the candidate's translation unit. It builds in `wt-sync` on
   transitive includes; confirming it in the candidate needs a compile, which is excluded here.
4. **Whether #5495's memory-manager changes are behaviourally safe for this title's mapping and
   pool patterns.** The code change is verified against the diff; the runtime consequences
   (pool carve granularity, `DirectMemoryQuery` reporting longer runs) are not.
5. **Whether the `#5433` parent-creation path actually keeps its data long enough to be expanded**
   (§3.5). The code path is clear; the frequency is not.
6. **Lock-order consequences of calling `RefreshImage` from inside `ResolveOverlap`** (§2.2),
   which now calls into the buffer cache and scheduler while the texture-cache mutex is held.
   Upstream does the same, so it is not a fork-specific risk, but no static reading proves it.
7. **Whether the sync branch `wt-sync` has been built or run at all.** There is no CI evidence in
   either tree; the commits describe ports, not validations.
8. **The upstream PR discussion for #5444** (whether an "overwrites" argument was made in review
   and dropped). Only the commit is available locally; the commit contains no such reasoning
   (§6).
9. **Timing/frame-rate effect of the coverage skip on this title.** Not measurable here, and A1's
   own measurement says the shapes it applies to do not occur at 300% on the classroom scene.

## 8. Git cleanliness

No source tree was modified by this analysis; the only file created or edited is this document.
`git apply --check --3way` was verified index-neutral by comparing `git write-tree` before and
after the batch (identical, `8c63985d…`), and every real application was done in
`%TEMP%\deracine-int-2498976`, a `--shared` clone, never in a worktree under `M:\`. The scratch
clone was reset with `git reset --hard` between checks and ended clean.

Final `git status --porcelain` (taken at the end of the analysis):

| Tree | HEAD | Result |
|---|---|---|
| `DeracinePCVR` | `de9c78c` | pre-existing only, untouched by me: ` M pc-vr/shadps4.exe` and six untracked files (`docs/AUTONOMOUS.md`, `o`, `pc-vr/render-config.ps1`, `pc-vr/shadps4.exe.before-r58`, `pc-vr/shadps4.v0.21.0-bundled.bak`, `pc-vr/user/config.json.bak`) |
| `wt-sync` | `24b8620` | clean |
| `wt-crossfork-int` | `98888e2` (was `2498976` when the brief was written) | ` M docs/research/upstream-reconciliation.md` — this document, and nothing else. Another agent committed an earlier revision of it in `98888e2` (`docs(research): …`) while the analysis was finishing; the modification shown is the accuracy pass applied afterwards (the already-landed `7a2b042` assert, the final tip, two corrected line citations). No source file was touched by me. |

So the only dirtiness this analysis leaves anywhere is the deliverable itself, in a tree shared
with an agent that also edits it.

The candidate worktree is shared with an agent that rewrote the branch repeatedly while this was
being written (tips observed: `e813b58`, `de64a7a`+`3d375a4`, `117a3bb`, `d3123a8`, `98888e2`), so
its `HEAD` and dirty set change independently of this document; the values above are a snapshot,
not a guarantee. Nothing in this analysis staged, committed, rebased, checked out or reverted
anything in any tree; I never ran a git command that writes.
