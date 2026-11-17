# Source digest — renderer optimisations worth porting to the Déraciné fork

Purpose: hand an implementer the *exact* upstream text for four candidate port sources, plus the
mapping onto our tree, so nothing has to be re-fetched and no commit has to be reverse-engineered
from memory.

Scope of this document. Sources were fetched as **raw files at the named commit** (not the GitHub
HTML diff view, whose text the fetch tool collapses to one line), so every fenced block below is
verbatim file content. Where a quote is not verbatim it is labelled **(paraphrased)**. Line numbers
in fence captions are the line numbers of the fetched file at that commit; the local-tree line
numbers are from this worktree at the time of writing.

Reading conventions, so nothing below is mistaken for source text:

- A line of `...` inside a fence is **elided source**, not code.
- Signatures longer than the page were wrapped with the file's own 4-space continuation indent;
  no identifiers, arguments or comments were changed.
- A caption of the form `path:first-last (sha)` gives the line range in the file **at that commit**.
  A range that starts mid-hunk is noted as "excerpt".

Local tree under study: `M:\Development\Deracine-work\wt-crossfork\src\src\video_core\` —
`texture_cache\{texture_cache.cpp,texture_cache.h,image.cpp,image.h,preserve_tracker.h,alias_trace.h}`
and `renderer_vulkan\{vk_scheduler.cpp,vk_scheduler.h,vk_gpu_profile.cpp,vk_gpu_profile.h,vk_resource_pool.cpp,vk_resource_pool.h}`.
Read-only; nothing in `src/` was modified while writing this.

### Local tree snapshot — read this before trusting any local line number

Those files were **being edited by another agent while this digest was written** (worktree
`wt-crossfork`, files `texture_cache.cpp`, `texture_cache.h`, `image.cpp` plus the new
`image_coverage.h`, `image_stats.h`; the last two appeared at 19:32, `texture_cache.cpp` changed at
19:33). Two consequences:

1. Every local line number below is a **snapshot at ~19:30–19:34, 2026-11-17**. Re-grep the symbol
   before you edit; do not paste a line number into a tool call.
2. The tree is already **ahead of A1 on one axis and behind on another**:
   - Ahead: `RefreshImage` now takes a coverage proof rather than a boolean —
     `void TextureCache::RefreshImage(Image& image, const CopyCoverage* covered)` at
     `texture_cache.cpp:1402`, declared at `texture_cache.h:151`, with the per-level skip at
     `1444-1446` (`if (covered != nullptr && covered->CoversLevel(m, num_layers)) {`) and the proof
     built by `CoveringCopyOf(src.info, dst.info)` in
     [`image_coverage.h:59`](../../src/src/video_core/texture_cache/image_coverage.h#L59). That is
     *stricter* than A1's `Covers()`, which compares only width/height/depth for the whole image:
     the local version is per level and per layer and refuses depth/stencil/aspect/block mismatches
     ([`image_coverage.h:83-95`](../../src/src/video_core/texture_cache/image_coverage.h#L83-L95)).
     **Do not replace it with A1's predicate** — that would be a rigor regression.
   - Behind: there is still **no alias state machine** — no `AliasState`, no `SynchronizeAlias`, no
     `ResolveAliasCopy`, no `alias_generation` (grep at 19:36 returned none of these names). A1's
     unique remaining value is exactly that authority/generation model, not its coverage test.


---

## A. cuesta4/shadPS4 — `52a244684ffc77c8a57cef2c2d1f03babbb04309`

URL: https://github.com/cuesta4/shadPS4/commit/52a244684ffc77c8a57cef2c2d1f03babbb04309
SHA: `52a244684ffc77c8a57cef2c2d1f03babbb04309` (Author: cuesta4, 2026-09-27)
Touched: 31 files, +1139 / −195. Relevant here: `texture_cache/image.cpp`, `image.h`,
`texture_cache.cpp`, `texture_cache.h`; adjacent: `tile_manager.cpp/.h`, `host_shaders/tiling.comp`.

The commit's own scope statement (verbatim from the patch header, reformatted as a list):

> - Texture cache: an image an alias copy is about to overwrite whole skips its upload and detile
>   from guest memory. Images created at an address another image uses are placed in shared memory
>   blocks instead of dedicated allocations, which the driver serializes with presentation. Freed
>   images are recycled by shape and destroyed off the command processor. These remove the drops of
>   God of War III hit effects.
> - Command processor: completion signals queue behind the guest copies that read memory before
>   them and land in command stream order. A `WAIT_REG_MEM` a queued signal satisfies runs on
>   without waiting for it.
> - Presentation: presents on its own queue of the graphics family when there is one
>   (`SHADPS4_PRESENT_QUEUE=0` shares the graphics queue), and frames recycle asynchronously. The
>   new `gpu_frames_ahead` setting bounds how far the command processor may run ahead of the GPU.
> - Texel buffers over tiled images sync only the bytes they read, tiling straight from the image
>   through a uint view.
> - Copy shader HLE uploads share one barrier per batch.
> - The native Vulkan pipeline cache lives in the game's shader cache, so deleting the shader cache
>   clears it.

### A1. Overwrite-aware image refreshing

#### The alias state and the two predicates

The authority record lives in `texture_cache.h`. "Generation" is a monotonically increasing
`alias_generation` counter per image; "authority" is who owns the canonical bytes (`backing`) and
who holds an uncommitted write (`writer`), each tagged with the image's `image_uid` so that a
recreated image (new uid) makes a stale identity detectable.

```cpp
// src/video_core/texture_cache/texture_cache.h:483-484, 490-508 (52a2446)
    /// Outlives the images, which return their Vulkan images to it.
    ImageRecycler image_recycler;
    Common::SlotVector<Image> slot_images;
    ...
    struct AliasState {
        // Backing contains the complete shared-memory view; writer is an uncommitted write.
        u64 backing_uid{};
        u64 writer_uid{};
        u64 download_uid{};
        ImageId backing{};
        ImageId writer{};
        ImageId download{};
        u32 members{};

        void ResetAuthority() {
            const u32 member_count = members;
            *this = {};
            members = member_count;
        }
    };
    tsl::robin_map<VAddr, AliasState> alias_states;
    boost::container::small_vector<VAddr, 4> pending_alias_downloads;
    u64 alias_generation{};
```

The coverage test — this is the whole "is the refresh redundant" predicate, and it is a **full-size
comparison, not a byte-range test**:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:290-302 (52a2446)
[[nodiscard]] static bool Covers(const Extent3D& extent, const ImageInfo& info) {
    return extent.width == info.size.width && extent.height == info.size.height &&
           extent.depth == info.size.depth;
}

[[nodiscard]] static bool CanAlias(const Image& lhs, const Image& rhs) {
    return GetAliasCopyExtent(lhs.info, rhs.info) || GetAliasCopyExtent(rhs.info, lhs.info);
}

static void SetAliasIdentity(ImageId& dst_id, u64& dst_uid, ImageId src_id, u64 src_uid) {
    dst_id = src_id;
    dst_uid = src_uid;
}
```

`GetAliasCopyExtent(from, to)` (declared in `image_info.h`, unchanged by this commit) returns the
overlap extent of two guest allocations, or `nullopt` when they do not overlap at all.

#### `SynchronizeAlias()`

Note the ownership split: an image that *owns authority* must upload from guest memory first,
because the pending-write commit can copy into it. An image that does **not** own authority may
skip the upload when the alias copy that follows will replace it completely.

```cpp
// src/video_core/texture_cache/texture_cache.cpp:409-460 (52a2446)
void TextureCache::PrepareImageAccess(ImageId image_id, AliasAccess access) {
    std::scoped_lock lock{mutex};
    UpdateImageImpl(image_id);
    if (access == AliasAccess::ReadWrite) {
        PublishAliasWrite(image_id);
    }
}

void TextureCache::UpdateImageImpl(ImageId image_id) {
    Image& image = slot_images[image_id];
    TrackImage(image_id);
    TouchImage(image);
    SynchronizeAlias(image_id);
}

void TextureCache::SynchronizeAlias(ImageId image_id) {
    Image& dst = slot_images[image_id];
    const auto state_it = True(dst.flags & ImageFlagBits::Aliased)
                              ? alias_states.find(dst.info.guest_address)
                              : alias_states.end();
    if (state_it == alias_states.end()) {
        RefreshImage(dst);
        return;
    }
    AliasState& state = state_it.value();
    // Committing the pending write can copy into these, so guest memory has to go first.
    const bool owns_authority =
        (state.backing == image_id && state.backing_uid == dst.image_uid) ||
        (state.writer == image_id && state.writer_uid == dst.image_uid);
    if (owns_authority) {
        RefreshImage(dst);
    }
    const std::optional copy_extent = ResolveAliasCopy(image_id, state);
    if (!owns_authority) {
        // Uploading guest memory is wasted when the alias copy replaces all of the image.
        RefreshImage(dst, copy_extent && Covers(*copy_extent, dst.info));
    }
    if (!copy_extent) {
        return;
    }

    Image& current = slot_images[state.backing];
    CopyAlias(state.backing, image_id, *copy_extent);
    dst.alias_generation = current.alias_generation;
    dst.flags |= ImageFlagBits::GpuModified;
    dst.flags &= ~ImageFlagBits::Dirty;
    const std::optional reverse_extent = GetAliasCopyExtent(dst.info, current.info);
    if (reverse_extent && Covers(*reverse_extent, current.info) &&
        !Covers(*copy_extent, dst.info)) {
        SetAliasIdentity(state.backing, state.backing_uid, image_id, dst.image_uid);
    }
}
```

Line 421 also shows the structural change that makes this cheap: `SynchronizeAlias()` moved *into*
`UpdateImageImpl()`, so an ordinary `UpdateImage()` now performs alias synchronisation, and
`PrepareImageAccess()` no longer calls it separately (the diff removes that call).

#### `ResolveAliasCopy()` — authority resolution without side effects

The split of the pre-commit body into `ResolveAliasCopy` (decide) + inline copy (act) is what lets
`SynchronizeAlias` ask "will there be a copy, and how big" *before* deciding whether to upload.

```cpp
// src/video_core/texture_cache/texture_cache.cpp:462-511 (52a2446)
std::optional<Extent3D> TextureCache::ResolveAliasCopy(ImageId image_id, AliasState& state) {
    const Image& dst = slot_images[image_id];
    if (!IsLiveImage(state.backing, state.backing_uid)) {
        if (!IsLiveImage(state.writer, state.writer_uid)) {
            state.ResetAuthority();
            return std::nullopt;
        }
        SetAliasIdentity(state.backing, state.backing_uid, state.writer, state.writer_uid);
        SetAliasIdentity(state.writer, state.writer_uid, {}, 0);
    }

    if (state.writer == image_id && state.writer_uid == dst.image_uid) {
        return std::nullopt;
    }
    if (!CommitAliasWriter(state)) {
        state.ResetAuthority();
        return std::nullopt;
    }

    const Image& current = slot_images[state.backing];
    if (state.backing == image_id || current.alias_generation <= dst.alias_generation) {
        return std::nullopt;
    }
    return GetAliasCopyExtent(current.info, dst.info);
}

bool TextureCache::CommitAliasWriter(AliasState& state) {
    if (!state.writer) {
        return true;
    }
    if (!IsLiveImage(state.writer, state.writer_uid) ||
        !IsLiveImage(state.backing, state.backing_uid)) {
        return false;
    }

    Image& writer = slot_images[state.writer];
    Image& backing = slot_images[state.backing];
    if (state.writer != state.backing && writer.alias_generation > backing.alias_generation) {
        const std::optional extent = GetAliasCopyExtent(writer.info, backing.info);
        if (!extent) {
            return false;
        }
        CopyAlias(state.writer, state.backing, *extent);
        backing.alias_generation = writer.alias_generation;
        backing.flags |= ImageFlagBits::GpuModified;
        backing.flags &= ~ImageFlagBits::Dirty;
    }
    SetAliasIdentity(state.writer, state.writer_uid, {}, 0);
    return true;
}
```

The "is this copy still worth doing" test is exactly:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:481-485 (52a2446) — inside ResolveAliasCopy
    const Image& current = slot_images[state.backing];
    if (state.backing == image_id || current.alias_generation <= dst.alias_generation) {
        return std::nullopt;
    }
    return GetAliasCopyExtent(current.info, dst.info);
```

#### `RefreshImage(..., overwritten)` — where the upload is dropped

The `overwritten` flag does **not** skip the bookkeeping. It is checked *after* the mip-copy list
has been built (which is where the GPU-written/CPU-written hashing happens) and after the
"nothing to copy" early-out, and it returns without clearing `Dirty` — the caller clears it once
`CopyAlias` has run. That ordering is the whole trick: guest memory is not read and not detiled,
but the image still records what it has seen.

```cpp
// src/video_core/texture_cache/texture_cache.cpp:2497-2500, 2528-2581 (52a2446)
void TextureCache::RefreshImage(Image& image, bool overwritten) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }
    ...
    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        ...
        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            Common::PerformanceTelemetry::ScopedFastDuration hash_time{
                telemetry_enabled, Common::PerformanceTelemetry::Counter::TextureHashNs};
            if (telemetry_enabled) {
                Common::PerformanceTelemetry::AddEnabled(
                    Common::PerformanceTelemetry::Counter::TextureHashBytes, mip_size);
            }
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }
        ...image_copies.push_back({... .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil, ...});
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }
    if (overwritten) {
        // The alias copy that follows replaces all of the image.
        return;
    }
    scheduler.EndRendering(...);
    ...
    image.Upload(image_copies, buffer, offset);
}
```

Declaration and its documentation:

```cpp
// src/video_core/texture_cache/texture_cache.h:230-233 (52a2446)
    /// Reuploads image contents. An image that is about to be overwritten keeps only the
    /// bookkeeping of a refresh.
    void RefreshImage(Image& image, bool overwritten = false);
