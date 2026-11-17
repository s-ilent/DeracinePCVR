// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <xxhash.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/staging_diag.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"
#include "core/vr/frame_timeline.h"
#include "video_core/texture_cache/preserve_tracker.h"
#include "video_core/texture_cache/alias_trace.h"
#include "video_core/texture_cache/image_stats.h"
#include <vk_mem_alloc.h>

namespace VideoCore {
namespace {
// Counts of the overlap-resolution entry points, once a second, so a capture can be checked
// against the scene it was taken in rather than trusted on its own.
std::atomic<u32> g_depth_overlap{};
std::atomic<u32> g_overlap{};
std::atomic<u32> g_expand{};
std::atomic<u32> g_expand_block{};
std::atomic<u32> g_expand_3d{};
std::atomic<u32> g_expand_res{};
std::atomic<u32> g_branch_direct{};
std::atomic<u32> g_branch_buffered{};
std::atomic<u32> g_branch_ms{};
std::atomic<u32> g_copyimage{};
std::atomic<u32> g_draws{};
std::atomic<long long> g_last_report{};
void NoteOverlap(const char* which) {
    ImageStats::Report();
    if (std::getenv("SHADPS4_DBG_OVERLAP") == nullptr) {
        return;
    }
    if (std::strcmp(which, "depth") == 0) {
        g_depth_overlap.fetch_add(1);
    } else if (std::strcmp(which, "expand") == 0) {
        g_expand.fetch_add(1);
    } else if (std::strcmp(which, "expand_block") == 0) {
        g_expand_block.fetch_add(1);
    } else if (std::strcmp(which, "expand_3d") == 0) {
        g_expand_3d.fetch_add(1);
    } else if (std::strcmp(which, "expand_res") == 0) {
        g_expand_res.fetch_add(1);
    } else if (std::strcmp(which, "direct") == 0) {
        g_branch_direct.fetch_add(1);
    } else if (std::strcmp(which, "depthpass") == 0) {
        g_branch_direct.fetch_add(1);
    } else if (std::strcmp(which, "buffered") == 0) {
        g_branch_buffered.fetch_add(1);
    } else if (std::strcmp(which, "msreinterpret") == 0) {
        g_branch_ms.fetch_add(1);
    } else if (std::strcmp(which, "copyimage") == 0) {
        g_copyimage.fetch_add(1);
    } else if (std::strcmp(which, "draw") == 0) {
        g_draws.fetch_add(1);
    }
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    long long last = g_last_report.load();
    if (now - last >= 1000 && g_last_report.compare_exchange_strong(last, now)) {
        LOG_INFO(Render_Vulkan,
                 "OVERLAPS depth={} direct={} buffered={} msreinterp={} expand={} copyimage={} draws={} skipped={} cleared_after={} loaded_after={} exp_block={} exp_3d={} exp_res={} passes={} alias_clear={} alias_load={} alias_sample={}",
                 g_depth_overlap.exchange(0), g_branch_direct.exchange(0),
                 g_branch_buffered.exchange(0), g_branch_ms.exchange(0), g_expand.exchange(0),
                 g_copyimage.exchange(0), g_draws.exchange(0),
                 PreserveTracker::skipped_total.exchange(0),
                 PreserveTracker::cleared_after_skip.exchange(0),
                 PreserveTracker::loaded_after_skip.exchange(0),
                 g_expand_block.exchange(0), g_expand_3d.exchange(0), g_expand_res.exchange(0),
                 Core::Vr::FrameTimeline::TakeRenderPasses(),
                 Alias3DTracker::used_cleared.exchange(0),
                 Alias3DTracker::used_loaded.exchange(0),
                 Alias3DTracker::used_sampled.exchange(0));
    }
}} // Anonymous namespace



static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

// SHADPS4_DBG_TRACE_IMAGES=1: every image the cache takes on or lets go of, and every request
// answered with an image of another size, in the log (issue #19).
static bool TraceImages() {
    static const bool trace = [] {
        const char* value = std::getenv("SHADPS4_DBG_TRACE_IMAGES");
        return value != nullptr && value[0] == '1';
    }();
    return trace;
}

static void TraceImage(std::string_view what, const ImageInfo& info, u32 index) {
    LOG_INFO(Render_Vulkan, "IMAGES {} {} at {:#x}+{:#x} {}x{} {} levels {} layers {}{}{}", what,
             index, info.guest_address, info.guest_size, info.size.width, info.size.height,
             vk::to_string(info.pixel_format), info.resources.levels, info.resources.layers,
             info.props.is_depth ? " depth" : "",
             info.meta_info.htile_addr ? fmt::format(" htile {:#x}", info.meta_info.htile_addr)
                                       : std::string{});
}
static constexpr std::size_t MaxViewDimensionMismatchRecords = 64;

static void LogViewDimensionMismatch(const TextureCache::ImageDesc& desc, const Image& image) {
    if (IsViewTypeCompatible(desc.view_info.type, image.info.type)) {
        return;
    }

    const auto& requested = desc.info;
    const auto& backing = image.info;
    const auto requested_micro = AmdGpu::GetMicroTileMode(requested.tile_mode);
    const auto backing_micro = AmdGpu::GetMicroTileMode(backing.tile_mode);
    const u32 active_backing_samples = image.backing ? image.backing->num_samples : 0;
    const std::string actual_vk_type =
        image.backing ? vk::to_string(image.backing->image.image_ci.imageType) : "None";
    const std::string actual_vk_format =
        image.backing ? vk::to_string(image.backing->image.image_ci.format) : "None";
    const std::string actual_vk_samples =
        image.backing ? vk::to_string(image.backing->image.image_ci.samples) : "None";
    const std::string record = fmt::format(
        "VIEW_DIMENSION_MISMATCH binding={} view={} requested_type={} backing_type={} "
        "request_addr={:#x} request_size={:#x} backing_addr={:#x} backing_size={:#x} "
        "request_extent={}x{}x{} request_pitch={} request_bpp={} request_samples={} "
        "request_levels={} request_layers={} request_tile={} request_array={} "
        "request_micro={} request_thickness={} request_format={} "
        "backing_extent={}x{}x{} backing_pitch={} backing_bpp={} backing_samples={} "
        "active_backing_samples={} actual_vk_type={} actual_vk_format={} actual_vk_samples={} "
        "backing_levels={} backing_layers={} backing_tile={} "
        "backing_array={} backing_micro={} backing_thickness={} backing_format={} "
        "view_format={} mip_base={} mip_count={} layer_base={} layer_count={} storage={} "
        "history_texture={} history_storage={} history_rt={} history_depth={} history_vo={} "
        "bound={} target={} usage_flags={}",
        magic_enum::enum_name(desc.type), magic_enum::enum_name(desc.view_info.type),
        magic_enum::enum_name(requested.type), magic_enum::enum_name(backing.type),
        requested.guest_address, requested.guest_size, backing.guest_address, backing.guest_size,
        requested.size.width, requested.size.height, requested.size.depth, requested.pitch,
        requested.num_bits, requested.num_samples, requested.resources.levels,
        requested.resources.layers, magic_enum::enum_name(requested.tile_mode),
        magic_enum::enum_name(requested.array_mode), magic_enum::enum_name(requested_micro),
        AmdGpu::GetMicroTileThickness(requested.array_mode),
        vk::to_string(requested.pixel_format), backing.size.width, backing.size.height,
        backing.size.depth, backing.pitch, backing.num_bits, backing.num_samples,
        active_backing_samples, actual_vk_type, actual_vk_format, actual_vk_samples,
        backing.resources.levels, backing.resources.layers,
        magic_enum::enum_name(backing.tile_mode), magic_enum::enum_name(backing.array_mode),
        magic_enum::enum_name(backing_micro), AmdGpu::GetMicroTileThickness(backing.array_mode),
        vk::to_string(backing.pixel_format), vk::to_string(desc.view_info.format),
        desc.view_info.range.base.level, desc.view_info.range.extent.levels,
        desc.view_info.range.base.layer, desc.view_info.range.extent.layers,
        desc.view_info.is_storage, image.usage.texture, image.usage.storage,
        image.usage.render_target, image.usage.depth_target, image.usage.vo_surface,
        image.binding.is_bound, image.binding.is_target, vk::to_string(image.usage_flags));

    static std::mutex records_mutex;
    static std::unordered_set<std::string> records;
    {
        std::scoped_lock lock{records_mutex};
        if (records.size() >= MaxViewDimensionMismatchRecords || !records.emplace(record).second) {
            return;
        }
    }
    LOG_ERROR(Render_Vulkan, "{}", record);
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)},
      readback_linear_images{EmulatorSettings.IsReadbackLinearImagesEnabled()} {
    // The per-second counters report this allocator's own device-memory total; it is the only
    // place that can see the allocator at report time.
    ImageStats::SetAllocator(instance.GetAllocator());
    // Create basic null image at fixed image ID.
    const auto null_id = GetNullImage(vk::Format::eR8G8B8A8Unorm);
    ASSERT(null_id.index == NULL_IMAGE_ID.index);

    // Set up garbage collection parameters.
    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
#ifdef ENABLE_BACHATA_RUNTIME
    // GPU memory is system memory here and the budget is small: start dropping images that have
    // not been used for a while long before the desktop thresholds, which exceed the budget.
    pressure_gc_memory = static_cast<u64>(device_local_memory * 4 / 10);
    critical_gc_memory = static_cast<u64>(device_local_memory * 6 / 10);
    trigger_gc_memory = 0;
    return;
#endif
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
}

