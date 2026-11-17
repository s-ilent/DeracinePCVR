# Vulkan image recycler — review and standalone candidate

Source: `6170df6` on `investigation/cross-fork-renderer` (worktree `wt-crossfork`), parent
`fcdf3de`, based on the contaminated overnight line `de9c78c`.

Standalone candidate: worktree `M:\Development\Deracine-work\wt-recycler-candidate`, branch
`candidate/image-recycler-2026-11-17` at **`4d244d1`**, cut from the trusted source baseline
`acda49e`. `de9c78c` was **not** cherry-picked. The diagnostics dependency (`49ec266`'s
`ImageStats`/`ScopedTimer`) is stripped, so the candidate builds on `acda49e`; the instrumentation
needed to report the metric is a separate, optional commit (see "Missing gates").

## What the original does

Four Vulkan images are created and destroyed per classroom frame; the two large shapes alternate
(5760×3240 `R32Sfloat` ↔ `D32SfloatS8Uint`, ~74.6 MB each). `ImageRecycler` pools released
`vk::Image` + `VmaAllocation` by exact creation shape and hands one back instead of calling
`vmaCreateImage` (create 0.78 ms + destroy 2.5 ms per frame on the crossfork build, measured with
its per-second counters).

## Invariant review

| invariant | how it is enforced | verdict |
|---|---|---|
| Key is the whole create shape | `SameShape` compares flags, imageType, format, extent, mip levels, layers, samples, tiling, usage, sharing mode, queue-family count/indices, initial layout, and the `view_formats` list | sound |
| pNext is part of the identity | deliberately excluded; the only pNext this fork creates is the stack `VkImageFormatListCreateInfo`, and its list is compared separately. The extraction **destroys instead of pooling any release whose create info has a non-null pNext**, so a future caller cannot make the key read a dangling chain | sound after hardening; original was fragile |
| Creation pNext data | `UniqueImage::Create` builds the format list on the stack; the stored `image_ci` is the caller's (pNext null in every current caller). Verified: the only image-creation pNext uses are the format list; instance-level pNext uses are unrelated | sound today |
| Allocation compatibility | Entry carries its `VmaAllocator`; the new `UniqueImage` adopts it. One device/allocator in this process; the pool is shut down before `vmaDestroyAllocator`. A multi-device/allocator process would need an allocator key or an assert | assumption, not enforced |
| GPU completion | release runs in `~UniqueImage`; cache images are erased only from `scheduler.DeferOperation`, which runs after the master semaphore reports the tick complete; views are erased before the image | sound for cache images; relies on every destroy path being deferred |
| Contents / layout reset | a recycled image goes into a fresh `Image`; `BackingImage::State.layout` starts at `eUndefined`, and the first barrier declares `oldLayout = eUndefined`, which Vulkan defines as discard. Nothing that says *what is in* the image (validity, hashes, GpuModified, layout state, meta) is pooled | sound |
| Image/view/descriptor lifetimes | views are destroyed before the image in the deferred erase; descriptors are not pooled. A stale descriptor would now point at recycled memory instead of freed memory — same pre-existing hazard, but a recycled image can make it read old pixels rather than fault | note, no new guarantee |
| Eviction | bounded at 16 images / 768 MB; a full pool replaces the cheapest entry when the incoming is larger and fits, otherwise destroys the incoming; same-shape release replaces the older pooled one | sound; accounting checked |
| Teardown | `Instance::~Instance` calls `Shutdown()` before `vmaDestroyAllocator`; `shutdown` is sticky, so a device recreation declines to pool. Teardown order checked in the base: `Rasterizer` owns `TextureCache` as a member, so cache images are destroyed while the `Instance` that owns the allocator is still alive; `~Instance` then shuts the pool down before the allocator. That same ordering is what makes the base's immediate `vmaDestroyImage` safe | sound; the static pool itself has no destructor, but it is empty after Shutdown |
| Budget | pooled allocations stay in VMA's budget and in VRAM; measured ~250–293 MB in the crossfork cells (6.92–6.94 GB total either way) | bounded, but not free |
| Thread safety | one mutex; `Acquire`/`Release`/`Snapshot` lock; release happens on the deferred-operation thread | sound |

## Findings and risks

1. **Major — the advertised 4.15 → 0.29 ms saving is not measured on a controlled pair.** The
   original off arm (`xf-a2-0-r2`) landed on the calibration screen (176 draws / 273 Mpx), while
   the on arms are the classroom (535 draws / 500 Mpx). The timers wrap the right calls, but the
   workloads differ, so 3.9 ms cannot be attributed to recycling. The commit/report says this; the
   task's warning is correct.
2. **Major — no unit test.** The key comparison, same-shape replacement, size-based eviction and
   the pNext guard have no automated test. A pure test needs the class decoupled from Vulkan
   (or a device), which the original does not do. The candidate inherits that gap.
3. **Major — GPU-completion proof is structural, not asserted.** It is true today because the only
   `slot_images.erase` is inside `DeferOperation`. A future direct `Image` destruction (e.g., cache
   teardown, a new fast path) would pool an image the GPU is still using. Recommend a debug check
   or keeping the pool release behind a scheduler tick token.
4. **Minor — allocator identity.** `Acquire` adopts the pooled entry's allocator without checking
   it against the caller's. One device makes this moot; multi-device does not.
5. **Minor — `SameShape` reads `pQueueFamilyIndices`.** Pooled entries always have count 0 (split
   images are destroyed on release), so the loop is not entered; the invariant is the only thing
   keeping the pointer safe.
6. **Minor — static_vector capacity.** `view_formats` is a `static_vector<vk::Format, 4>`; more
   than four formats would overflow on `assign` (pre-existing in `UniqueImage::Create`).
7. **Minor — `Acquire` does not check `shutdown`.** Safe today because `Shutdown` clears the pool,
   but the state machine is implicit.
8. **Minor — stale descriptors.** A recycled image keeps the memory alive; a descriptor that
   should have been retired would read recycled pixels instead of crashing. This makes the
   GPU-completion invariant more important, not less.

## Missing gates before any verdict

- **Build** of `4d244d1` on this machine (not run yet: the user is using the headset, and a full
  compile is not allowed to contend with that session).
- **Unit test** for the key/eviction/pNext guard (or an explicit decision to accept review-only).
- **Same-binary, same-scene, interleaved A/B** with `SHADPS4_DBG_IMAGE_RECYCLER=0/1`, no competing
  builds, each cell screenshot-verified, repeated; report separately:
  - allocations per frame (create calls, recycler hits/misses/pool),
  - CPU wall time in `vmaCreateImage`/`vmaDestroyImage` (needs the counters from `49ec266` or an
    equivalent timer ported into the candidate),
  - GPU timing (timestamps around the same calls, not inferred from CPU),
  - VRAM used (VMA budget + pool bytes),
  - FPS (with the noise floor; a single pair is not enough).
- **Capture A/B** of the same scene with the recycler off/on, render-target dumps bit-identical.
  The crossfork attempt was a partial pass (9 of 9 dumpable images identical, but the classroom
  itself was not captured with the recycler).

## Verdict

The design is plausible and the invariants are mostly sound; the extraction on `acda49e` is a
self-contained, reversible candidate. **It is not yet a verified optimisation.** The 3.9 ms figure
must not be quoted: it compares different scenes. The recycler can be integrated only after the
gates above, and only as an explicitly switchable default-on candidate with the pool budget
documented.

Candidate: `candidate/image-recycler-2026-11-17` @ `4d244d1`; one commit; no other branch touched.
