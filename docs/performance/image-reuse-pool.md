# Bounded Vulkan image reuse pool — implementation report

## Commit and scope

- Implementation commit: `20063f67436e9239c08c4965909e75ba89264b6e` (`perf(video_core): reuse Vulkan images by exact create shape`)
- Branch/worktree: `perf/aliased-image-reuse-2026-11-19` / `wt-perf-reuse`.
- Status: **UNBUILT / UNTESTED** by instruction. No build, compiler, CMake configure, emulator, test, benchmark, or Vulkan validation run was performed.
- This report is in the worktree because the shared audit-folder destination is outside the implementation agent's write boundary.

## Mechanism

`Vulkan::ImageReusePool` is a function-local-static, mutex-protected pool of image/allocation ownership records. Its `Entry` contains only the create-shape key and view-format list, `vk::Image`, `VmaAllocation`, originating `VmaAllocator`, and allocation byte count. It carries no `ImageInfo`, image validity/hash flags, `GpuModified`, layout, image views, or metadata.

`UniqueImage::Create` preserves the existing `ASSERT(!image)`, copies its create info and view formats, then attempts `Acquire`. On a hit it adopts the pooled image/allocation and saved `VmaAllocationInfo::size`; it does not call `vmaCreateImage`, add `create_ns`, or call `NoteImageCreated`. On a miss it follows the existing VMA create path and records the actual allocation size. Thus `images_created` continues to mean actual VMA image creations, not new logical owners.

`UniqueImage::Destroy()` and `~UniqueImage()` now share one idempotent implementation. An image is marked eligible for reuse only by `TextureCache::DeleteImage` inside its deferred callback. If marked and `Release` accepts it, the source object's image, allocation, allocation byte count, and reuse marker are cleared; subsequent `Destroy()` calls and the destructor see a null image and return without a second release or free. If it is not marked, or the pool rejects it, the normal timed `vmaDestroyImage` path runs and clears all ownership handles. The FSR resize call sites in `host_passes/fsr_pass.cpp` call `Destroy()` directly and are not marked, so they are not pooled.

Move construction and move assignment transfer the Vulkan/VMA handles, byte count, queue-index storage, and reuse marker with `std::exchange` for source-owned scalar fields. The moved-from object has a null image and cannot release the handle a second time. The receiving object retains the allocator from the moved allocation. Queue-family-index storage is copied at `Create`, and `image_ci.pQueueFamilyIndices` is rebound after the copy and moves; the pool entry separately owns its queue-family-index vector.

A successful release retains an allocation only if it is at least the configured minimum and fits both configured bounds. When full, the newly released image is destroyed by the caller; no live pooled entry is evicted. All pool destruction uses the allocator recorded with the entry. `Acquire` also checks that allocator identity matches.

## Feature switch and bounds

- Enable only with `SHADPS4_DBG_IMAGE_REUSE` set to a nonempty value whose first character is not `0`. Unset/empty/leading `0` is **OFF**.
- `SHADPS4_DBG_IMAGE_REUSE_MIN_MB`: default **16 MiB**.
- `SHADPS4_DBG_IMAGE_REUSE_MAX_ENTRIES`: default **6**.
- `SHADPS4_DBG_IMAGE_REUSE_MAX_MB`: default **256 MiB**.
- Invalid numeric values fall back to the defaults. The one-line pool startup log is emitted from the function-local pool's constructor and reports enabled state and bounds.
- When disabled, `Acquire` misses and `Release` refuses retention; the existing VMA create/destroy calls and their timers/counters remain the path. There is an intentional one-time pool construction/logging overhead, so “same behavior” here means no changed allocation ownership or rendered contents, not byte-for-byte identical process logging/timing.

## Exact matching / rejections

`SameCreateShape` is a pure helper in `vk_image_reuse_pool.h`, included by a device-free gtest. It compares every requested create field: `flags`, `imageType`, `format`, `extent.width/height/depth`, `mipLevels`, `arrayLayers`, `samples`, `tiling`, `usage`, `sharingMode`, `queueFamilyIndexCount`, every queue-family index in order, and `initialLayout`. View formats are compared as an **unordered multiset** (copied, sorted, and compared); duplicates remain significant.