TextureCache::~TextureCache() = default;

ImageId TextureCache::GetNullImage(const vk::Format format) {
    const auto existing_image = null_images.find(format);
    if (existing_image != null_images.end()) {
        return existing_image->second;
    }

    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.num_bits = 32;
    info.UpdateSize();

    const ImageId null_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    auto& image = slot_images[null_id];
    Vulkan::SetObjectName(instance.GetDevice(), image.GetImage(),
                          fmt::format("Null Image ({})", vk::to_string(format)));

    image.flags = ImageFlagBits::Empty;
    image.track_addr = image.info.guest_address;
    image.track_addr_end = image.info.guest_address + image.info.guest_size;

    null_images.emplace(format, null_id);
    return null_id;
}

void TextureCache::ProcessDownloadImages() {
    for (const ImageId image_id : download_images) {
        DownloadImageMemory(image_id);
    }
    download_images.clear();
}

void TextureCache::DownloadImageMemory(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return;
    }
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 download_size = image.info.pitch * image.info.size.height *
                              image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(download_size <= image.info.guest_size);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy image_download = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), image_download);

    scheduler.DeferPriorityOperation(
        [this, device_addr = image.info.guest_address, download, download_size] {
            Core::Memory::Instance()->TryWriteBacking(std::bit_cast<u8*>(device_addr), download,
                                                      download_size);
        });
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        // Initialize hash
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            // Modified region overlaps image, so the image was definitely accessed by this fault.
            // Untrack the image, so that the range is unprotected and the guest can write freely.
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            // This page access may or may not modify the image.
            // We should not mark it as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            // Remove tracking from this page only.
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            // This page access does not modify the image but the page should be untracked.
            // We should not mark this image as dirty now. If it really was modified
            // it will receive more invalidations on its other pages.
            UntrackImageTail(image_id);
        } else {
            // Image begins and ends on this page so it can not receive any more invalidations.
            // We will check it's hash later to see if it really was modified.
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    // Mark the image at the written address, then follow the images stored right after it
    // (e.g. the chroma planes after a luma plane), as long as they lie inside the written range.
    const VAddr end = address + max_size;
    VAddr next = address;
    while (next < end) {
        VAddr next_end = 0;
        ForEachImageInRegion(next, end - next, [&](ImageId image_id, Image& image) {
            if (image.info.guest_address != next) {
                return;
            }
            // Ensure the image is reuploaded when accessed again.
            image.flags |= ImageFlagBits::GpuDirty;
            next_end = std::max<VAddr>(next_end, next + image.info.guest_size);
        });
        next = next_end > next ? next_end : end;
    }
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        // TODO: Download image data back to host.
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    ScopedTimer timed{ImageStats::depth_ns};
    NoteOverlap("depth");
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    // If an image in the cache has less slices we need to expand it
    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        // The guest requires a depth sampled texture, but cache can offer only Rxf. Need to
        // recreate the image.
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        // If the guest is going to use previously created depth as storage, the image needs to be
        // recreated. (TODO: Probably a case with linear rgba8 aliasing is legit)
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        // Render target can have only Rxf format. If the cache contains only Dx[S8] we need to
        // re-create the image.
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        // The guest has requested previously allocated texture to be bound as a depth target.
        // In this case we need to convert Rx float to a Dx[S8] as requested
        recreate |= !cache_image.info.props.is_depth;

        // The guest is trying to bind a depth target and cache has it. Need to be sure that aspects
        // and bpp match
        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
        RegisterImage(new_image_id);

        // Re-fetch references after slot_images.insert may have reallocated storage
        auto& new_image = slot_images[new_image_id];
        auto& source_image = slot_images[cache_image_id];
        new_image.usage = source_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        // When creating a depth buffer through overlap resolution don't clear it on first use.
        new_image.info.meta_info.htile_clear_mask = 0;
        ReprTrace::Note(source_image.info.guest_address,
                        static_cast<u32>(source_image.info.pixel_format),
                        static_cast<u32>(new_image.info.pixel_format), static_cast<u32>(binding),
                        static_cast<u64>(source_image.info.size.width) *
                            source_image.info.size.height * source_image.info.num_bits / 8);

        // Content preservation is only needed when the new representation will be read as
        // data (a texture or a storage image). A colour or depth target is drawn into, and
        // its previous contents live in the other representation, where they are not the
        // depth or colour values the guest expects anyway. This is the condition the NHL
        // 24 fork uses for the same per-frame depth/colour aliasing pattern.
        static const bool nhl_condition = std::getenv("SHADPS4_DBG_NHL_COND") != nullptr;
        const bool contents_matter = binding == BindingType::Texture ||
                                     binding == BindingType::Storage;
        // The conservative predicate: contents may be dropped only when a later use cannot
        // read them. Nothing proves that yet for this title - its depth targets are bound
        // with a load, not a clear - so this falls back to preserving unless a future
        // clear intent is propagated into the request.
        static const bool conservative = std::getenv("SHADPS4_DBG_CONSERVATIVE") != nullptr;
        const bool preserve_contents = conservative || !nhl_condition || contents_matter;
        if (!preserve_contents) {
            PreserveTracker::NoteSkipped(cache_image.info.guest_address);
        }
        if (AliasTrace::Enabled() && AliasTrace::Hits(new_info.guest_address, new_info.guest_size)) {
            const u64 copy_bytes = static_cast<u64>(source_image.info.size.width) *
                                   source_image.info.size.height * source_image.info.num_bits / 8;
            AliasTrace::recreations.fetch_add(1, std::memory_order_relaxed);
            if (preserve_contents) {
                AliasTrace::recreations_preserved.fetch_add(1, std::memory_order_relaxed);
                AliasTrace::copied_bytes.fetch_add(copy_bytes, std::memory_order_relaxed);
            }
            const u64 key = 2u ^ (static_cast<u64>(binding) << 8) ^
                            (static_cast<u64>(source_image.info.pixel_format) << 16) ^
                            (static_cast<u64>(new_image.info.pixel_format) << 32) ^
                            (static_cast<u64>(preserve_contents) << 48);
            if (AliasTrace::FirstTime(key)) {
            LOG_INFO(Render_Vulkan,
                     "ALIAS seq={} op=representation binding={} {}->{} type={}->{} "
                     "{}x{}x{} -> {}x{}x{} levels={}->{} layers={}->{} samples={}->{} "
                     "stencil={}->{} preserve={} copy_bytes={}",
                     AliasTrace::NextSeq(), static_cast<u32>(binding),
                     vk::to_string(source_image.info.pixel_format),
                     vk::to_string(new_image.info.pixel_format),
                     static_cast<u32>(source_image.info.type),
                     static_cast<u32>(new_image.info.type), source_image.info.size.width,
                     source_image.info.size.height, source_image.info.size.depth,
                     new_image.info.size.width, new_image.info.size.height,
                     new_image.info.size.depth, source_image.info.resources.levels,
                     new_image.info.resources.levels, source_image.info.resources.layers,
                     new_image.info.resources.layers, source_image.info.num_samples,
                     new_image.info.num_samples, source_image.info.props.has_stencil ? 1 : 0,
                     new_image.info.props.has_stencil ? 1 : 0, preserve_contents ? 1 : 0,
                     copy_bytes);
            }
        }
        if (preserve_contents && source_image.info.num_samples == 1 &&
            new_info.num_samples == 1) {            // Perform depth<->color copy using the intermediate copy buffer.
            // The buffer-mediated path needs room for the whole picture: the utility buffer is
            // a stream buffer whose regions must be reserved before use. It used to be taken
            // with Handle() alone and no reservation, so a 5760x3240 copy wrote past its
            // capacity.
            static const bool via_buffer = [] {
                const char* value = std::getenv("SHADPS4_DBG_COPY_VIA_BUFFER");
                return value != nullptr && value[0] == '1';
            }();
            // Carry a colour picture into a depth image by writing it through the depth unit
            // instead of copying it: the words are the same 32-bit floats, but a copy from a
            // colour format into a depth one makes the driver detile and decompress the whole
            // depth surface first. SHADPS4_DBG_DEPTH_PASS=0 puts the image copy back.
            static const bool via_depth_pass = [] {
                const char* value = std::getenv("SHADPS4_DBG_DEPTH_PASS");
                return value == nullptr || value[0] != '0';
            }();
            const bool color_into_depth = !source_image.info.props.is_depth &&
                                          new_info.props.is_depth &&
                                          source_image.info.pixel_format == vk::Format::eR32Sfloat &&
                                          (new_info.pixel_format == vk::Format::eD32Sfloat ||
                                           new_info.pixel_format == vk::Format::eD32SfloatS8Uint);
            if (via_depth_pass && color_into_depth) {
                source_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                     vk::AccessFlagBits2::eShaderRead, {});
                new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
                blit_helper.CopyColorToDepth32(new_info.size.width, new_info.size.height,
                                               source_image.GetImage(),
                                               source_image.info.pixel_format,
                                               new_image.GetImage(), new_info.pixel_format);
                NoteOverlap("depthpass");
            } else if (instance.IsMaintenance8Supported() && !via_buffer) {
                new_image.CopyImage(source_image);
                NoteOverlap("direct");
                {
                    static const bool trace_fmt = std::getenv("SHADPS4_DBG_PAIRFMT") != nullptr;
                    if (trace_fmt) {
                        static std::atomic<u32> traced{};
                        if (traced.fetch_add(1) < 40) {
                            LOG_INFO(Render_Vulkan,
                                     "PAIRFMT src={} dst={} depth={}/{} bpp={}/{} samples={}/{} aspect={:#x}/{:#x}",
                                     vk::to_string(source_image.info.pixel_format),
                                     vk::to_string(new_image.info.pixel_format),
                                     static_cast<int>(source_image.info.props.is_depth),
                                     static_cast<int>(new_image.info.props.is_depth),
                                     source_image.info.num_bits, new_image.info.num_bits,
                                     source_image.info.num_samples, new_image.info.num_samples,
                                     static_cast<u32>(source_image.aspect_mask),
                                     static_cast<u32>(new_image.aspect_mask));
                        }
                    }
                }
            } else {
                // A whole 5760x3240 picture is 74.6 MB; the utility stream buffer is 32 MB when
                // maintenance8 is present, and a stream buffer cannot grow to hold it. The
                // transfer gets a buffer of its own, big enough, made once.
                const u64 bytes_per_pixel = source_image.info.num_bits / 8;
                const u64 needed = static_cast<u64>(source_image.info.size.width) *
                                   source_image.info.size.height * bytes_per_pixel;
                static vk::Buffer preserve_buffer{};
                static u64 preserve_capacity{};
                {
                    static std::once_flag once;
                    std::call_once(once, [&] {
                        const VkBufferCreateInfo buffer_ci = static_cast<VkBufferCreateInfo>(
                            vk::BufferCreateInfo{
                                .size = 128_MB,
                                .usage = vk::BufferUsageFlagBits::eTransferSrc |
                                         vk::BufferUsageFlagBits::eTransferDst,
                            });
                        const VmaAllocationCreateInfo alloc_ci = {
                            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
                        VkBuffer buffer{};
                        VmaAllocation allocation{};
                        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer,
                                            &allocation, nullptr) == VK_SUCCESS) {
                            preserve_buffer = buffer;
                            preserve_capacity = 128_MB;
                        }
                    });
                }
                if (preserve_buffer && needed <= preserve_capacity) {
                    new_image.CopyImageWithBuffer(source_image, preserve_buffer, 0);
                    NoteOverlap("buffered");
                } else {
                    LOG_ERROR(Render_Vulkan,
                              "Depth overlap copy needs {} bytes, more than the {} provided", needed,
                              preserve_capacity);
                }
            }
        } else if (source_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            // Perform a rendering pass to transfer the channels of source as samples in dest.
            source_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height,
                new_image.backing->image.image_ci.samples, source_image.info.pixel_format,
                new_info.pixel_format, source_image.GetImage(), new_image.GetImage());
            NoteOverlap("msreinterpret");
        } else {
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
        }

        // Free the cache image.
        FreeImage(cache_image_id);
        return new_image_id;
    }

    // Will be handled by view
    return cache_image_id;
}