```

#### How GPU-written vs CPU-written contents are distinguished

Three separate signals, and all three are needed:

1. `ImageFlagBits::Dirty` / `CpuDirty` / `MaybeCpuDirty` — the guest (CPU) wrote memory under the
   image. `InvalidateMemory()` sets `CpuDirty` and resets alias authority
   (`texture_cache.cpp:1513-1537`).
2. `ImageFlagBits::GpuModified` / `GpuDirty` — the GPU wrote the image. `GpuModified && !GpuDirty`
   means guest memory is stale relative to the image, so an upload is only allowed for mips whose
   guest bytes changed (the XXH3 per-mip hashes above).
3. The alias generation + authority pair above — the *cross-image* version of the same question,
   which flags cannot answer because two images share one address.

A GPU write is published with:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:582-621 (52a2446)
void TextureCache::PublishAliasWrite(ImageId image_id) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    readback_tracker->Invalidate(image.info.guest_address, image.info.guest_size);
    image.alias_generation = ++alias_generation;
    if (False(image.flags & ImageFlagBits::Aliased)) {
        return;
    }

    AliasState& state = alias_states[image.info.guest_address];
    if (IsLiveImage(state.backing, state.backing_uid)) {
        const Image& backing = slot_images[state.backing];
        if (!CanAlias(image, backing)) {
            state.ResetAuthority();
        }
    }
    state.members = std::max(state.members, 2u);
    const bool continuing_write = state.writer == image_id && state.writer_uid == image.image_uid;
    if (!state.backing) {
        SetAliasIdentity(state.backing, state.backing_uid, image_id, image.image_uid);
    } else if (!continuing_write && IsLiveImage(state.backing, state.backing_uid)) {
        const Image& backing = slot_images[state.backing];
        const std::optional extent = GetAliasCopyExtent(image.info, backing.info);
        if (extent && Covers(*extent, backing.info)) {
            SetAliasIdentity(state.backing, state.backing_uid, image_id, image.image_uid);
        } else {
            SetAliasIdentity(state.writer, state.writer_uid, image_id, image.image_uid);
        }
    } else if (!continuing_write) {
        SetAliasIdentity(state.backing, state.backing_uid, image_id, image.image_uid);
    }
    ...
}
```

Note the rule in the middle: a write that covers the backing completely **becomes** the backing
(no copy ever needed); a partial write becomes the `writer` and must be flushed into the backing
before the next read.

#### HTILE / depth / stencil under A1

There is no HTILE handling anywhere in this path. `CopyAlias` is colour-only and hard-codes the
colour aspect:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:533-553 (52a2446)
    const vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = std::min(src.info.resources.layers, dst.info.resources.layers),
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = std::min(src.info.resources.layers, dst.info.resources.layers),
            },
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, extent.depth},
    };
```

Stencil is excluded from uploads (`aspect_mask & ~vk::ImageAspectFlagBits::eStencil`, both in the
upstream and the commit version), and the metadata surfaces (CMask/FMask/**HTile**) are only
bookkept as `surface_metas` and erased when an image is deleted:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:3021-3031 (52a2446)
    // Remove any registered meta areas.
    const auto& meta_info = image.info.meta_info;
    if (meta_info.cmask_addr) {
        surface_metas.erase(meta_info.cmask_addr);
    }
    if (meta_info.fmask_addr) {
        surface_metas.erase(meta_info.fmask_addr);
    }
    if (meta_info.htile_addr) {
        surface_metas.erase(meta_info.htile_addr);
    }
```

So: A1's optimisation silently **assumes the overwrite covers the whole image for every aspect and
every metadata surface that matters**. For colour aliases that is fine. For a depth alias whose
HTILE state another image believes in, it is not proven by this commit. Treat depth aliases as out
of scope until measured.

### A2. `ImageRecycler`

Full API as declared:

```cpp
// src/video_core/texture_cache/image.h:48-84 (52a2446)
/// Keeps images the GPU is done with for reuse by an image of the same shape, and destroys the
/// rest on a background thread. Most images get dedicated memory, which the driver allocates and
/// frees with system calls.
class ImageRecycler {
public:
    explicit ImageRecycler(VmaAllocator allocator);
    ~ImageRecycler();

    ImageRecycler(const ImageRecycler&) = delete;
    ImageRecycler& operator=(const ImageRecycler&) = delete;

    /// Takes a free image created from image_ci, if there is one.
    bool TryTake(const vk::ImageCreateInfo& image_ci, vk::Image& image, VmaAllocation& allocation);

    /// Takes over an image the GPU no longer uses.
    void Release(const vk::ImageCreateInfo& image_ci, vk::Image image, VmaAllocation allocation);

private:
    struct FreeImage {
        vk::ImageCreateInfo image_ci;
        vk::Image image;
        VmaAllocation allocation;
        u64 size;
        u64 release_ns;
    };

    void EvictLocked(u64 now_ns);
    void DestroyLoop(std::stop_token stoken);

    VmaAllocator allocator;
    std::mutex mutex;
    std::condition_variable_any destroy_cv;
    std::vector<FreeImage> free_images; ///< Oldest first.
    u64 free_bytes{};
    std::vector<std::pair<vk::Image, VmaAllocation>> doomed;
    std::jthread destroy_thread;
};
```

Keying is by **creation parameters**, not by guest address or shape heuristic:

```cpp
// src/video_core/texture_cache/image.cpp:92-108 (52a2446)
constexpr u64 MaxFreeImageBytes = 128_MB;
constexpr size_t MaxFreeImages = 32;
constexpr u64 FreeImageLifetimeNs = 2'000'000'000;

u64 NowNs() { ... }

bool SameShape(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
    return a.pNext == nullptr && b.pNext == nullptr && a.flags == b.flags &&
           a.imageType == b.imageType && a.format == b.format && a.extent == b.extent &&
           a.mipLevels == b.mipLevels && a.arrayLayers == b.arrayLayers &&
           a.samples == b.samples && a.tiling == b.tiling && a.usage == b.usage &&
           a.sharingMode == b.sharingMode;
}
```

Budget, eviction, release and the destroy thread:

```cpp
// src/video_core/texture_cache/image.cpp:112-204 (52a2446)
ImageRecycler::ImageRecycler(VmaAllocator allocator_) : allocator{allocator_} {
    destroy_thread = std::jthread{[this](std::stop_token stoken) { DestroyLoop(stoken); }};
}

ImageRecycler::~ImageRecycler() {
    destroy_thread.request_stop();
    destroy_thread.join();
    for (const auto& free_image : free_images) {
        vmaDestroyImage(allocator, free_image.image, free_image.allocation);
    }
    for (const auto& [image, allocation] : doomed) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

bool ImageRecycler::TryTake(const vk::ImageCreateInfo& image_ci, vk::Image& image,
                            VmaAllocation& allocation) {
    std::scoped_lock lock{mutex};
    EvictLocked(NowNs());
    // The latest release of a shape is the likeliest to be taken again.
    for (auto it = free_images.rbegin(); it != free_images.rend(); ++it) {
        if (SameShape(it->image_ci, image_ci)) {
            image = it->image;
            allocation = it->allocation;
            free_bytes -= it->size;
            free_images.erase(std::next(it).base());
            return true;
        }
    }
    return false;
}

void ImageRecycler::Release(const vk::ImageCreateInfo& image_ci, vk::Image image,
                            VmaAllocation allocation) {
    VmaAllocationInfo allocation_info{};
    vmaGetAllocationInfo(allocator, allocation, &allocation_info);
    const u64 now_ns = NowNs();
    bool destroy{};
    {
        std::scoped_lock lock{mutex};
        if (image_ci.pNext == nullptr && allocation_info.size <= MaxFreeImageBytes / 2) {
            free_images.push_back({
                .image_ci = image_ci,
                .image = image,
                .allocation = allocation,
                .size = allocation_info.size,
                .release_ns = now_ns,
            });
            free_bytes += allocation_info.size;
        } else {
            doomed.emplace_back(image, allocation);
        }
        EvictLocked(now_ns);
        destroy = !doomed.empty();
    }
    if (destroy) {
        destroy_cv.notify_one();
    }
}

void ImageRecycler::EvictLocked(u64 now_ns) {
    size_t evicted = 0;
    while (evicted < free_images.size()) {
        const FreeImage& oldest = free_images[evicted];
        const size_t remaining = free_images.size() - evicted;
        if (free_bytes <= MaxFreeImageBytes && remaining <= MaxFreeImages &&
            now_ns - oldest.release_ns <= FreeImageLifetimeNs) {
            break;
        }
        free_bytes -= oldest.size;
        doomed.emplace_back(oldest.image, oldest.allocation);
        ++evicted;
    }
    free_images.erase(free_images.begin(), free_images.begin() + evicted);
}

void ImageRecycler::DestroyLoop(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:ImageRecycler");
    std::vector<std::pair<vk::Image, VmaAllocation>> batch;
    for (;;) {
        {
            std::unique_lock lock{mutex};
            if (!destroy_cv.wait(lock, stoken, [this] { return !doomed.empty(); })) {
                return;
            }
            batch.swap(doomed);
        }
        for (const auto& [image, allocation] : batch) {
            vmaDestroyImage(allocator, image, allocation);
        }
        batch.clear();
    }
}
```

`UniqueImage` gains the recycler pointer and consults it on create/destroy:

```cpp
// src/video_core/texture_cache/image.cpp:206-251 (52a2446)
void UniqueImage::Destroy() {
    if (image) {
        if (recycler) {
            recycler->Release(image_ci, image, allocation);
        } else {
            vmaDestroyImage(allocator, image, allocation);
        }
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    if (recycler) {
        vk::Image free_image{};
        if (recycler->TryTake(image_ci, free_image, allocation)) {
            image = free_image;
            return;
        }
    }
    if (suballocate && CreateSuballocated()) {
        return;
    }
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };
    ...
}
```

#### How "all consuming submissions completed" is guaranteed

Not by the recycler — by the **scheduler's deferred-operation queue**. An image is never destroyed
inline; deletion queues an erase keyed on the scheduler tick at which it was queued, and that
callback only runs when the master semaphore reports that tick retired:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:3040-3049 (52a2446)
    // Reclaim image and any image views it references.
    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