Both shapes are rejected for comparison if `pNext != nullptr`, either has `eAlias`, the sharing mode is not `eExclusive` or `eConcurrent`, or a nonzero queue-family count has a null index pointer. The release path applies the same shape rejection. This rejects format-list chains, external-memory chains and other unenumerated pNext extensions. No pointer address is part of the key. Queue indices are never dereferenced when the count is zero.

The size gate uses the allocation size returned by `VmaAllocationInfo::size`, not a calculation from extent/format. This is intended to admit the measured ~74.75 MiB repeated images and reject both 0.125 MiB LUT images without hardcoding extent or guest address.

## GPU completion and teardown

Source inspected:

- `src/src/video_core/texture_cache/texture_cache.cpp`, `TextureCache::DeleteImage` (around lines 1588–1598): creates a `scheduler.DeferOperation` callback, erases views there, then erases `slot_images` there. The new `MarkGpuIdleForReuse()` call is within that callback, immediately before the `Image` slot erase.
- `src/src/video_core/renderer_vulkan/vk_scheduler.h`, `Scheduler::DeferOperation` (around lines 657–662): captures `CurrentTick()` with the callback.
- `src/src/video_core/renderer_vulkan/vk_scheduler.cpp`, `Scheduler::PopPendingOperations` (around lines 1099 onward): callbacks are only popped/executed after `master_semaphore.IsFree(pending_ops.front().gpu_tick)` succeeds. `Finish()` waits for the tick and calls `PopPendingOperations`; regular polling likewise only reclaims already-completed operations.

The structural argument is therefore: cache-image pooling is allowed only after the cache's deferred slot erase callback runs; that callback is gated by completion of the GPU tick that could still reference the image. The explicit reuse-safe bit prevents generic immediate `UniqueImage::Destroy` callers from relying on that proof. This remains a source-structure invariant, not a runtime scheduler token or independently verified synchronization proof.

`Instance::~Instance` in `vk_instance.cpp` calls `ImageReusePool::Get().Shutdown()` immediately before `vmaDestroyAllocator(allocator)`. `Shutdown` latches `shutdown`, destroys every retained image using its recorded allocator, clears entries/bytes, and prevents later acquisition or retention. Both `Acquire` and `Release` check that latch while holding the pool mutex. The function-local static pool destructor deliberately performs no Vulkan calls: there is no static-destruction fallback, because process static destruction ordering relative to Vulkan allocator lifetime is not proven. If normal `Instance` shutdown is skipped, the vector's trivial entry teardown can leak the retained VMA allocations; that is safer than calling Vulkan after allocator destruction, but is an outstanding teardown risk.

## Instrumentation and projected effect

`ImageStats` now reports `reuse_hits`, `reuse_misses`, `reuse_bytes` (current pooled-byte gauge), and `reuse_destroyed`. The counters are gated by the existing `SHADPS4_DBG_OVERLAP` reporting switch. `reuse_destroyed` counts an opted-in eligible release that was not retained plus images destroyed during pool shutdown. Pool-held images call `NoteImageDestroyed` only when VMA actually destroys them.

A repository search found `NoteImageCreated`, `NoteImageDestroyed`, and the report's values are only consumed by `ImageStats::Report()` in the texture-cache `RENDERSTAT` path. The VMA creation/destruction totals are thus visible only there. Under reuse, `images_created` remains actual VMA creates, but `images_destroyed` is intentionally delayed until a held allocation is actually destroyed; their counts will not line up in each one-second window. `reuse_hits` and `reuse_bytes` expose that deferred ownership. This timing/interval semantic change is important and is not hidden by the report.

The ledger attributes the **3.149 ms/frame** (`create_ms` 0.852 + `destroy_ms` 2.297) to the two large recurring shapes. At steady state, if those shapes are reused every frame and the pool remains in budget, their measured create/destroy work should be removed; the two LUT shapes are excluded by the 16 MiB threshold, with the ledger bounding their contribution at **≤0.099 ms/frame**. A numerical post-change `create_ms + destroy_ms` is **not measured**; the expected result is therefore approximately the residual LUT/other work (at most that ledger bound for the LUT component), plus cold misses, rejected releases, and measurement noise—not a promised exact value. Expected steady-state counters for the two recurring large shapes are about **−2 `images_created` and −2 `images_destroyed` per frame** relative to baseline after warm-up, alongside roughly two reuse hits per frame, assuming both shapes hit each frame. **No FPS improvement is claimed**; the baseline is CPU-bound and the previous recycler measurements did not establish an FPS benefit.