ImageId TextureCache::ResolveDimensionOverlap(const ImageInfo& requested_info,
                                              ImageId cache_image_id) {
    NoteOverlap("overlap");
    auto& cache_image = slot_images[cache_image_id];
    if (!NeedsViewTypeRecreation(requested_info.type, cache_image.info.type)) {
        return {};
    }

    const auto& cached_info = cache_image.info;
    const bool is_supported_single_row =
        requested_info.size.width == cached_info.size.width && requested_info.size.height == 1 &&
        cached_info.size.height == 1 && requested_info.size.depth == 1 &&
        cached_info.size.depth == 1 && requested_info.resources.levels == 1 &&
        cached_info.resources.levels == 1 && requested_info.resources.layers == 1 &&
        cached_info.resources.layers == 1 && requested_info.num_samples == 1 &&
        cached_info.num_samples == 1 && requested_info.pixel_format == cached_info.pixel_format &&
        requested_info.tile_mode == cached_info.tile_mode;
    if (!is_supported_single_row) {
        LOG_ERROR(Render_Vulkan,
                  "Unsupported image dimension recreation: requested={} backing={} "
                  "requested_extent={}x{}x{} backing_extent={}x{}x{}",
                  magic_enum::enum_name(requested_info.type),
                  magic_enum::enum_name(cached_info.type), requested_info.size.width,
                  requested_info.size.height, requested_info.size.depth, cached_info.size.width,
                  cached_info.size.height, cached_info.size.depth);
        return {};
    }

    const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
    const u64 copy_bytes = static_cast<u64>(cached_info.size.width) * cached_info.size.height *
                           cached_info.size.depth * cached_info.num_bits / 8;
    if (copy_bytes > copy_buffer.SizeBytes()) {
        LOG_ERROR(Render_Vulkan, "Image dimension recreation needs {} bytes, buffer has {}",
                  copy_bytes, copy_buffer.SizeBytes());
        return {};
    }

    const auto& new_info = requested_info;
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, new_info);
    RegisterImage(new_image_id);

    auto& new_image = slot_images[new_image_id];
    auto& source_image = slot_images[cache_image_id];
    new_image.usage = source_image.usage;

    // Fold any pending CPU or buffer-cache changes into the old backing before moving its
    // authoritative contents through a dimension-neutral buffer copy.
    RefreshImage(source_image);
    new_image.CopyImageWithBuffer(source_image, copy_buffer.Handle(), 0);
    new_image.flags &= ~ImageFlagBits::Dirty;
    new_image.flags |= source_image.flags & ImageFlagBits::GpuModified;

    if (source_image.binding.is_bound || source_image.binding.is_target) {
        source_image.binding.needs_rebind = 1u;
    }
    FreeImage(cache_image_id);
    return new_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id) {
    ScopedTimer timed{ImageStats::overlap_ns};
    auto& cache_image = slot_images[cache_image_id];
    if (AliasTrace::Enabled() &&
        (AliasTrace::Hits(image_info.guest_address, image_info.guest_size) ||
         AliasTrace::Hits(cache_image.info.guest_address, cache_image.info.guest_size))) {
        AliasTrace::requests.fetch_add(1, std::memory_order_relaxed);
        AliasTrace::Report();
        const u64 key = 1u ^ (static_cast<u64>(binding) << 8) ^
                        (static_cast<u64>(image_info.pixel_format) << 16) ^
                        (static_cast<u64>(cache_image.info.pixel_format) << 32) ^
                        (static_cast<u64>(image_info.props.is_depth) << 48) ^
                        (static_cast<u64>(cache_image.info.props.is_depth) << 49);
        if (AliasTrace::FirstTime(key)) {
            LOG_INFO(Render_Vulkan,
                     "ALIAS seq={} op=request binding={} asked(addr={:#x} size={:#x} fmt={} type={} "
                     "{}x{}x{} levels={} layers={} samples={} bits={} depth={} stencil={} tile={}) "
                     "cached(addr={:#x} size={:#x} fmt={} {}x{}x{} levels={} layers={} samples={} "
                     "depth={} stencil={} tile={})",
                     AliasTrace::NextSeq(), static_cast<u32>(binding), image_info.guest_address,
                     image_info.guest_size, vk::to_string(image_info.pixel_format),
                     static_cast<u32>(image_info.type), image_info.size.width,
                     image_info.size.height, image_info.size.depth, image_info.resources.levels,
                     image_info.resources.layers, image_info.num_samples, image_info.num_bits,
                     image_info.props.is_depth ? 1 : 0, image_info.props.has_stencil ? 1 : 0,
                     static_cast<u32>(image_info.tile_mode), cache_image.info.guest_address,
                     cache_image.info.guest_size, vk::to_string(cache_image.info.pixel_format),
                     cache_image.info.size.width, cache_image.info.size.height,
                     cache_image.info.size.depth, cache_image.info.resources.levels,
                     cache_image.info.resources.layers, cache_image.info.num_samples,
                     cache_image.info.props.is_depth ? 1 : 0,
                     cache_image.info.props.has_stencil ? 1 : 0,
                     static_cast<u32>(cache_image.info.tile_mode));
        }
    }
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    // Equal address
    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            // Very likely this kind of overlap is caused by allocation from a pool.
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        if (const auto dimension_image_id =
                ResolveDimensionOverlap(image_info, cache_image_id)) {
            return {dimension_image_id, -1, -1};
        }

        // Compressed view of uncompressed image with same block size.
        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            NoteOverlap("expand_block");
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            NoteOverlap("expand_3d");
            {
                static const bool trace_pair = std::getenv("SHADPS4_DBG_ALIAS3D") != nullptr;
                if (trace_pair) {
                    static std::atomic<u32> traced{};
                    if (traced.fetch_add(1) < 200) {
                        LOG_INFO(Render_Vulkan,
                                 "ALIAS3D addr={:#x} cached(type={} {}x{}x{} mips={} layers={}) "
                                 "requested(type={} {}x{}x{} mips={} layers={})",
                                 cache_image.info.guest_address,
                                 static_cast<u32>(cache_image.info.type),
                                 cache_image.info.size.width,
                                 cache_image.info.size.height,
                                 cache_image.info.size.depth,
                                 cache_image.info.resources.levels,
                                 cache_image.info.resources.layers,
                                 static_cast<u32>(image_info.type), image_info.size.width,
                                 image_info.size.height, image_info.size.depth,
                                 image_info.resources.levels,
                                 image_info.resources.layers);
                    }
                }
            }
            Alias3DTracker::NoteExpanded(cache_image.info.guest_address);
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size and resources are less than or equal, use image view.
        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_format_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            const bool is_view_compatible =
                IsViewTypeCompatible(image_info.type, result_image.info.type);
            return {is_format_compatible && is_view_compatible ? result_id : ImageId{}, -1, -1};
        }

        // Size and resources are greater, expand the image.
        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            NoteOverlap("expand_res");
            return {ExpandImage(image_info, cache_image_id), -1, -1};
        }

        // Size is greater but resources are not, because the tiling mode is different.
        // Likely the address is reused for a image with a different tiling mode.
        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        // Enhanced debug logging for unreachable case
        // Calculate expected size based on format and dimensions
        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  // Old image details
                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, +cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,

                  // New image details
                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,

                  // Comparison
                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  // Size ratios
                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  // Difference between actual and expected sizes with percentages
                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    // Right overlap, the image requested is a possible subresource of the image from cache.
    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        // Image isn't a subresource but a chance overlap.
        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        // Left overlap, the image from cache is a possible subresource of the image requested
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                // We have a larger image created and a separate one, representing a subres of it
                // bound as render target. In this case we need to rebind render target.
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                // If we don't have an allocated image, create one here. Otherwise it is created
                // later from guest memory, the GPU data of this subresource is lost, and reads
                // and writes end up in different images.
                if (!merged_image_id) {
                    merged_image_id = slot_images.insert(instance, scheduler, blit_helper,
                                                         slot_image_views, image_info);
                    RegisterImage(merged_image_id);
                    RefreshImage(slot_images[merged_image_id]);
                    TrackImage(merged_image_id);
                }
                auto& merged_image = slot_images[merged_image_id];
                merged_image.CopyMip(slot_images[cache_image_id], mip, slice);
                // The parent now holds GPU-authored texels for that subresource, which guest
                // memory does not have - that is the whole reason #5433 exists. Without the flag,
                // SafeToDownload() answers false and a later expansion under #5444's gate would
                // drop exactly what this copy preserved.
                merged_image.flags |= ImageFlagBits::GpuModified;
                FreeImage(cache_image_id);
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, ImageId image_id) {
    ScopedTimer timed{ImageStats::expand_ns};
    {
        // Expanding an image to the size it already has only re-allocates it and copies its
        // contents to the same place. The cache already holds an image big enough.
        static const bool skip_empty_expand = [] {
            const char* value = std::getenv("SHADPS4_DBG_SKIP_EMPTY_EXPAND");
            return value != nullptr && value[0] == '1';
        }();
        const auto& existing = slot_images[image_id];
        if (skip_empty_expand && existing.info.size.width >= info.size.width &&
            existing.info.size.height >= info.size.height &&
            existing.info.resources.levels >= info.resources.levels &&
            existing.info.resources.layers >= info.resources.layers) {
            return image_id;
        }
    }
    NoteOverlap("expand");
    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    RegisterImage(new_image_id);

    auto& src_image = slot_images[image_id];
    auto& new_image = slot_images[new_image_id];

    ReprTrace::Note(src_image.info.guest_address,
                    static_cast<u32>(src_image.info.pixel_format),
                    static_cast<u32>(new_image.info.pixel_format), 0xffu,
                    static_cast<u64>(src_image.info.size.width) * src_image.info.size.height *
                        src_image.info.num_bits / 8);

    if (AliasTrace::Enabled() &&
        AliasTrace::Hits(src_image.info.guest_address, src_image.info.guest_size)) {
        AliasTrace::expands.fetch_add(1, std::memory_order_relaxed);
        const u64 key = 3u ^ (static_cast<u64>(src_image.info.pixel_format) << 8) ^
                        (static_cast<u64>(src_image.info.type) << 16) ^
                        (static_cast<u64>(src_image.info.resources.layers) << 24) ^
                        (static_cast<u64>(new_image.info.resources.layers) << 32);
        if (AliasTrace::FirstTime(key)) {
            LOG_INFO(Render_Vulkan,
                 "ALIAS seq={} op=expand addr={:#x} fmt={} {}x{}x{} levels={} layers={} -> "
                 "{}x{}x{} levels={} layers={}",
                 AliasTrace::NextSeq(), src_image.info.guest_address,
                 vk::to_string(src_image.info.pixel_format), src_image.info.size.width,
                 src_image.info.size.height, src_image.info.size.depth,
                 src_image.info.resources.levels, src_image.info.resources.layers,
                 new_image.info.size.width, new_image.info.size.height,
                 new_image.info.size.depth, new_image.info.resources.levels,
                 new_image.info.resources.layers);
        }
    }

    // An existing measuring aid: when the copy is known to reach everything, the whole refresh can
    // be skipped. Kept as it was; the coverage below is the predicate that can actually prove it.
    const bool copy_covers_all =
        src_image.info.size.width >= new_image.info.size.width &&
        src_image.info.size.height >= new_image.info.size.height &&
        src_image.info.resources.levels >= new_image.info.resources.levels &&
        src_image.info.resources.layers >= new_image.info.resources.layers;
    static const bool skip_covered_refresh =
        std::getenv("SHADPS4_DBG_SKIP_COVERED_REFRESH") != nullptr;
    // Whether the new image takes the old image's contents at all. Written as one condition
    // because the coverage below is only sound while the copy that follows really runs: upstream
    // #5444 gates this copy on the source holding GPU-written data
    // (`src_image.SafeToDownload() || src_image.info.num_samples > 1` - the second term because
    // RefreshImage declines multisampled images, so for those the copy is the only writer). Porting
    // it means putting exactly that expression here: the coverage skip collapses to "skip nothing"
    // when the copy will not run, which is why the assertion below exists.
    const bool src_has_gpu_data = src_image.SafeToDownload();
    // RefreshImage declines multisampled images, so for those the copy is the only writer.
    const bool carries_contents = src_has_gpu_data || src_image.info.num_samples > 1;
    // What the copy is going to write, level by level. Only levels it does not reach are read
    // from guest memory: the rest would be overwritten before anything could read them.
    // SHADPS4_DBG_COVERED_REFRESH=1 turns it on; the shapes it decides on are logged once each.
    static const bool covered_refresh = [] {
        const char* value = std::getenv("SHADPS4_DBG_COVERED_REFRESH");
        return value != nullptr && value[0] == '1';
    }();
    const CopyCoverage covered = covered_refresh && carries_contents
                                     ? CoveringCopyOf(src_image.info, new_image.info)
                                     : CopyCoverage{};
    ImageStats::NoteExpand(!covered.CoversNothing());
    if (covered_refresh) {
        const u64 key = 0xC0DE0000ull ^ (static_cast<u64>(src_image.info.pixel_format) << 8) ^
                        (static_cast<u64>(new_image.info.pixel_format) << 16) ^
                        (static_cast<u64>(src_image.info.size.width) << 24) ^
                        (static_cast<u64>(src_image.info.size.height) << 40) ^
                        (static_cast<u64>(src_image.info.resources.levels) << 56) ^
                        (static_cast<u64>(new_image.info.resources.levels) << 60);
        if (AliasTrace::FirstTime(key)) {
            LOG_INFO(Render_Vulkan,
                     "COVERED_REFRESH src={:#x} dst={:#x} src={}x{} levels={} layers={} "
                     "dst={}x{} levels={} layers={} fmt={}/{} mips={} layers={} depth={}/{}",
                     src_image.info.guest_address, new_image.info.guest_address,
                     src_image.info.size.width, src_image.info.size.height,
                     src_image.info.resources.levels, src_image.info.resources.layers,
                     new_image.info.size.width, new_image.info.size.height,
                     new_image.info.resources.levels, new_image.info.resources.layers,
                     vk::to_string(src_image.info.pixel_format),
                     vk::to_string(new_image.info.pixel_format), covered.mips, covered.layers,
                     static_cast<int>(src_image.info.props.is_depth),
                     static_cast<int>(new_image.info.props.is_depth));
        }
    }
    if (covered.CoversNothing()) {
        if (!copy_covers_all || !skip_covered_refresh) {
            RefreshImage(new_image);
        }
    } else {
        RefreshImage(new_image, &covered);
    }
    // The property that keeps this coherent: a level whose upload was skipped is a level the copy
    // is about to write. Adopting #5444 without moving the coverage gate with it breaks exactly
    // this, and breaks it silently - the skip is opt-in, so the corruption would only appear when
    // someone turned the measurement switch on.
    ASSERT(carries_contents || covered.CoversNothing());
    if (carries_contents) {
        new_image.CopyImage(src_image);
    }

    if (src_image.binding.is_bound || src_image.binding.is_target) {
        src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    new_image.flags &= ~ImageFlagBits::Dirty;
    if (carries_contents) {
        new_image.flags |= ImageFlagBits::GpuModified;
    }
    return new_image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    if (info.guest_address == 0) [[unlikely]] {
        return GetNullImage(info.pixel_format);
    }

    std::scoped_lock lock{mutex};
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookups);

    auto& found = found_images[((info.guest_address >> 6) ^ (info.guest_address >> 17) ^
                                static_cast<u64>(info.pixel_format)) %
                               found_images.size()];
    if (found.registrations == registrations && found.guest_address == info.guest_address &&
        found.guest_size == info.guest_size && found.size == info.size &&
        found.resources == info.resources && found.pixel_format == info.pixel_format &&
        found.type == info.type && found.exact_fmt == exact_fmt) {
        // The image may have changed since; what made it the answer is checked again.
        Image& image = slot_images[found.image_id];
        if (image.info.guest_address == info.guest_address &&
            image.info.guest_size == info.guest_size && image.info.size == info.size &&
            IsVulkanFormatCompatible(image.info.pixel_format, info.pixel_format) &&
            IsViewTypeCompatible(info.type, image.info.type) &&
            (!exact_fmt || info.pixel_format == image.info.pixel_format) &&
            !(image.info.resources < info.resources)) {
            image.tick_accessed_last = scheduler.CurrentTick();
            TouchImage(image);
            Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookupsShort);
            return found.image_id;
        }
    }

    ImageIds image_ids;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId image_id, Image& image) { image_ids.push_back(image_id); });

    ImageId image_id{};

    // Check for a perfect match first
    for (const auto& cache_id : image_ids) {
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            !IsViewTypeCompatible(info.type, cache_image.info.type)) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
    }

    const ImageId perfect_match = image_id;

    // Try to resolve overlaps (if any)
    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& cache_id : image_ids) {
            view_mip = -1;
            view_slice = -1;

            const auto& merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id);
            if (overlap_image_id) {
                image_id = overlap_image_id;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            // Cannot reuse this image as we need the exact requested format.
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            // The image was clearly picked up wrong.
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    // Create and register a new image
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);
    if (TraceImages() && !perfect_match &&
        (image.info.size != info.size || image.info.guest_address != info.guest_address)) {
        TraceImage(fmt::format("asked for (type {}) answered with {} {}x{} mip {} slice {}:",
                               static_cast<int>(desc.type), image_id.index,
                               image.info.size.width, image.info.size.height, view_mip,
                               view_slice),
                   info, 0);
    }

    if (perfect_match && image_id == perfect_match) {
        found = {
            .guest_address = info.guest_address,
            .guest_size = info.guest_size,
            .size = info.size,
            .resources = info.resources,
            .pixel_format = info.pixel_format,
            .type = info.type,
            .exact_fmt = exact_fmt,
            .image_id = image_id,
            .registrations = registrations,
        };
    }

    // If the image requested is a subresource of the image from cache record its location.
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindTextureImage(const AmdGpu::Image& sharp,
                                       const Shader::ImageResource& resource, u32 index,
                                       ImageDesc& desc) {
    std::array<u64, 4> words;
    static_assert(sizeof(AmdGpu::Image) == sizeof(words));
    std::memcpy(words.data(), &sharp, sizeof(words));
    // Everything else ImageDesc and the mip level of the binding depend on.
    const u64 use = u64{resource.is_depth} | u64{resource.is_atomic} << 1 |
                    u64{resource.is_array} << 2 | u64{resource.is_written} << 3 |
                    u64{resource.is_r128} << 4 |
                    u64{static_cast<u32>(resource.mip_fallback_mode)} << 8 |
                    u64{resource.constant_mip_index} << 16 | u64{index} << 40;
    const u64 hash = (words[0] * 0x9E3779B97F4A7C15ULL) ^ (words[1] * 0xC2B2AE3D27D4EB4FULL) ^
                     (words[2] * 0x165667B19E3779F9ULL) ^ ((words[3] + use) * 0x27D4EB2F165667C5ULL);
    auto& found = found_textures[(hash ^ (hash >> 31)) % found_textures.size()];
    const auto describe = [&](ImageDesc& out) {
        out = ImageDesc{sharp, resource};
        if (resource.mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
            out.view_info.range.base.level += resource.constant_mip_index;
            out.view_info.range.extent.levels = 1;
        } else if (resource.mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
            out.view_info.range.base.level += index;
            out.view_info.range.extent.levels = 1;
        }
    };
    if (!Vulkan::FrameStats::AbOff("textures")) {
        ImageId remembered{};
        {
            std::scoped_lock lock{mutex};
            if (found.registrations == registrations && found.sharp == words &&
                found.use == use && !slot_images[found.image_id].binding.needs_rebind) {
                Image& image = slot_images[found.image_id];
                Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookups);
                Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageLookupsShort);
                desc = found.desc;
                image.tick_accessed_last = scheduler.CurrentTick();
                TouchImage(image);
                remembered = found.image_id;
            }
        }
        if (remembered) {
            // SHADPS4_DBG_CHECK_RECOGNIZE=1: what the long way finds as well, and a line in the
            // log where the two differ.
            static const bool check = [] {
                const char* value = std::getenv("SHADPS4_DBG_CHECK_RECOGNIZE");
                return value != nullptr && value[0] == '1';
            }();
            if (check) {
                static u64 checked = 0;
                static u64 differed = 0;
                ImageDesc long_way;
                describe(long_way);
                const ImageId long_id = FindImage(long_way);
                ++checked;
                if (long_id != remembered || long_way.view_info != desc.view_info ||
                    long_way.type != desc.type ||
                    long_way.info.guest_address != desc.info.guest_address ||
                    long_way.info.guest_size != desc.info.guest_size ||
                    long_way.info.pixel_format != desc.info.pixel_format) {
                    ++differed;
                    LOG_ERROR(Render_Vulkan,
                              "Texture at {:#x}: remembered as image {}, the long way finds {} "
                              "({} of {} checks differed)",
                              desc.info.guest_address, remembered.index, long_id.index, differed,
                              checked);
                } else if (checked % 100000 == 0) {
                    LOG_INFO(Render_Vulkan, "Textures remembered: {} checked, {} differed",
                             checked, differed);
                }
            }
            return remembered;
        }
    }

    describe(desc);
    u64 before;
    {
        std::scoped_lock lock{mutex};
        before = registrations;
    }
    const ImageId image_id = FindImage(desc);
    std::scoped_lock lock{mutex};
    if (!image_id || registrations != before) {
        return image_id;
    }
    // Remembered only where the image is the one asked for as it is, which is what holds
    // until an image comes or goes (as in FindImage).
    const Image& image = slot_images[image_id];
    const auto& info = desc.info;
    if (image.info.guest_address == info.guest_address &&
        image.info.guest_size == info.guest_size && image.info.size == info.size &&
        IsVulkanFormatCompatible(image.info.pixel_format, info.pixel_format) &&
        IsViewTypeCompatible(info.type, image.info.type) &&
        !(image.info.resources < info.resources)) {
        found.sharp = words;
        found.use = use;
        found.registrations = registrations;
        found.image_id = image_id;
        found.desc = desc;
    }
    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        // Sometimes image size might not exactly match with requested buffer size
        // If we only found 1 candidate image use it without too many questions.
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView& TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    LogViewDimensionMismatch(desc, image);
    if (desc.type == BindingType::Storage) {
        image.flags |= ImageFlagBits::GpuModified;
        if (readback_linear_images && !image.info.props.is_tiled && image.info.guest_address != 0) {
            download_images.emplace(image_id);
        }
    }
    UpdateImage(image_id);
    return image.FindView(desc.view_info);
}