```

```cpp
// src/video_core/renderer_vulkan/vk_scheduler.h:626-632 and vk_scheduler.cpp:585-629 (52a2446)
    void DeferOperation(Common::UniqueFunction<void>&& func,
                        ...) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick(), trace);
        ...
    }
...
void Scheduler::PopPendingOperations(bool force) {
    ...
    if (!master_semaphore.IsFree(front_tick)) {
        ...
        master_semaphore.Refresh();
        if (!master_semaphore.IsFree(front_tick)) {
            return;
        }
    }
    std::unique_lock lk(pending_ops_mutex);
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
    }
    ...
}
```

So the chain is: LRU/GC decides a tick-`T` image is dead → `DeleteImage` queues an erase at
`CurrentTick()` → the erase (and therefore `Image`'s destructor, `UniqueImage::Destroy`, and
`ImageRecycler::Release`) runs only after `master_semaphore.IsFree(T)` → the VMA destroy itself
happens on the recycler's own `jthread`. Three separate completion points, all tick-gated or
off-thread; nothing waits on the device.

#### Layout / access reinitialisation

Implicit, and worth understanding before porting. Vulkan layout and access state live on the
**image object**, not on the `VkImage` handle, and they default to `eUndefined`:

```cpp
// src/video_core/texture_cache/image.h:269-284 (52a2446)
    // Resource state tracking
    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    struct State {
        vk::PipelineStageFlags2 pl_stage = vk::PipelineStageFlagBits2::eAllCommands;
        vk::AccessFlags2 access_mask = vk::AccessFlagBits2::eNone;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BackingImage {
        UniqueImage image;
        State state;
        std::vector<State> subresource_states;
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 num_samples;
    };
```

A recycled `VkImage` arrives into a **fresh** `BackingImage`, so the first use transitions it from
`eUndefined` — legally correct (contents discarded, which is what reuse implies) and requiring no
extra barrier. View invalidation is the deferred `slot_image_views.erase(...)` in the snippet above
plus fresh `image_view_infos`/`image_view_ids` on the new object.

#### Thread safety

Coarse and simple: one `std::mutex` around `free_images`/`doomed`/`free_bytes`, held across
`EvictLocked`; the destroy thread only ever touches `doomed` under the same mutex, then destroys
outside the lock. `Release` is called from whatever thread drops the last reference, so the mutex
(not thread affinity) is what makes it correct. `ImageRecycler` is constructed in `TextureCache`
and declared **before** `slot_images` so it outlives the images that return their handles
(`texture_cache.h:483-485`).

Memory budget: `128 MiB` of free images, at most `32` of them, each kept at most `2 s`, and an
individual image larger than `MaxFreeImageBytes / 2` (64 MiB) is never cached — it goes straight
to the destroy queue. The 2880×3240-per-eye case matters here: an RGBA8 eye buffer is ~37 MiB, so
two of them fill most of the cache.

### A3. Shared-block (sub)allocation of images that alias guest memory

The exact trigger — **an existing live image at the same guest address** — computed at creation
time in the slow find path:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:2089-2101 (52a2446)
    // Create and register a new image
    if (!image_id) {
        find_path = Common::PerformanceTelemetry::ImageFindPath::Created;
        // An image sharing its address with another tends to come and go with the uses of that
        // memory, so it skips the driver allocation of dedicated memory.
        const bool is_alias = std::ranges::any_of(image_ids, [&](ImageId cache_id) {
            return slot_images.is_allocated(cache_id) &&
                   slot_images[cache_id].info.guest_address == info.guest_address;
        });
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info,
                                      &image_recycler, is_alias);
        RegisterImage(image_id);
    }
```

Everywhere else the same `&image_recycler` is passed **without** `suballocate`, i.e. dedicated
memory (`ResolveDepthOverlap`, `ExpandImage`, `PrepareDepthTarget`, `ResolveOverlap`). So: images
that alias guest memory are suballocated; new, uniquely-addressed images and depth/stencil
companions stay dedicated.

The VMA call used, and the fallback that keeps it safe:

```cpp
// src/video_core/texture_cache/image.cpp:253-290 (52a2446)
bool UniqueImage::CreateSuballocated() {
    // Dedicated memory costs a driver allocation, which can stall behind presentation.
    const auto [result, new_image] = device.createImage(image_ci);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    const VkMemoryRequirements requirements = device.getImageMemoryRequirements(new_image);
    const VkPhysicalDeviceMemoryProperties* properties{};
    vmaGetMemoryProperties(allocator, &properties);
    u32 device_types = 0;
    for (u32 i = 0; i < properties->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
            (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
            device_types |= 1u << i;
        }
    }
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_UNKNOWN,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        .memoryTypeBits = device_types,
    };
    VmaAllocation new_allocation{};
    if (vmaAllocateMemory(allocator, &requirements, &alloc_info, &new_allocation, nullptr) !=
        VK_SUCCESS) {
        device.destroyImage(new_image);
        return false;
    }
    if (vmaBindImageMemory(allocator, new_allocation, new_image) != VK_SUCCESS) {
        vmaFreeMemory(allocator, new_allocation);
        device.destroyImage(new_image);
        return false;
    }
    image = new_image;
    allocation = new_allocation;
    return true;
}
```

Notable: it does **not** use `vmaCreateImage`; it calls `vkCreateImage` itself and then
`vmaAllocateMemory` **without** `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`, no `pool`, and no
`VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT` — it relies on VMA's default block suballocation. That is why
a failure path exists: if either call fails, the created image is destroyed and the caller falls back
to the dedicated `vmaCreateImage` path in `Create`.

Two VMA facts that matter for a port (from our own vendored header,
[`src/externals/vma/include/vk_mem_alloc.h:690-695`](../../src/externals/vma/include/vk_mem_alloc.h#L690-L695)):

```cpp
    /** \brief Set this flag if the allocated memory will have aliasing resources.

    Usage of this flag prevents supplying `VkMemoryDedicatedAllocateInfoKHR` when #VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT is specified.
    Otherwise created dedicated memory will not be suitable for aliasing resources, resulting in Vulkan Validation Layer errors.
    */
    VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT = 0x00000200,
```

A cleaner port passes an explicit `VmaAllocationCreateInfo` to `vmaCreateImage` instead of
hand-rolling the `vkCreateImage` + `vmaAllocateMemory` + `vmaBindImageMemory` triple, so
`WITHIN_BUDGET`, dedicated-allocation heuristics and the block bookkeeping all stay inside VMA.

### A. Lands where (our tree)

Local baseline, verified by reading:

- `texture_cache.cpp` has **no** `SynchronizeAlias`, `ResolveAliasCopy`, `CommitAliasWriter`,
  `PublishAliasWrite`, `CopyAlias` or `alias_states` — aliasing is handled by re-creation +
  `CopyImage` in `ResolveOverlap`/`ExpandImage`. So A1 is a new subsystem, not a patch.
- `image.h` has **no** `ImageRecycler`; `UniqueImage` is `vk::Device`+`VmaAllocator` only.
- `RefreshImage` is `void TextureCache::RefreshImage(Image& image)` at
  [texture_cache.cpp:1402](../../src/src/video_core/texture_cache/texture_cache.cpp#L1402) with no
  `overwritten` parameter; declared at
  [texture_cache.h:151](../../src/src/video_core/texture_cache/texture_cache.h#L151).

**A1 lands where** — *re-verify every line number; these files were moving (see the snapshot note
above)*

| Change | Local site (snapshot 2026-11-17 ~19:30) |
| --- | --- |
| Alias authority model: `AliasState`, `alias_states`, `alias_generation`, `SynchronizeAlias`, `ResolveAliasCopy`, `CommitAliasWriter`, `PublishAliasWrite`, `CopyAlias` | **absent**; new members beside `lru_cache`/`slot_images` (`texture_cache.h:348`), new functions in `texture_cache.cpp`. This is A1's real remaining contribution. |
| `RefreshImage` skip predicate | **already implemented, and stricter than A1** — `RefreshImage(Image&, const CopyCoverage* covered)` at `texture_cache.cpp:1402` (moved from 1366), `texture_cache.h:151`, per-level skip at `1444-1446`. Do not replace it with A1's `Covers()`. |
| Call sites of the refresh decision | `texture_cache.cpp:649` (`RefreshImage(source_image)`), `1006-1007` (coverage built in `ExpandImage`) |
| Where A1's whole-image skip would still add something | nothing for `ExpandImage`; only for the cross-image alias materialisation (`SynchronizeAlias`), which is the case `CopyCoverage` does not model at all |

Our tree **already answers A1's question for the expansion path**, gated behind an env flag, with a
better proof than A1's boolean:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:999-1008 (local, snapshot 19:32)
    // What the copy is going to write, level by level. Only levels it does not reach are read
    // from guest memory: the rest would be overwritten before anything could read them.
    // SHADPS4_DBG_COVERED_REFRESH=1 turns it on; the shapes it decides on are logged once each.
    static const bool covered_refresh = [] {
        const char* value = std::getenv("SHADPS4_DBG_COVERED_REFRESH");
        return value != nullptr && value[0] == '1';
    }();
    const CopyCoverage covered =
        covered_refresh ? CoveringCopyOf(src_image.info, new_image.info) : CopyCoverage{};
    ImageStats::NoteExpand(!covered.CoversNothing());
```

and the skip itself:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:1444-1446 (local, snapshot 19:33)
        // not reach are uploaded. See CopyCoverage, and ExpandImage, the only caller that can
        ...
        if (covered != nullptr && covered->CoversLevel(m, num_layers)) {
```

So A1's *upload-skip* idea is already being landed by someone else, in a more rigorous form.
A1's *authority model* is not, and it is what unblocks the cross-image cases (`CopyAlias`,
`CommitAliasWriter`, `PublishAliasWrite`) that no single-image coverage proof can express.

**A2 lands where**

- `TextureCache` constructor
  ([texture_cache.cpp:187](../../src/src/video_core/texture_cache/texture_cache.cpp#L187)) gains
  `image_recycler{instance.GetAllocator()}`; declare it **before** `slot_images` in
  `texture_cache.h:348`.
- `UniqueImage` (`image.h:44`, `Create` at `image.h:71`, `Destroy` at `image.h:79`) gains
  `recycler`/`suballocate`, and `image.cpp:96-139` (the two `vmaDestroyImage` calls and the
  `vmaCreateImage` call) gains the try-take / release paths.
- Completion gating already exists: `Scheduler::DeferOperation` is at
  `vk_scheduler.h:623` and `PopPendingOperations` at `vk_scheduler.cpp:885` — same shape as the A
  version, but **without** the `force` parameter and without `pending_ops_front_tick` early-out.
  Verify the ordering guarantee before relying on it.
- `DeleteImage` at `texture_cache.cpp:1784`; check whether the local erase is already deferred.

**A3 lands where**

- `FindImage` slow path creation, `texture_cache.cpp:1049`-region (local): add the `is_alias`
  computation over `image_ids` and pass it to `slot_images.insert(...)`.
- `UniqueImage::Create` (`image.cpp:116-139`, `vmaCreateImage` with
  `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`): add the suballocated branch.
- `ResourcePool` in `vk_resource_pool.h:23` is unrelated (command pools, descriptor heaps); do not
  conflate A2's recycler with it.

### A. Applicability to Déraciné

- Déraciné renders two eyes at 2880×3240; at 8 bytes/texel that is ~37 MiB per colour target, so
  A2's recycler and its 128 MiB budget are the cheapest wins here: every resolution change or
  target re-creation currently pays a `vkCreateImage` + VMA allocation inside the frame.
- The game aliases depth and colour over the same guest memory across frames (the fork already
  needs `alias_trace.h` and `preserve_tracker.h` to reason about it). A1's authority model is the
  principled version of what `PreserveTracker`/`Alias3DTracker` currently sample and assert about.
- A1's upload skip removes one full detile+upload per alias materialisation; at 37 MiB per image
  that is a bytes-class win, not a calls-class win, and it lands exactly on the "image refreshed
  then immediately overwritten by the alias copy" pattern that dominates aliased-target frames.
- A3 mainly buys latency: a driver allocation for a 37 MiB aliased image can serialise behind
  presentation. Fewer allocations per frame means fewer places to stall.
- Suballocation is the weakest of the three for us: it trades allocator stalls for fragmentation
  and VMA block-search cost, and Déraciné's image sizes change much less often than God of War
  III's effects.

### A. Risks / prerequisites

- A1 assumes "covers" is a full-size test. Two images of the same size but different pitch/tiling
  count as covering. If our aliases ever differ in pitch, the skip is unsound.
- A1 is **colour-only** (`CopyAlias` hard-codes `eColor`). Déraciné's aliasing includes depth;
  do not let the overwrite skip apply to a depth/stencil image, and do not assume HTILE state
  follows the colour copy.
- A2's correctness rests on `DeferOperation` being tick-gated in *our* scheduler. The local
  `DeferOperation` (`vk_scheduler.h:623-625`) records `CurrentTick()` but has no
  `pending_ops_front_tick` fast path; confirm `PopPendingOperations` still only runs a callback
  when the semaphore says the tick retired. If an image ever reaches `vmaDestroyImage` while its
  last submission is in flight, the portal is a GPU fault that reproduces once an hour.
- A2 keys on `vk::ImageCreateInfo`, and our `UniqueImage::Create` takes an extra
  `std::span<const vk::Format> view_formats` (`image.h:71`, see the comment at `image.h:67-70`
  about mutable formats). `SameShape` must compare `view_formats` too, or a recycled image will
  fail `AllowsViewFormat` and silently drop to a slower format path — or worse, be viewed in a
  format the driver did not reserve.
- A3's `CreateSuballocated` bypasses `vmaCreateImage`, so VMA's `WITHIN_BUDGET` accounting and
  dedicated-allocation preference heuristics no longer apply to it. Port it as a VMA
  allocation-create flag instead of a hand-rolled `vkCreateImage` + `vmaAllocateMemory` pair.
- Whole-commit hazards: the same diff changes `liverpool.cpp` signal ordering, the presenter queue
  and `gpu_frames_ahead`. A1's `DeferOperation`-based recycling interacts with that signal
  rewiring; a partial port must not assume the old completion semantics.

### A. Explicitly unrelated parts of `52a2446` — do not drag them in

| Area | Files | Why unrelated |
| --- | --- | --- |
| Presentation on its own queue, async frame recycling, `gpu_frames_ahead` | `vk_presenter.cpp/.h`, `vk_instance.cpp/.h`, `imgui/renderer/imgui_impl_vulkan.*`, `emulator_settings.h`, `emulator.cpp` | Queue/frame pacing for a desktop window; Déraciné presents through OpenXR (see local `vk_vr_exporter.cpp`). Our presenter already diverges. |
| Command-processor signal ordering (`GuestSignalLabel`, `SignalAfterGuestReads`, `SkipWaitForQueuedSignal`, `OrderAfterSkippedSignals`, `DeferGpuCompletionInOrder`) | `amdgpu/liverpool.cpp/.h` | Guest-visible fence semantics; the largest and riskiest part of the commit, and orthogonal to texture-cache work. The only coupling is that A1/A2's deferred erases assume tick retirement, which our scheduler already models. |
| Native pipeline-cache-in-shader-cache move | `vk_pipeline_cache.cpp/.h`, `vk_pipeline_serialization.cpp`, `cache_storage.cpp/.h` | Startup-time improvement; Déraciné's shader set is stable and small compared with God of War III. |
| Tiling straight from the image through a uint view | `tile_manager.cpp/.h`, `host_shaders/tiling.comp` | Self-contained and independently valuable (removes a linear copy + scratch buffer per detile), but not part of A1/A2/A3. Port separately if detile bandwidth shows up in a profile. |
| Texel-buffer sync of only the bytes read; copy-shader HLE barrier batching; buffer-cache changes | `tile_manager.*`, `buffer_cache.cpp/.h`, `vk_shader_hle.cpp`, `vk_rasterizer.cpp/.h` | Buffer-side optimisations with their own prerequisites. |

---

## B. AbdelmoumenDeghbouche/wipeout_pc — `ca4662a60762cec2d5d119f62a7c6075ba7a3f00`

URL: https://github.com/AbdelmoumenDeghbouche/wipeout_pc/commit/ca4662a60762cec2d5d119f62a7c6075ba7a3f00
SHA: `ca4662a60762cec2d5d119f62a7c6075ba7a3f00` (Author: cmd, 2026-11-14)
Subject: *Texture cache: reuse growing mip chains and stage only new levels (#217)*
Touched: `gpu/shadps4/video_core/texture_cache/{image.cpp,image.h,texture_cache.cpp,texture_cache.h}`,
`vk_staging_buffer_pool.{cpp,h}`, `tests/test_mip_growth_gpu.cpp` (new), `gpu/CMakeLists.txt`.

### The question this commits answers

WipEout builds a mip chain **one level at a time**: level 1, then 2, then 3… Each `ExpandImage`
used to allocate a new image, copy, free the old one. This commit reserves host capacity once and
then grows the declared chain in place.

**So: is B a mip-in-place trick or a reallocation trick?** Both, in a defined order. The first
expansion allocates a backing with *all* mip levels the geometry could ever need
(`host_mip_levels`), and later expansions mutate `image.info` in place on that same backing — no
new image, no copy. If the reserve was not possible (wrong shape, depth, arrays, multisample) it
falls back to the upstream reallocate-and-copy path. The test's own assertion names the intent:
`"mip growth reuses its backing and preserves GPU-written pixels"`.

### Backing-mip-capacity preallocation

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1320-1327 (ca4662a)
    // WipEout builds a mip chain one level at a time. Reserve host capacity on the first
    // expansion, keeping guest sizes, views and uploads limited to the declared levels.
    const u32 host_mips = info.type == AmdGpu::ImageType::Color2D &&
                                 !info.props.is_depth && !info.props.is_block &&
                                 info.resources.layers == 1 && info.num_samples == 1 &&
                                 info.guest_samples == 1 && info.size.depth == 1
                             ? std::bit_width(std::max(info.size.width, info.size.height)) : 0;
    const auto new_image_id = slot_images.insert(instance, runtime, slot_image_views, info, host_mips);
```

```cpp
// gpu/shadps4/video_core/texture_cache/image.cpp:216-220 (ca4662a)
        .mipLevels = std::max(host_mip_levels, static_cast<u32>(info.resources.levels)),
```

`std::bit_width(max(w, h))` is the full chain for a power-of-two-shaped 2D image; `0` means "no
reserve", preserving upstream behaviour. The image's `info.resources.levels` still reports only the
*declared* levels, so guest sizes, uploads and views stay limited to what the game has announced.

### `Image::CanGrowMipLevels()`

This is the compatibility gate. Everything that could make in-place growth observable must match;
only `resources.levels` and `guest_size` may increase.

```cpp
// gpu/shadps4/video_core/texture_cache/image.cpp:240-265 (ca4662a)
bool Image::CanGrowMipLevels(const ImageInfo& requested) const {
    if (!backing || backing_images.size() != 1 || scene_proxy ||
        True(flags & ImageFlagBits::Aliased) || info.type != AmdGpu::ImageType::Color2D ||
        requested.type != info.type || info.props.is_depth || info.props.is_block ||
        info.resources.layers != 1 || requested.resources.layers != 1 ||
        info.num_samples != 1 || requested.num_samples != 1 ||
        info.guest_samples != 1 || requested.guest_samples != 1 ||
        info.size != requested.size || info.size.depth != 1 || info.pitch != requested.pitch ||
        info.pixel_format != requested.pixel_format || info.num_bits != requested.num_bits ||
        info.tile_mode != requested.tile_mode || info.array_mode != requested.array_mode ||
        info.alt_tile != requested.alt_tile || info.bank_swizzle != requested.bank_swizzle ||
        info.props.is_pow2 != requested.props.is_pow2 ||
        info.guest_address != requested.guest_address || requested.guest_size < info.guest_size ||
        requested.resources.levels <= info.resources.levels ||
        requested.resources.levels > backing->image.image_ci.mipLevels) {
        return false;
    }
    for (u32 mip = 0; mip < info.resources.levels; ++mip) {
        const auto& a = info.mips_layout[mip];
        const auto& b = requested.mips_layout[mip];
        if (a.size != b.size || a.pitch != b.pitch || a.height != b.height || a.offset != b.offset) {
            return false;
        }
    }
    return true;
}
```

The last loop is the important one: the **existing** mips must keep their exact guest layout
(offset, pitch, height, size), otherwise growing in place would move data the GPU already has.

### The in-place growth path

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1293-1319 (ca4662a)
ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    auto& image = slot_images[image_id];
    if (image.CanGrowMipLevels(info)) {
        ResolveReadbacks(info.guest_address, info.guest_size, true, ReadbackSource::ImageRefresh);
        RefreshImage(image);
        const u32 first_mip = image.info.resources.levels;
        const VAddr old_end = image.guest_end;
        total_used_memory += Common::AlignUp(info.guest_size, 1024) -
                             Common::AlignUp(image.info.guest_size, 1024);
        image.info = info;
        image.guest_end = info.guest_address + info.guest_size;
        image.mip_hashes.resize(info.resources.levels);
        ++registry_generation;
        // Only register and protect the newly declared guest range, never the reserved capacity.
        const u64 first_page = ((old_end - 1) >> Traits::PageBits) + 1;
        const u64 last_page = (image.guest_end - 1) >> Traits::PageBits;
        for (u64 page = first_page; page <= last_page; ++page) {
            page_table[page].push_back(image_id);
        }
        image.flags |= ImageFlagBits::CpuDirty;
        TrackImage(image_id);
        RefreshImage(image, first_mip);
        if (image.binding.is_bound || image.binding.is_target) {
            image.binding.needs_rebind = 1;
        }
        return image_id;
    }
```

Note the three bookkeeping steps that make in-place growth safe: (1) `first_mip` captured *before*
`image.info = info`; (2) the page table only gains the pages of the newly declared range — the
reserved-but-undeclared capacity is never watched or protected; (3) `binding.needs_rebind` so any
cached descriptor set pointing at the old mip count is rebuilt.

### `RefreshImage(image, first_mip)` — signature and semantics

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.h:243-245 (ca4662a)
    /// Reuploads image contents.
    void RefreshImage(Image& image, u32 first_mip = 0);
```

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1778-1781, 1816, 1827 (ca4662a)
void TextureCache::RefreshImage(Image& image, u32 first_mip) {
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.guest_samples > 1) {
        return;
    }
    ...
    for (u32 m = first_mip; m < num_mips; m++) {
    ...
            if (first_mip == 0 && image.mip_hashes[m] == hash) {
                continue;
            }
```

Semantics: refresh **only levels `[first_mip, num_mips)`**; levels below `first_mip` are already
resident and are not touched. `first_mip == 0` means the historical "refresh everything", and the
GPU-modified mip-hash short-circuit is **disabled for incremental refreshes** (`first_mip == 0 &&`
in the condition), because a freshly declared level has no meaningful previous hash.

The upload itself is compacted so only the new levels are staged, when the guest layout allows it:

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1857-1905 (ca4662a, excerpt)
    ImageInfo upload_info = image.info;
    u32 source_offset = 0;
    bool compact = first_mip != 0 && !image.info.props.is_block &&
                   image.info.resources.layers == 1 && image.info.size.depth == 1 &&
                   image.info.mips_layout[first_mip].offset % instance.StorageMinAlignment() == 0;
    if (compact && AmdGpu::IsMacroTiled(image.info.array_mode)) {
        ...
        // A dispatch uses one tiling mode. Keep the old path for a mixed macro/micro range.
        compact = micro_mip(first_mip) == micro_mip(num_mips - 1);
        ...
    }
    if (compact) {
        source_offset = image.info.mips_layout[first_mip].offset;
        upload_info.guest_address += source_offset;
        upload_info.guest_size -= source_offset;
        upload_info.resources.levels -= first_mip;
        upload_info.size.width = std::max(image.info.size.width >> first_mip, 1u);
        upload_info.size.height = std::max(image.info.size.height >> first_mip, 1u);
        upload_info.pitch = image.info.mips_layout[first_mip].pitch;
        // Preserve the guest's mip layout; recomputing it as a new mip 0 changes its padding.
        for (u32 mip = 0; mip < upload_info.resources.levels; ++mip) {
            upload_info.mips_layout[mip] = image.info.mips_layout[first_mip + mip];
            upload_info.mips_layout[mip].offset -= source_offset;
        }
    }
```

and the per-copy offsets are rebased, not recomputed:

```cpp
// gpu/shadps4/video_core/texture_cache/texture_cache.cpp:1948-1952 (ca4662a)
    for (auto& copy : image_copies) {
        copy.bufferOffset = copy.bufferOffset - source_offset + offset;
    }
```

The staging charge is also corrected (this is what stops the early-submit heuristic from
under-counting large uploads):

```cpp
// gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp:68-71 (ca4662a)
u64 StagingBufferPool::UploadReservationBytes(u64 size, u64 alignment) {
    return size > BLOCK_SIZE ? std::max(size + size / 4, RoundAllocationSize(size))
                             : Common::AlignUp(size, alignment);
}
```

### The GPU regression test it adds

File: `tests/test_mip_growth_gpu.cpp` (283 lines, new). CMake target: `mip-growth-gpu-test`
(`EXCLUDE_FROM_ALL`, so never built by default), linked against the game runtime with
`-Wl,--wrap=runtime_memory_region`.

What it is: a real Vulkan fixture. It reserves and maps guest memory through `libkernel`'s own NIDs
(`runtime_memory_reserve`, then `allocate`/`map` resolved by `runtime_memory_resolve("...")`),
constructs `Vulkan::Instance`, `Scheduler`, `Runtime`, `AmdGpu::Liverpool` and a `Rasterizer`, and
drives `TextureCache::FindImage` / `UpdateImage` directly.

How it sets up the guest data (verbatim, including the run parameters):

```cpp
// tests/test_mip_growth_gpu.cpp:56-85 (ca4662a)
    TextureCache::ImageDesc desc{};
    std::memset(guest, 0xcd, GuestBytes);
    desc.type = TextureCache::BindingType::Texture;
    auto &info = desc.info;
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = bits == 64 ? vk::Format::eR32G32Uint : vk::Format::eR32Uint;
    const u32 side = tiled ? 1024 : 64;
    info.size = {side, side, 1};
    info.pitch = side;
    info.num_bits = bits;
    info.resources = {.levels = 1, .layers = layers};
    info.tile_mode = tiled ? AmdGpu::TileMode::Thin2DThin : AmdGpu::TileMode::DisplayLinearAligned;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.props.is_tiled = tiled;
    info.alt_tile = tiled && pro;
    info.bank_swizzle = tiled ? 3 : 0;
    info.guest_address = reinterpret_cast<VAddr>(guest);
    const u32 max_levels = layers == 1 ? (tiled ? 11 : 7) : 3;
    auto full = info;
    full.resources.levels = max_levels;
    full.UpdateSize();
    assert(full.guest_size < GuestBytes);
    for (u32 mip = 0; mip < max_levels; mip++) {
        const auto &layout = full.mips_layout[mip];
        auto *words = reinterpret_cast<u32 *>(static_cast<u8 *>(guest) + layout.offset);
        std::fill_n(words, layout.size / 4, 0xa5000000u + mip);
    }
    desc.view_info.type =
        layers == 1 ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DArray;
    desc.view_info.format = info.pixel_format;
```

The scenario signature is `RunCase(instance, scheduler, runtime, cache, guest, layers, tiled, pro,
bits, jumps)`, with `GuestBytes = 16 << 20`, `side = tiled ? 1024 : 64`. The guest buffer is poisoned
with `0xcd` first, then every mip of the **full** chain is filled with a per-level sentinel
`0xa5000000u + mip` — so a later check can tell mip *m* from mip *n*, and tell "never uploaded"
(`0xcdcdcdcd`) from "uploaded the wrong level".

What it asserts:

1. **The backing is not re-created.** From the second declared level onward `image_uid` and
   `image.GetImage()` must be unchanged for the single-layer case:
   `if (layers == 1 && levels >= 3 && (image.image_uid != uid || image.GetImage() != handle))` →
   `"FAIL: mip growth %u recreated backing (uid %llu -> %llu)"`.
2. **Older guest data is never re-read.** Without the wrapping of `runtime_memory_region` the test
   cannot see uploads, so it wraps it and counts any read below `copy_floor = guest_address +
   mips_layout[levels - 1].offset` → `"FAIL: mip %u reread older guest data (%llu / %llu reads)"`.
   That is the assertion that pins the `first_mip` semantics.
3. **Pixels survive.** Every newly added mip is uploaded from guest data (`0xa5000000u + mip`),
   downloaded and compared per texel; afterwards each mip is `ClearImage`d to `0x12345678u + mip`
   on the GPU and re-checked with `gpu_written = true`, so the test proves GPU-written contents are
   preserved across later expansions.
4. **Bookkeeping matches the declared levels**: `image.info.guest_size != info.guest_size ||
   image.info.resources.levels != levels` fails.
5. **Arrayed images (layers > 1) must *not* reuse the backing** (`image.GetImage() == handle` →
   fail), pinning `CanGrowMipLevels`'s single-layer requirement.
6. A staging-budget case: 8 × 256 KiB uploads must cross a 1 MiB submission budget
   (`"PASS: 2 MiB batch crossed the 1 MiB staging submission budget"`), and
   `UploadReservationBytes(size) >= ref.size`, plus `>= ref.buffer->SizeBytes()` above 16 MiB.

Cases run: linear 64×64 and tiled 1024×1024, `pro` on/off, 32- and 64-bit texels, plus a
`jumps` variant that only declares levels 1, 2, 4, 7, 11 (to test the fallback when the chain is
not contiguous).

### B. Lands where (our tree)

| Change | Local site |
| --- | --- |
| `ExpandImage` in-place growth branch | `texture_cache.cpp:938` — currently always inserts a new image at `953-955` and copies at `1036` |
| `RefreshImage` gains `first_mip` | `texture_cache.cpp:1402`; loop at `1439`; hash short-circuit at `1461` |
| `CanGrowMipLevels` | new method on `Image`, `image.h:103`; local `Image` has no `backing_images.size()==1`/`scene_proxy` concept — map to our `backing`/`depth_id` model |
| backing mip preallocation | `UniqueImage::Create` in `image.cpp:116-139` (`Create` receives only `image_ci`); `image.h:71` |
| `RefreshImage(image, first_mip)` call sites | `texture_cache.cpp:649`, `1006`, and the `ResolveOverlap` path at `926-933` |

Our tree currently has the *opposite* optimisation in `ExpandImage`: an env-gated early return when
the existing image is already big enough (`texture_cache.cpp:938-953`), which avoids the
reallocation entirely but only when the requested size is not larger. B generalises it to "grow the
chain on the same backing", which is what a title that builds mips one level at a time actually
needs.

### B. Applicability to Déraciné

- Déraciné's assets are streamed and its render targets are re-described per pass; any title-side
  mip-chain growth currently costs a full image reallocation plus `CopyImage`. If Déraciné grows
  chains level-by-level the way WipEout does, B is a direct bytes-and-calls win.
- Even when the chain is not grown level-by-level, the `first_mip` compaction is a reusable idea
  for our `ExpandImage`: stage only the levels the new image actually adds.
- The test harness is the real deliverable. It shows how to drive `TextureCache` against a real
  Vulkan device with guest memory mapped through `libkernel` and **without launching a game** — we
  have no such fixture, and a Déraciné-side regression test for alias/mip behaviour would be worth
  more than the optimisation.
- The `runtime_memory_region` wrap trick is the pattern to copy for counting guest reads from a
  test: no production instrumentation needed.

### B. Risks / prerequisites

- Reserved capacity is *memory* the guest never declared: `bit_width(max(w,h))` levels of a 2880×3240
  RGBA8 image is 2880×3240×4 × (1 + 1/4 + 1/16 + …) ≈ 50 MiB instead of 37 MiB. For Déraciné's two
  eye buffers that is ~26 MiB extra, and the reservation happens on *every* eligible image, not
  just the ones that grow. Gate it on evidence, or reserve only when a growth has already been
  observed once for that shape.
- `image.info = info` mutates the image in place while `slot_image_views`, descriptor sets and
  `binding` still describe the old level count. B handles this with `needs_rebind` and
  `++registry_generation`; skipping either produces stale views that read undefined levels.
- The page-table registration deliberately covers only the declared range. Our `TrackImage`/
  `UntrackImage` and the fault handler (`InvalidateMemory`) must agree on that, or a CPU write into
  reserved-but-unwatched capacity will not invalidate the image.
- `first_mip != 0` disables the mip-hash protection. That is only sound because newly declared
  levels have never been uploaded; if the same code path is reused for a *non*-incremental refresh,
  GPU-written pixels get overwritten.
- The test is `EXCLUDE_FROM_ALL` and links the game runtime; it will not run in CI as-is.

---

## C. eFMann/shadPS4-Bloodborne-DLSS-FSR — profiling machinery

Repo: https://github.com/eFMann/shadPS4-Bloodborne-DLSS-FSR (branch `main`, no pinned SHA published
for these files; fetched 2026-11-17).
Files: `src/video_core/renderer_vulkan/vk_gpu_profiler.{h,cpp}`, `vk_bb_perf_meter.{h,cpp}`, plus
the scheduler-side timestamp plumbing in `vk_scheduler.{h,cpp}` that makes them work.

### C1. `GpuProfiler` — pooled marks, labelled intervals, delayed retrieval

Design in one sentence: a **ring of 4 frames**, each with its own **4096 timestamps** in one query
pool; every `Mark(key, describe)` writes one timestamp into the current slice's next slot; an
interval is the time between two consecutive timestamps; results are read **four frames later**.

```cpp
// src/video_core/renderer_vulkan/vk_gpu_profiler.h:4-8, 26-79
// GPU time per render pass, dispatch and upscaler run (after bbport's BB_GPU_PROFILE), for the
// Bloodborne threaded renderer. A timestamp is written where each of them starts (and at the
// frame end); the time to the next timestamp is charged to its label, barriers and copies
// recorded in between included. Results are read four frames later and logged every 10 s as
// GPU ms per frame by label. Switched on in the Graphics settings.
...
class GpuProfiler {
public:
    /// The profiler while it is switched on, else null.
    static GpuProfiler* Get() noexcept {
        return active;
    }
    /// Per game frame on the GPU command thread: follows the setting, closes the frame's last
    /// segment and reads an old frame's results.
    static void OnFrame(const Instance& instance, Scheduler& scheduler, bool enabled);

    /// Starts a segment labelled `key`; `describe` names it the first time the key is seen.
    template <typename Describe>
    void Mark(u64 key, Describe&& describe) {
        if (!described.contains(key)) {
            described.emplace(key, describe());
        }
        WriteTimestamp(key);
    }

    /// Whether `other` is the scheduler it records into (the presenter has its own).
    [[nodiscard]] bool Records(const Scheduler* other) const noexcept {
        return other == &scheduler;
    }

private:
    GpuProfiler(const Instance& instance, Scheduler& scheduler);
    void WriteTimestamp(u64 key);
    void BeginFrame();
    void Collect(u32 slice);
    void Print();

    static constexpr u32 NumSlices = 4;
    static constexpr u32 SliceQueries = 4096;
    static inline GpuProfiler* instance_ptr = nullptr;
    static inline GpuProfiler* active = nullptr;

    vk::Device device;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    double period_ns = 1.0;
    u32 slice = 0;
    std::array<std::vector<u64>, NumSlices> keys; ///< label of each timestamp but the last
    std::array<u32, NumSlices> used{};
    std::array<bool, NumSlices> pending{};
    std::unordered_map<u64, std::string> described;
    struct Total {
        double ms = 0;
        u64 segments = 0;
    };
    std::unordered_map<u64, Total> totals;
    u64 frames = 0;
    double usual_ms = 0.0; ///< recent GPU time per frame (slow frames logged against it)
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
```

Timestamp placement, frame close, and the delayed retrieval:

```cpp
// src/video_core/renderer_vulkan/vk_gpu_profiler.cpp:55-138 (excerpts)
void GpuProfiler::WriteTimestamp(u64 key) {
    if (used[slice] + 1 >= SliceQueries) {
        return; // the frame's slice is full: the rest of the frame goes to the last label
    }
    // Outside render passes; marks sit where a pass, dispatch or submission begins anyway.
    scheduler.EndRendering();
    const u32 query = slice * SliceQueries + used[slice]++;
    keys[slice].push_back(key);
    scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
    });
}

void GpuProfiler::BeginFrame() {
    // Close the frame: one more timestamp without a label.
    if (used[slice] > 0 && used[slice] < SliceQueries) {
        scheduler.EndRendering();
        const u32 query = slice * SliceQueries + used[slice]++;
        scheduler.Record([pool = *pool, query](vk::CommandBuffer cmdbuf) {
            cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, query);
        });
        pending[slice] = true;
    }
    slice = (slice + 1) % NumSlices;
    // The oldest slice: its frame was submitted four frames ago.
    if (pending[slice]) {
        Collect(slice);
    }
    if (used[slice]) {
        device.resetQueryPool(*pool, slice * SliceQueries, used[slice]);
    }
    used[slice] = 0;
    keys[slice].clear();
    pending[slice] = false;
    Print();
}

void GpuProfiler::Collect(u32 which) {
    const u32 count = used[which];
    std::vector<u64> stamps(count);
    // Four frames on this is complete unless the GPU lags that far: then wait for it.
    const auto result = device.getQueryPoolResults(
        *pool, which * SliceQueries, count, count * sizeof(u64), stamps.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
    if (result != vk::Result::eSuccess) {
        return;
    }
    ...
    for (u32 i = 0; i + 1 < count; ++i) {
        if (stamps[i + 1] < stamps[i]) {
            continue;
        }
        const double ms = double(stamps[i + 1] - stamps[i]) * period_ns / 1e6;
        auto& total = totals[keys[which][i]];
        total.ms += ms;
        ++total.segments;
        ...
    }
```

**How a query interval is associated with a caller.** By the `key`: the caller hashes the thing it
is measuring into a `u64` and supplies the human-readable name lazily through `describe()`, which
runs only the first time that key is seen (`described.contains(key)`). The canonical call site:

```cpp
// src/video_core/renderer_vulkan/vk_rasterizer.cpp:7692-7710
void Rasterizer::MarkPass(const GraphicsPipeline* pipeline, const RenderState& state) {
    ...
    auto* profiler = GpuProfiler::Get();
    if (!profiler || scheduler.IsRenderingWith(state)) {
        return;
    }
    const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto* ps = pipeline->GetStages()[static_cast<u32>(Shader::SwStage::Fragment)];
    u64 key = (u64(state.width) | u64(state.height) << 32) * 0x9E3779B97F4A7C15ull;
    key ^= vs.pgm_hash * 0xFF51AFD7ED558CCDull;
    key ^= (ps ? ps->pgm_hash : 0) * 0xC4CEB9FE1A85EC53ull;
    key ^= u64(state.num_color_attachments) << 7;
    profiler->Mark(key, [&] {
        return fmt::format("pass {}x{} ({} color{}) vs {:08x} ps {:08x}", state.width,
                           state.height, state.num_color_attachments,
                           state.depth_stencil_attachment.has_depth ? " + depth" : "",
                           vs.pgm_hash, ps ? ps->pgm_hash : 0);
    });
}
```

So the key is `(target size, vs hash, ps hash, colour-attachment count)` hashed multiplicatively —
collision-resistant without keeping names for every draw. Aggregation is per key, and the profiler
picks the top 8 labels of a slow frame and the top 30 of a 10-second window.

**How it is associated with a submission.** Two mechanisms:

1. Recording: the timestamps are recorded into the scheduler's own command buffer via
   `scheduler.Record(...)`, so an interval lives in exactly the submission whose command buffer
   holds it.
2. Slicing: `BeginFrame()` rotates a slice per frame, and a slice is only read after 4 rotations,
   i.e. its command buffer was submitted 4 frames ago. Correctness never depends on a fence, only
   on elapsed frames, with `eWait` as the fallback when the GPU is more than 4 frames behind.
3. Ownership: `Records(const Scheduler* other)` keeps the presenter's separate scheduler from
   writing into the same slice ring.

**What it does about attributing waits.** Honest answer: it does **not** subtract them from the
operation; it *labels* them. Two explicit measures:

- The header states the accounting rule, so no one is surprised: *"the time to the next timestamp is
  charged to its label, barriers and copies recorded in between included"*.
- The scheduler closes every submission with a synthetic label whose whole purpose is to absorb the
  GPU-idle gap, so the gap does not land on the last real operation:

```cpp
// src/video_core/renderer_vulkan/vk_scheduler.cpp:599-602
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
```

So the wait becomes the label `(between submissions: GPU idle)` — visible, and attributed to
nobody's operation.

### C2. The perf meter's per-submission busy time (`vk_bb_perf_meter.h` + scheduler)

`vk_bb_perf_meter.h` carries no Vulkan code; it is the consumer contract. Its GPU numbers come from
the scheduler's `TopOfPipe`/`BottomOfPipe` pair per submission:

```cpp
// src/video_core/renderer_vulkan/vk_bb_perf_meter.h:25-72 (excerpt)
/// What the meter's second gathered (the rasterizer's sampler thread fills it).
struct Inputs {
    double seconds{};
    /// Samples of the command thread's state (Common::GuestStats::CmdState order).
    std::array<u64, 7> states{};
    double recorder_busy_s{}; ///< the draw recorder's busy time (0 without the threaded renderer)
    double gpu_busy_s{};      ///< the GPU's busy time over the submissions it measured
    bool gpu_measured{};      ///< the GPU's timestamps came in
    double fps{};             ///< game frames per second
    double draws{};           ///< draws and dispatches handed to the draw recorder
    /// Every 8th draw's time by step on the command thread and on the recorder (summed ns) and
    /// how many draws were timed.
    std::array<double, 6> cmd_step_ns{};
    double cmd_timed{};
    std::array<double, 20> rec_step_ns{};
    double rec_timed{};
    /// Parts of the recorder's steps on the same draws (Rasterizer::RecDetail).
    std::array<double, 10> rec_detail_ns{};
    /// The draw recorder's finish thread (recorder on two threads): on, its busy time, and how
    /// often and how long the recorder waited for it, by reason (Rasterizer::FinishWait order).
    bool finisher_on{};
    double finisher_busy_s{};
    std::array<double, 5> finisher_waits{};
    std::array<double, 5> finisher_wait_s{};
    double cap_fps{}; ///< the frame rate cap held by the frame rate hooks (0: none)
    bool stalled{};   ///< long frames took a fifth of the second (loading, pipeline builds)
};

/// The last second's verdict, for the counter.
struct Reading {
    bool valid{};
    std::string limit; ///< "GPU", "command thread", "draw recorder", "game", "capped", ...
    double limit_load{};
    std::string next;
    double next_load{};
    double cmd_ms{}, rec_ms{}, gpu_ms{}; ///< busy time per frame
    double draws_per_frame{};
    std::string cmd_steps; ///< the command thread's largest steps per frame, "decode 3.1 ..."
};

/// From the sampler thread, once a second: works out the verdict, publishes it and adds it to
/// the minute's summary.
void Update(const Inputs& in);
/// The summary of what the meter gathered since the last one (it writes one every 20 s), at once:
/// when the game closes.
void Flush();
/// The last verdict (invalid when the meter has not run for a while).
Reading Get();
```

```cpp
// src/video_core/renderer_vulkan/vk_scheduler.cpp:849-872
void Scheduler::CollectGpuTiming() {
    while (!pending_timings.empty() && work_semaphore.IsFree(pending_timings.front().tick)) {
        const auto [tick, slot] = pending_timings.front();
        pending_timings.pop_front();
        std::array<u64, 2> stamps{};
        const auto result = instance.GetDevice().getQueryPoolResults(
            *timer_pool, slot * 2, 2, sizeof(stamps), stamps.data(), sizeof(u64),
            vk::QueryResultFlagBits::e64);
        if (result != vk::Result::eSuccess || stamps[1] < stamps[0]) {
            continue;
        }
        const u64 busy = static_cast<u64>((stamps[1] - stamps[0]) * timer_period_ns);
        gpu_busy_ns.fetch_add(busy, std::memory_order_relaxed);
        Common::GuestStats::gpu_timeline_busy_ns.fetch_add(busy, std::memory_order_relaxed);
        Common::GuestStats::gpu_timeline_submits.fetch_add(1, std::memory_order_relaxed);
        if (timer_last_end != 0 && stamps[0] > timer_last_end) {
            const u64 idle = static_cast<u64>((stamps[0] - timer_last_end) * timer_period_ns);
            gpu_idle_ns.fetch_add(idle, std::memory_order_relaxed);
            Common::GuestStats::gpu_timeline_idle_ns.fetch_add(idle, std::memory_order_relaxed);
        }
        timer_last_end = stamps[1];
        gpu_submits.fetch_add(1, std::memory_order_relaxed);
    }
}
```

with the slots written at submission boundaries:

```cpp
// src/video_core/renderer_vulkan/vk_scheduler.cpp:487-492 and :603-613
    if (timer_pool && gpu_timing && !timer_open) {
        session.primary.resetQueryPool(*timer_pool, timer_slot * 2, 2);
        session.primary.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *timer_pool,
                                       timer_slot * 2);
        timer_open = true;
    }
...
    if (timer_pool && timer_open && !sessions.empty()) {
        EndRendering();
        sessions.back().primary.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
                                               *timer_pool, timer_slot * 2 + 1);
        if (pending_timings.size() >= TimerPairs - 1) {
            pending_timings.pop_front(); // never read: its slot is about to be reused
        }
        pending_timings.push_back({signal_value, timer_slot});
        timer_slot = (timer_slot + 1) % TimerPairs;
        timer_open = false;
    }
```

`TimerPairs = 512` (`vk_scheduler.h:856`). This is the cleaner design of the two: one timestamp
pair per **submission** (not per draw), retired by the submission's **own timeline value**
(`work_semaphore.IsFree(pending_timings.front().tick)`), never by a device wait, and the gap
between the previous submission's end and this one's start is accumulated as `gpu_idle_ns`
separately — so dependency/idle time is excluded from "busy" by construction.

The consumer side:

```cpp
// src/video_core/renderer_vulkan/vk_rasterizer.cpp:7280-7288
    PerfMeter::Inputs in;
    ...
    in.gpu_busy_s = double(marks.gpu_busy_ns - meter_marks.gpu_busy_ns) / 1e9;
    in.gpu_measured = marks.gpu_submits != meter_marks.gpu_submits;
```

i.e. `gpu_busy_s` is a delta of a monotonic counter, and `gpu_measured` is a delta of the submit
count — a plain "did any submission report in this window" test, so the meter never divides by a
stale GPU figure.

### C. Lands where (our tree)

- `vk_gpu_profile.h` / `vk_gpu_profile.cpp` already exist and are a **different design**: one
  `MaxQueries = 16384` pool, one frame sampled every `SHADPS4_GPU_PROFILE=<seconds>`, a
  `Before(work, vertices)` / `After()` pair per draw, and host work attributed to the *next* draw
  (`HostWork` enum, `vk_gpu_profile.h:60-73`). It has no 4-slice ring, no `describe`-on-first-sight,
  no submission-level busy/idle split. C1 would be an addition beside it, not a port over it.
- `vk_scheduler.h` has **no** query pool or `TimerPairs`; `vk_scheduler.cpp` has no
  `getQueryPoolResults`/`writeTimestamp` at all. C2's scheduler plumbing is entirely new work; the
  insertion points are `Scheduler::Submit` (where `EndSession`/`QueueSubmit` happen) and
  `Scheduler::DeferOperation`/`PopPendingOperations` (`vk_scheduler.h:523`, `.cpp:885`) for the
  retirement path — the same tick-gated hook A2 needs.
- `vk_gpu_profile.cpp:242-244` reads the pool **immediately** after a frame with no `eWait`, then
  resets it. C1's `eWait` + 4-frame delay is the safer pattern and the one to copy if the local
  profiler ever produces `stamps[i + 1] < stamps[i]` garbage on a lagging GPU.

### C. Applicability to Déraciné

- We need exactly one number C gives cheaply and correctly: **GPU busy time per submission vs
  total frame time**. Two eyes at 2880×3240 make `gpu_busy_s / seconds` the only way to tell an
  OpenXR-side pacing problem from a renderer cost.
- C1's key-on-shape labelling (size + shader hash + attachment count) maps directly onto Déraciné's
  pass set and needs no per-pass registration: add `MarkPass` at our pass-begin site and the pass
  table builds itself.
- C1's `(between submissions: GPU idle)` label is the diagnostic we lack: with OpenXR, the GPU sits
  idle between the game's submissions and the runtime's composition, and without that label the
  local per-draw profiler silently charges the idle gap to whichever draw comes next.
- C2's `TimerPairs` design is 512 × 2 timestamps for the whole process — cheap enough to leave
  always-on behind a debug setting, unlike a per-draw profiler.
- The `PerfMeter::Reading.limit` idea (name the bottleneck: "GPU", "command thread", "game",
  "capped") is what makes the data actionable for a 2-eyes-90 Hz target.

### C. Risks / prerequisites

- `GpuProfiler::Mark` calls `scheduler.EndRendering()` before writing a timestamp. That **breaks
  render passes** to measure them; on our tree with two-eye passes this changes what is measured and
  can serialise things that would otherwise overlap. Use `writeTimestamp2` with
  `eAllCommands`/`eBottomOfPipe` at pass boundaries only if the driver allows timestamps inside a
  pass; otherwise accept the split and document it.
- The interval model ("time to the next timestamp") charges barriers and copies **between** passes
  to the *preceding* label. For a renderer that spends its time in barriers between aliased targets
  (ours), this systematically inflates whichever pass precedes the barrier.
- `NumSlices = 4` frames of delay assumes the GPU is never more than 4 frames behind. With
  `eWait` that is a stall, not a wrong number — but on a 90 Hz title a stall at frame close is
  exactly the artefact the profile is supposed to find. `TimerPairs = 512` has the same property,
  handled by dropping the slot (`never read: its slot is about to be reused`).
- The perf meter (`vk_bb_perf_meter.*`) is written around `Common::GuestStats::*` and the
  threaded-renderer/`Rasterizer::FinishWait` split that this fork has and we do not. Port
  `Inputs`/`Reading`/`Update` shape, not the plumbing.
- No pinned commit SHA exists for these files; they are `main`-branch content and may drift. If the
  numbers matter for a decision, freeze the file hashes in the port commit.

---

## D. Assessment-only commits

Short assessment of five commits worth knowing about; only the reusable part of each is described.
These are **not** recommended as wholesale ports.

### D1. cuesta4 flush epochs — `114fb00be304ce663c4eefbe315774ac1613e2d6`

URL: https://github.com/cuesta4/shadPS4/commit/114fb00be304ce663c4eefbe315774ac1613e2d6
*cuesta4/shadPS4, "video_core: replace flush barriers with flush epochs", +145/−51, adds
`src/video_core/flush_epoch.h`.*

Reusable idea, verbatim from the message: `EVENT_WRITE` cache flushes and `ACQUIRE_MEM` no longer
end the rendering scope and record a global barrier; they "only advance a flush epoch and accumulate
their masks". Three rules survive: a buffer bound read-write twice in one state gets a barrier if a
flush happened since its last write ("Resource tracking alone never orders that case"); pipelines
that access memory through device addresses "still get the accumulated global barrier before they
run and at the next flush after them"; "Image barriers are unchanged from upstream". It also stops
the download drain from ending rendering up front (each download does it right before its copy) and
narrows upload barriers in `SynchronizeBuffer` to the uploaded span. For Déraciné, the reusable
15-line `flush_epoch.h` counter and the "read-write twice + flush since last write" rule are worth
taking alone; the barrier removal itself is a wide blast radius on a title we cannot yet measure.
**Lands where:** local barriers come from `vk_scheduler`'s `EndRendering`/`DeferOperation` and the
texture-cache `RefreshImage`/`CopyImage` paths; `flush_epoch` would be a new header beside
`preserve_tracker.h`.

### D2. WipEout resident-memory slabs — `21ecc2402db871ab253d0fbf52879d0c09ca671f`

URL: https://github.com/AbdelmoumenDeghbouche/wipeout_pc/commit/21ecc2402db871ab253d0fbf52879d0c09ca671f
*"Buffer cache: resident ranges take their memory from 8 MiB slabs (#213)", +162/−30, adds
`buffer_cache/resident_slab.h` + `tests/test_resident_slab.cpp`.*

Reusable idea: `EnsureResident` asked the driver for one allocation per call, and a frame that
first uses many ranges "made hundreds of driver allocations and stalled". Memory now comes from
8 MiB slabs handed out front-to-back, never freed, so a slab only needs a fill level; a range that
does not fit in the remainder is "split across slabs, so nothing is wasted"; if the driver refuses
a whole slab, the range falls back to an exact allocation. This is the buffer-side twin of A2 and
A3: **the same shape as our image problem**, and the only one of the five with a unit test for the
allocator logic alone (`test_resident_slab.cpp`, fill-level logic, no GPU). For Déraciné it is
relevant only if buffer-cache allocation shows up in a profile; the slab-fill-level pattern is
better borrowed than the code.
**Lands where:** local `vk_resource_pool.h`/`.cpp` (command pools and descriptor heaps only) and
`video_core/buffer_cache/` — neither is in this task's read scope beyond the pool file.

### D3. WipEout per-slice fast clear — `db815126c4` (full SHA `db815126c41ab7ab48db89f4ff86460b5e2d10b3`)

URL: https://github.com/AbdelmoumenDeghbouche/wipeout_pc/commit/db815126c41ab7ab48db89f4ff86460b5e2d10b3
*"Fast clear: a CMask write for one slice clears that slice, not the whole target (#218)",
`vk_rasterizer.cpp` only, +68/−11.*

Reusable idea, and a correctness fix rather than an optimisation: the fast-clear shortcut was keyed
by the CMask of a target's **first slice**, so one face-write of a cube was taken as "clear the
whole layered target", blanking faces that had not been written yet (WipEout's ship reflections
flickered). Now "a write that holds less than every slice's CMask clears one slice and marks that
slice alone: the first slice at the entry's own address, a later one when the write lies a whole
number of its own size above a layered target's CMask", and a write covering every slice behaves as
before. The first four one-slice clears are logged. **Our tree has the same class of bug surface** —
`ClearMeta`-style fast clear on a layered target — and Déraciné's VR path draws layered |eye|
targets. Highest value-per-line of the five, but it needs the CMask/`MetaType` machinery in
`texture_cache.h` (`enum class MetaType { CMask, FMask, HTile }`) to be exercised, which we have not
verified.
**Lands where:** the fast-clear entry point in `renderer_vulkan/vk_rasterizer.cpp` (not in this
task's read scope); the meta bookkeeping lives at `texture_cache.h` (`MetaType`, `surface_metas`).

### D4. NHL 24 persistent Vulkan driver cache — `31b6c78764f441bcae704b478ed150948d3939ea`

URL: https://github.com/HajDEO/nhl24pc/commit/31b6c78764f441bcae704b478ed150948d3939ea
*"Keep the driver's Vulkan pipeline cache between runs", `vk_pipeline_cache.cpp/.h`, +89/−4.*

Reusable idea: the `VkPipelineCache` was created empty on every start, so the start-up warm-up made
the NVIDIA driver recompile every saved pipeline each time — "about 100 s with ~3000 cached shaders,
~85% of the main thread's time in the driver". The fix loads `<cache dir>/<serial>.vkcache` as
`VkPipelineCacheCreateInfo::pInitialData`, saves it again after the warm-up only if it grew, via
temporary file + rename, and logs the warm-up time. The driver ignores data that does not match the
GPU or driver version, and a rejected cache falls back to an empty one with a warning. For Déraciné
this is startup-time only, and it is a **one-file, low-risk change** — the best effort/benefit ratio
in this section if cold starts matter for iteration.
**Lands where:** `renderer_vulkan/vk_pipeline_cache.cpp` (not read in this task); note A's commit
does the same thing differently (moves the cache into the game shader cache), so pick one.

### D5. Driveclub texture-cache GC — `28eb6c6bb0e108fdb5e35f29befda1ca523e1524`

URL: https://github.com/ondrejeder/Driveclub-pc-wrapper/commit/28eb6c6bb0e108fdb5e35f29befda1ca523e1524
*"driveclub-pc: texture GC no longer stalls on images it cannot free", `texture_cache.cpp/.h`,
+67/−7.*

Reusable idea, and **we have the bug**: an image the collector skips (GPU-written tiled, or one that
needs a download with no memory pressure) consumed a deletion for the pass and stayed at the front
of the LRU queue; once the oldest images were all of that kind, passes freed nothing and texture
memory only grew. The fix moves such an image to the back of the queue for free and caps a pass at
256 image visits, with `DCPC_GC_SKIP_FIX=0` to revert, a 10-second log of what the collector did
under pressure, and `DCPC_GC_BUDGET_MB=<n>` to lower thresholds for testing. Measured under forced
pressure: "120-318 images freed and 546-1006 moved back per 10 s, 87.9 FPS, normal picture".

Our code decrements the deletion budget **before** the skip checks:

```cpp
// src/video_core/texture_cache/texture_cache.cpp:1712-1737 (local, wt-crossfork)
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        if (False(image.flags & ImageFlagBits::Registered)) {
            ...
            return false;
        }
        const bool download = image.SafeToDownload();
        const bool tiled = image.info.IsTiled();
        if (tiled && download) {
            // This is a workaround for now. We can't handle non-linear image downloads.
            return false;
        }
        if (download && !pressured) {
            return false;
        }
```

That is exactly the pattern Driveclub's commit fixes. **Lands where:** `RunGarbageCollector`
(`texture_cache.cpp:1689`) and `clean_up` (`1712-1750`), plus `lru_cache.ForEachItemBelow` at
`1754`/`1759`. A move-to-back needs an `lru_cache` API that our tree may not expose
(`Touch`/`Insert`/`Free` are used at `texture_cache.cpp:1563`, `1781`).

---

## E. Upstream shadPS4 PRs (shadps4-emu/shadPS4)

Reference text only; these may already be ported locally, and the point here is to capture the
reference, not to re-derive it. All five are merged. "Changed functions" are the functions named by
the hunk contexts of the PR's diff.

PR URLs (each merged commit is also linked directly):

- #5444 https://github.com/shadps4-emu/shadPS4/pull/5444 → commit `699009c94932c64080391cf08d8131fc653ecee8`
- #5433 https://github.com/shadps4-emu/shadPS4/pull/5433 → commit `d3ff0e0cc2c85729b9b7ad08766ada576a7f9bf7`
- #5453 https://github.com/shadps4-emu/shadPS4/pull/5453 → commit `ee0364a9c17ab7ee39888e839a928d936309134a`
- #5599 https://github.com/shadps4-emu/shadPS4/pull/5599 → commit `6add101d85bc50e3cc79c7e9627aa6479700461e`
- #4953 https://github.com/shadps4-emu/shadPS4/pull/4953 → commit `4f48eabe699ac2fbfadba258eddcec778d6d99b1`

### #5444 — "texture_cache: don't copy stale CPU-dirty image contents in ExpandImage"

- Merged commit: `699009c94932c64080391cf08d8131fc653ecee8`
- File: `src/video_core/texture_cache/texture_cache.cpp` (+11/−2)
- Changed function: `TextureCache::ExpandImage`
- Intent: only carry the source image's contents into a new backing when they hold GPU-written data;
  a CPU-dirty source is stale and copying it overwrites the fresh upload.

```cpp
// PR #5444 diff (texture_cache.cpp, ExpandImage)
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

**Our tree:** not present. Local `ExpandImage` copies unconditionally
([texture_cache.cpp:1036](../../src/src/video_core/texture_cache/texture_cache.cpp#L1036)) and has no
`HasGpuValidData()`; the closest thing is
`True(flags & GpuModified) && False(flags & Dirty)` inline in `image.h`.

### #5433 — "texture_cache: keep subresource data when its parent image is first created"

- Merged commit: `d3ff0e0cc2c85729b9b7ad08766ada576a7f9bf7`
- File: `src/video_core/texture_cache/texture_cache.cpp` (+12/−5)
- Changed function: `TextureCache::ResolveOverlap`
- Intent: when a subresource needs a larger merged image and none exists yet, create it there and
  then, otherwise the GPU data is dropped and reads/writes go to different images.

```cpp
// PR #5433 diff (texture_cache.cpp, ResolveOverlap)
-                // We need to have a larger, already allocated image to copy this one into
-                if (merged_image_id) {
-                    auto& merged_image = slot_images[merged_image_id];
-                    runtime.CopyMip(&cache_image, &merged_image, mip, slice);
-                    FreeImage(cache_image_id);
+                // If we don't have an allocated image, we create one here
+                // Otherwise it's later created from guest memory and the GPU data is lost
+                // And reads/writes go to different images.
+                if (!merged_image_id) {
+                    merged_image_id =
+                        slot_images.Insert(instance, runtime, slot_image_views, image_info);
+                    RegisterImage(merged_image_id);
+                    RefreshImage(slot_images[merged_image_id]);
+                    TrackImage(merged_image_id);
                 }
+                auto& merged_image = slot_images[merged_image_id];
+                runtime.CopyMip(&cache_image, &merged_image, mip, slice);
+                FreeImage(cache_image_id);
```

**Our tree:** not present. Local `ResolveOverlap` still has the `if (merged_image_id)` guard
([texture_cache.cpp:917-930](../../src/src/video_core/texture_cache/texture_cache.cpp#L917-L930)), so
a missing merged image silently skips the copy.

### #5453 — "texture_cache: clamp mip levels to the full mip chain"

- Merged commit: `ee0364a9c17ab7ee39888e839a928d936309134a`
- Files: `src/video_core/amdgpu/resource.h` (+7/−1),
  `src/video_core/texture_cache/image_view.cpp` (+1/−1)
- Changed functions: `AmdGpu::Image::NumLevels()` and
  `ImageViewInfo::ImageViewInfo(const AmdGpu::Image&, const Shader::ImageResource&)`
- Intent: a guest that declares more mip levels than the extent's full chain actually has got
  levels that would be 1×1, which Vulkan does not allow and which makes uploads run past the image.

```cpp
// PR #5453 diff (amdgpu/resource.h, AmdGpu::Image::NumLevels)
-        return last_level + 1;
+        // Guests may declare more levels than the extent's full mip chain has (the extra ones
+        // would be 1x1), which Vulkan does not allow and makes uploads run past the image.
+        const u32 mip_width = width + 1;
+        const u32 mip_height = height + 1;
+        const u32 mip_depth = GetType() == ImageType::Color3D ? depth + 1 : 1;
+        const u32 max_levels = std::bit_width(std::max(std::max(mip_width, mip_height), mip_depth));
+        return std::min<u32>(last_level + 1, max_levels);
```

```cpp
// PR #5453 diff (texture_cache/image_view.cpp, ImageViewInfo ctor)
-    range.base.level = image.base_level;
+    range.base.level = std::min<u32>(image.base_level, image.NumLevels() - 1);
```

- **Our tree:** no `texture_cache/*.h` function changes; only `image_view.cpp`, which we have
  (`texture_cache/image_view.cpp`). Note the interaction with **B**: B's `host_mips` is
  `std::bit_width(std::max(width, height))` on the *guest* image and `mipLevels` is
  `std::max(host_mip_levels, info.resources.levels)` — with a guest that over-declares levels (the
  exact case #5453 fixes), B would try to create an image with more levels than Vulkan allows.
  Port #5453 **before** B, or clamp inside the same change.

### #5599 — "video_core: Refresh images stored right after a GPU-written image"

- Merged commit: `6add101d85bc50e3cc79c7e9627aa6479700461e`
- File: `src/video_core/texture_cache/texture_cache.cpp` (+16/−9)
- Changed function: `TextureCache::InvalidateMemoryFromGPU`
- Intent: a GPU write to a plane must also dirty the image stored immediately after it (e.g. chroma
  planes after luma), not only the image whose base address matches exactly.

```cpp
// PR #5599 diff (texture_cache.cpp, InvalidateMemoryFromGPU)
-    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
-        // Only consider images that match base address.
-        // TODO: Maybe also consider subresources
-        if (image.info.guest_address != address) {
-            return;
-        }
-        image.flags |= ImageFlagBits::GpuDirty;
-    });
+    // Mark the image at the written address, then follow images stored right after it
+    // (e.g. the chroma planes after a luma plane).
+    const VAddr end = address + max_size;
+    VAddr next = address;
+    while (next < end) {
+        VAddr next_end = 0;
+        ForEachImageWithAddress(next, [&](ImageId image_id, Image& image) {
+            if (image.info.guest_address != next) {
+                return;
+            }
+            // Ensure image is reuploaded when accessed again.
+            image.flags |= ImageFlagBits::GpuDirty;
+            next_end = std::max<VAddr>(next_end, next + image.info.guest_size);
+        });
+        next = next_end > next ? next_end : end;
+    }
```

**Our tree:** not present. Local
[texture_cache.cpp:345-356](../../src/src/video_core/texture_cache/texture_cache.cpp#L345-L356) is the
old exact-base-address-only loop, verbatim, including the `TODO: Maybe also consider
subresources`. This is a **correctness** fix relevant to Déraciné's multi-plane surfaces.

### #4953 — "Fix GT Sport menu rendering"

- Merged commit: `4f48eabe699ac2fbfadba258eddcec778d6d99b1`
- Files/functions touched under `src/video_core/texture_cache/`:
  - `image.cpp`: `Image::Image` (ctor — backing/format/view setup), `Image::GetBarriers`
  - `image.h`: `Image` members/`ImageViewInfo` plumbing (+14/−3)
  - `image_info.h`: `ImageInfo` (+15/−2)
  - `image_view.cpp`: `ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer&, ...)`,
    `ImageView::ImageView`
  - `image_view.h`: `ImageViewInfo` member
  - `texture_cache.cpp`: `TextureCache::FindImage` (+8/−0)
  - `texture_cache.h`: new declaration (+4/−0)
  - outside the cache: `renderer_vulkan/vk_rasterizer.cpp` (`BindIndexBuffer`, `BindTextures`),
    `vk_runtime.{cpp,h}` (`Transit`, `CopyImage`, `SetBackingSamples`)
- Intent: menu rendering in GT Sport — a set of depth/format/view-compatibility fixes bundled with a
  `FindImage` change. Multi-purpose; read the diff before porting any single hunk.

---

## E2. Summary table

| Source | What it optimises | Expected benefit class | Prerequisites |
| --- | --- | --- | --- |
| A1 (`52a2446`, overwrite-aware refresh) | Skips guest upload + detile of an image that an alias copy is about to replace whole | **bytes** (one full image upload per alias materialisation; ~37 MiB/eye at our resolution) | alias authority model (`AliasState`, `alias_generation`, `image_uid`) ported first; colour-only; depth/HTILE out of scope |
| A2 (`52a2446`, `ImageRecycler`) | Reuses `VkImage`s by creation shape; destroys off-thread; avoids driver alloc/free inside frames | **calls per frame** (allocator syscalls) + **latency** | tick-gated `DeferOperation` verified in our scheduler; `SameShape` must include our `view_formats`; 128 MiB/32 image/2 s budget |
| A3 (`52a2446`, shared-block suballocation) | Aliased images skip dedicated VMA allocations (which serialise with presentation) | **latency** (fewer allocator stalls), some memory cost | VMA block suballocation accepted for aliased images; fall back to dedicated on failure; do not hand-roll `vkCreateImage`+`vmaAllocateMemory` |
| B (`ca4662a`, mip growth) | Grows a mip chain in place on a pre-reserved backing; stages only the new levels | **bytes** (no reallocation copy, smaller uploads) + **calls per frame** (no `vkCreateImage`) | `CanGrowMipLevels` compatibility gate; reserved-capacity memory accounting (~+35% for a full chain); rebind after `info` mutation |
| B test harness (`test_mip_growth_gpu.cpp`) | Not an optimisation: a device-level fixture that counts guest reads via `--wrap=runtime_memory_region` | n/a (enables regression testing) | `EXCLUDE_FROM_ALL`; requires a real Vulkan device; not CI-friendly as-is |
| C1 (`vk_gpu_profiler.*`) | Pooled timestamp marks, 4-frame delayed retrieval, label-by-key, explicit GPU-idle label | **diagnosis only** (no frame-time win) | timestamps outside render passes (`EndRendering` per mark); 4-frame lag assumption; `eWait` fallback |
| C2 (`vk_bb_perf_meter.*` + scheduler `TimerPairs`) | One `TopOfPipe`/`BottomOfPipe` pair per submission; busy vs idle separated; retired by the submission's own timeline value | **diagnosis only** | scheduler query pool + `CollectGpuTiming` (absent locally); `TimerPairs = 512` slots |
| D1 flush epochs (`114fb00`) | `EVENT_WRITE`/`ACQUIRE_MEM` advance an epoch instead of forcing global barriers | **calls per frame** (barriers) | read-write-twice + flush-since-last-write rule; device-address pipelines still need the global barrier; image barriers unchanged |
| D2 resident slabs (`21ecc24`) | 8 MiB slabs for resident buffer ranges instead of one allocation per call | **calls per frame** | buffer-cache residency path; slab fill-level bookkeeping; exact-allocation fallback |
| D3 per-slice fast clear (`db815126`) | A one-slice CMask write clears/marks that slice, not the whole layered target | correctness (fixes flicker) + fewer wasted clears | CMask/FMask/HTile meta bookkeeping present and exercised; layered targets |
| D4 persistent driver cache (`31b6c78`) | `VkPipelineCache` loaded from disk as initial data, saved after warm-up | **latency** (cold start; minutes → seconds) | cache dir + serial naming; reject-and-fall-back on mismatch; do not also do A's shader-cache move |
| D5 texture GC skip fix (`28eb6c6`) | A skipped image no longer consumes a deletion or blocks the LRU front; pass capped at 256 visits | **latency** (GC passes stop spinning) + memory growth fixed | `lru_cache` move-to-back API; deletion budget decremented only on a real free |
| E #5444 (`699009c`) | `ExpandImage` does not copy stale CPU-dirty contents | **bytes** (one full image copy avoided) | `HasGpuValidData()`-style predicate |
| E #5433 (`d3ff0e0`) | Creates the merged overlap image instead of dropping the subresource data | correctness + **bytes** (avoids later re-upload) | `ResolveOverlap` restructure |
| E #5453 (`ee0364a`) | Clamps view mip levels to the full chain | correctness | `image_view.cpp` + `AmdGpu::Image` accessors |
| E #5599 (`6add101`) | Dirties images stored immediately after a GPU-written one | correctness | `ForEachImageWithAddress` helper |
| E #4953 (`4f48eab`) | GT Sport menu fixes: depth/format/view compatibility + `FindImage` | correctness (bundled) | read the whole diff; no single hunk is self-contained |