## Files and functions changed

- `src/src/video_core/renderer_vulkan/vk_image_reuse_pool.h/.cpp`: `ImageReusePool`, `Enabled`, `Get`, `Poolable`, `SameCreateShape`, `EligibleSize`, `FitsBounds`, `Acquire`, `Release`, `Shutdown`, bounds parsing/logging.
- `src/src/video_core/texture_cache/image.cpp`: `UniqueImage::Create`, `Destroy`, destructor; adoption/release hook and allocation-size capture.
- `src/src/video_core/texture_cache/image.h`: `UniqueImage` allocation-size and queue-family storage, reuse-safety marker, move transfer/rebinding.
- `src/src/video_core/texture_cache/texture_cache.cpp`: `TextureCache::DeleteImage` deferred callback marks backing images safe only at the GPU-completion point.
- `src/src/video_core/renderer_vulkan/vk_instance.cpp`: `Instance::~Instance` shutdown-before-allocator ordering.
- `src/src/video_core/texture_cache/image_stats.h`: reuse counters and RENDERSTAT fields.
- `src/CMakeLists.txt`: `vk_image_reuse_pool.cpp` source registration.
- `src/tests/video_core/test_image_reuse_pool.cpp` and `src/tests/CMakeLists.txt`: device-free tests for exact match, every create field, queue-index length/content, order-insensitive view formats, pNext/alias rejection, size threshold, and bounds. **Not run.**

## Unverified assumptions and correctness risks

1. The release-safety proof assumes every cache image that may still be referenced is ultimately released by `TextureCache::DeleteImage`'s deferred callback. Other direct `UniqueImage` destruction sites are deliberately not pooled; future code must not mark an image idle early.
2. The pool assumes VMA allocations remain valid and can be destroyed with the `VmaAllocator` stored in each entry. Acquire enforces allocator equality; no multi-device runtime exercise was performed.
3. `Instance::~Instance` is assumed to occur after the texture cache/scheduler owners have stopped producing releases, and before the allocator is destroyed. The explicit ordering before `vmaDestroyAllocator` is source verified; whole-program object teardown was not executed.
4. The static pool is only emptied by `Shutdown`. If an abnormal shutdown bypasses `Instance::~Instance`, entries may leak. There is intentionally no static destructor fallback due to allocator-order uncertainty.
5. The exact 16/6/256 defaults are bounded but retain up to 256 MiB in VRAM; even this smaller pool can increase memory pressure. `VmaAllocationInfo::size` and its relation to actual retained VRAM are not runtime-verified here.
6. The owner resets content-associated state by destroying/replacing its `UniqueImage` within a fresh backing image whose state starts undefined. Vulkan's `eUndefined` transition must discard prior contents as expected for all candidate shapes; no image-content A/B was run. Stale content remains a critical risk if future code retains owner state or bypasses the fresh backing initialization.
7. Premature reuse could corrupt commands still in flight. The `reuse_safe` marker narrows the path, but a wrong mark or future destruction path bypassing `DeferOperation` can still cause GPU use-after-reuse.
8. Move cases were reasoned from source but not dynamically exercised. Moved-from scalar handles and reuse marker are exchanged to empty; queue-index pointer rebinding after moves is intended to avoid dangling storage. Move-assigning over a live destination was already not handled by the previous implementation and remains an assumption about existing call sites.
9. Actual driver destruction can be delayed from the release window to a later reuse or shutdown window; `destroy_ms` and `images_destroyed` report actual calls, not logical-owner releases. No event-order/thread stress was run.
10. The historical later local recycler port crashed at startup even with recycling disabled; cause remains unproven. This code avoids namespace-scope Vulkan objects and keeps `Get()` function-local, but the prior startup crash does not establish that this change is safe.
11. The previous larger recycler review observed stale-content/descriptor concerns and lacked tests; this implementation does not pool image views or descriptors, but any latent stale descriptor use would be more visibly hazardous when the backing is kept alive rather than freed.
12. No performance, visual correctness, clean shutdown, startup, headset, or race result is available. The known earlier recycler result showed no reliable FPS benefit; this is an opt-in candidate only.