ImageView& TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    if (readback_linear_images && !image.info.props.is_tiled) {
        download_images.emplace(image_id);
    }
    image.usage.render_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this color buffer
    if (desc.info.meta_info.cmask_addr) {
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return image.FindView(desc.view_info, false);
}

ImageView& TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
    Image& image = slot_images[image_id];
    image.flags |= ImageFlagBits::GpuModified;
    image.usage.depth_target = 1u;
    UpdateImage(image_id);

    // Register meta data for this depth buffer
    if (desc.info.meta_info.htile_addr) {
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    // If there is a stencil attachment, link depth and stencil.
    if (desc.info.stencil_addr != 0) {
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
            RegisterImage(stencil_id);
        }
        Image& image = slot_images[image_id];
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    // The description says whether depth is written, which makes no difference to the view (a
    // depth buffer is never a storage image). Draws with and without depth writes get the same
    // one, and with it the same render pass.
    auto view_info = desc.view_info;
    view_info.is_storage = true;
    return image.FindView(view_info, false);
}

void TextureCache::RefreshImage(Image& image, const CopyCoverage* covered) {
    ScopedTimer timed{ImageStats::refresh_ns};
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return;
    }

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    if (True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty)) {
        // The image size should be less than page size to be considered MaybeCpuDirty
        // So this calculation should be very uncommon and reasonably fast
        // For now we'll just check up to 64 first pixels
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u32 w = std::min(image.info.size.width, u32(8));
        const u32 h = std::min(image.info.size.height, u32(8));

        const u32 s_w = image.info.props.is_block ? Common::DivCeil(w, 4u) : w;
        const u32 s_h = image.info.props.is_block ? Common::DivCeil(h, 4u) : h;
        const u32 size = s_w * s_h * (image.info.num_bits / 8);
        const u64 hash = XXH3_64bits(addr, size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;
    const bool is_gpu_modified = True(image.flags & ImageFlagBits::GpuModified);
    const bool is_gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    u32 uploaded_mips = 0;
    u32 skipped_mips = 0;
    u64 uploaded_bytes = 0;
    u64 skipped_bytes = 0;
    for (u32 m = 0; m < num_mips; m++) {
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        // A copy that follows this refresh writes this level whole. What guest memory holds for
        // it would never be read: the copy overwrites it first. Only the levels the copy does
        // not reach are uploaded. See CopyCoverage, and ExpandImage, the only caller that can
        // pass a coverage.
        if (covered != nullptr && covered->CoversLevel(m, num_layers)) {
            ++skipped_mips;
            skipped_bytes += mip_size;
            continue;
        }

        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;

        // Protect GPU modified resources from accidental CPU reuploads.
        if (is_gpu_modified && !is_gpu_dirty) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
        ++uploaded_mips;
        uploaded_bytes += static_cast<u64>(extent_width) * extent_height * depth * num_layers *
                          (image.info.num_bits / 8);
    }
    ImageStats::NoteRefresh(uploaded_mips, uploaded_bytes, skipped_mips, skipped_bytes);

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return;
    }

    Vulkan::GpuProfile::Note(Vulkan::GpuProfile::HostWork::ImageUpload, image.info.guest_size);
    scheduler.EndRendering();

    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address, image.info.guest_size);
    if (auto barrier = in_buffer->GetBarrier(vk::AccessFlagBits2::eTransferRead,
                                             vk::PipelineStageFlagBits2::eTransfer)) {
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    const auto [buffer, offset] =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
    for (auto& copy : image_copies) {
        copy.bufferOffset += offset;
    }

    if (image.info.guest_size >= 0x700000) {
        // Close the Mode A blind spot: stamp buffer_cache (or stream) source as detile-read.
        // Host dispatch log often has srcResource=0; guest track is authoritative for FHD src.
        buffer_cache.NoteDetileSourceUse(in_buffer->Handle(), in_offset, image.info.guest_size,
                                         "detile_src");
        if (StagingVerbose()) {
            LOG_WARNING(Render_Vulkan,
                        "IMAGE_UPLOAD_CHAIN size={:#x} srcBuffer={:#x} srcOffset={:#x} "
                        "detileDst={:#x} detileOffset={:#x} tick={} w={} h={} tiled={}",
                        image.info.guest_size, u64(VkBuffer(in_buffer->Handle())), in_offset,
                        u64(VkBuffer(buffer)), offset, scheduler.CurrentTick(),
                        image.info.size.width, image.info.size.height,
                        image.info.props.is_tiled ? 1 : 0);
        }
    }

    image.Upload(image_copies, buffer, offset);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    const u64 hash = XXH3_64bits(&sampler, sizeof(sampler));
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base);
    return it->second.Handle();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (TraceImages()) {
        TraceImage("new", image.info, image_id.index);
    }
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    ++registrations;
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageRegistrations);
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    if (TraceImages()) {
        TraceImage("gone", image.info, image_id.index);
    }
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    ++registrations;
    Vulkan::FrameStats::Add(Vulkan::FrameStats::Counter::ImageRegistrations);
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    ForEachPage(image.info.guest_address, image.info.guest_size, [this, image_id](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
    });
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        // Re-track the whole image
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdatePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdatePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdatePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdatePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        // This image spans only 2 pages and both are modified,
        // but the image itself was not directly affected.
        // Cehck its hash later.
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdatePageWatchers<false>(addr, size);
}

void TextureCache::RunGarbageCollector() {
    SCOPE_EXIT {
        ++gc_tick;
    };
    if (instance.CanReportMemoryUsage()) {
        total_used_memory = instance.GetDeviceMemoryUsage();
    }
    if (total_used_memory < trigger_gc_memory) {
        return;
    }
    std::scoped_lock lock{mutex};
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;

    const auto configure = [&](bool allow_aggressive) {
        pressured = total_used_memory >= pressure_gc_memory;
        aggresive = allow_aggressive && total_used_memory >= critical_gc_memory;
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto& image = slot_images[image_id];
        if (False(image.flags & ImageFlagBits::Registered)) {
            // Let go of already (see TouchImage): never twice.
            LOG_ERROR(Render_Vulkan,
                      "The garbage collector met image {} again after it was let go",
                      image_id.index);
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
        if (download) {
            DownloadImageMemory(image_id);
        }
        FreeImage(image_id);
        if (total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    // Try to remove anything old enough and not high priority.
    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (total_used_memory >= critical_gc_memory) {
        // If we are still over the critical limit, run an aggressive GC
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
}

void TextureCache::TouchImage(const Image& image) {
    // Only an image in the cache has an entry of its own in the LRU list. One let go of
    // (FreeImage) is looked at once more by whoever still holds it, to see that it needs to be
    // found again (binding.needs_rebind), and its slot lives on until the GPU is done with it;
    // a null image never had an entry, its lru_id is entry 0, someone else's. Touched, a free
    // entry went back into the list, and the garbage collector later let go of its image a
    // second time ("Trying to unregister an already unregistered image", at the last boss), or
    // the entry, given to a new image meanwhile, sat in the list twice.
    if (False(image.flags & ImageFlagBits::Registered)) {
        static u32 seen = 0;
        if (seen++ < 3) {
            LOG_INFO(Render_Vulkan,
                     "Image at {:#x} ({}x{}) used while not in the cache (let go of, or a null "
                     "image): its LRU entry is left alone",
                     image.info.guest_address, image.info.size.width, image.info.size.height);
        }
        return;
    }
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

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
}

} // namespace VideoCore