## Suspected compilation issues / uncertainty inventory

Nothing was compiled, configured, or tested. Items to verify in the permitted build/test stage:

- `vk_image_reuse_pool.h` includes `common/types.h`, `video_core/renderer_vulkan/vk_common.h`, and `<vk_mem_alloc.h>`. I believe `vk_common.h` declares the Vulkan-Hpp enums/types needed by `vk::ImageCreateInfo`, while `common/types.h` provides `u32/u64`. The exact include sufficiency and VMA include ordering were not compiled.
- `vk_image_reuse_pool.h` uses `std::ranges::sort`, `vk::ImageCreateFlagBits::eAlias`, and `vk::SharingMode::{eExclusive,eConcurrent}`. I believe `<algorithm>` and the project's C++23/Vulkan-Hpp setup provide these, but the enum operator/namespace availability is unverified.
- `vk_image_reuse_pool.cpp` uses `LOG_INFO(Render_Vulkan, ...)`, `<cstdlib>` conversion routines, `std::numeric_limits`, `static_cast<VkImage>(vk::Image)`, `vmaDestroyImage`, and `ImageStats::{NoteReuseHit,NoteReuseMiss,SetReusePool,NoteReuseDestroyed,NoteImageDestroyed}`. Their declarations appear in `common/logging/log.h`, standard headers, Vulkan-Hpp/VMA, and `texture_cache/image_stats.h`; format-argument compatibility and handle conversion were not compiled.
- `image.cpp` now assigns to `vk::ImageCreateInfo::pQueueFamilyIndices`, uses `ImageReusePool::{Entry,Get,Acquire,Release,Enabled}`, and stores `VmaAllocationInfo::size` in `UniqueImage::allocation_bytes`. I believe Vulkan-Hpp's structure exposes the expected pointer and `size` is `u64`-compatible, but this was not compiled.
- `image.h` adds `<vector>`, a bool marker and `std::exchange` operations. The move-initializer order is intended to match member declaration order. No warning/error check was run.
- `texture_cache.cpp` calls `UniqueImage::MarkGpuIdleForReuse`; declaration is in `image.h`. Whether this callback's exact lifetime assumptions hold is source-reviewed only.
- `vk_instance.cpp` invokes `ImageReusePool::Get().Shutdown()`; declaration is in the new header. No link verification was done.
- `src/CMakeLists.txt` registers `src/video_core/renderer_vulkan/vk_image_reuse_pool.cpp`; `src/tests/CMakeLists.txt` registers `shadps4_image_reuse_pool_test`, its test source, and `GTest::gtest_main`, `Vulkan::Headers`, `fmt::fmt`, and `spdlog::spdlog`. I believe these target names follow adjacent tests, but that target's include/transitive dependency sufficiency is unverified.
- The test uses Vulkan-Hpp designated initialization and `std::array` CTAD, GoogleTest, and global `u32/u64` aliases. I believe these are compatible with the surrounding C++23 test setup, but there may be a designated-initializer field-order or link dependency issue. The test source was not compiled or run.
- The RENDERSTAT format string and argument sequence were manually checked after changing the old recycler fields to four reuse fields; format/type correctness remains unverified.

## Outstanding work

- Build and run the device-free unit test when explicitly allowed.
- Compile the emulator and inspect the console/errors when allowed.
- Run startup/off-switch checks specifically against the historical startup-crash concern.
- Perform same-binary, same-scene, interleaved OFF/ON A/B; verify image content and shutdown, and measure create/destroy timing, hits, retained bytes, VMA VRAM, FPS and visual identity. None of these is part of this implementation commit or has been performed.
- Confirm RENDERSTAT interpretation across seconds, including pool drain at shutdown, and assess actual peak memory under gameplay.
