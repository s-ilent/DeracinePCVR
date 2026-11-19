// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_image_reuse_pool.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "common/logging/log.h"
#include "video_core/texture_cache/image_stats.h"

namespace Vulkan {
namespace {

u64 ReadLimitMb(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end{};
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return std::min<u64>(parsed, std::numeric_limits<u64>::max() / (1024 * 1024));
}

u32 ReadLimitEntries() {
    const char* value = std::getenv("SHADPS4_DBG_IMAGE_REUSE_MAX_ENTRIES");
    if (!value || !*value) {
        return 6;
    }
    char* end{};
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0') {
        return 6;
    }
    return static_cast<u32>(std::min<unsigned long>(parsed, std::numeric_limits<u32>::max()));
}

u64 MinBytes() {
    static const u64 bytes = ReadLimitMb("SHADPS4_DBG_IMAGE_REUSE_MIN_MB", 16) * 1024 * 1024;
    return bytes;
}

u64 MaxBytes() {
    static const u64 bytes = ReadLimitMb("SHADPS4_DBG_IMAGE_REUSE_MAX_MB", 256) * 1024 * 1024;
    return bytes;
}

u32 MaxEntries() {
    static const u32 entries = ReadLimitEntries();
    return entries;
}

} // namespace

bool ImageReusePool::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DBG_IMAGE_REUSE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

ImageReusePool& ImageReusePool::Get() {
    // Function-local lifetime avoids namespace-scope initialization-order dependencies. The
    // destructor intentionally does not call Vulkan: Instance::~Instance shuts this pool down
    // before destroying its VMA allocator.
    static ImageReusePool pool;
    return pool;
}

ImageReusePool::ImageReusePool() {
    LOG_INFO(Render_Vulkan,
             "Vulkan image reuse {} (min={} MiB, max_entries={}, max={} MiB)",
             Enabled() ? "enabled" : "disabled", MinBytes() / (1024 * 1024), MaxEntries(),
             MaxBytes() / (1024 * 1024));
}

bool ImageReusePool::Poolable(const vk::ImageCreateInfo& image_ci) {
    return image_ci.pNext == nullptr &&
           !(image_ci.flags & vk::ImageCreateFlagBits::eAlias) &&
           (image_ci.sharingMode == vk::SharingMode::eExclusive ||
            image_ci.sharingMode == vk::SharingMode::eConcurrent) &&
           (image_ci.queueFamilyIndexCount == 0 || image_ci.pQueueFamilyIndices != nullptr);
}

bool ImageReusePool::Acquire(const vk::ImageCreateInfo& image_ci,
                             std::span<const vk::Format> view_formats, VmaAllocator allocator,
                             Entry& out) {
    if (!Enabled() || !Poolable(image_ci)) {
        return false;
    }
    std::scoped_lock lock{mutex};
    if (shutdown) {
        return false;
    }
    auto it = std::ranges::find_if(entries, [&](const Entry& entry) {
        return entry.allocator == allocator &&
               SameCreateShape(entry.shape.image_ci, entry.shape.view_formats, image_ci,
                               view_formats);
    });
    if (it == entries.end()) {
        VideoCore::ImageStats::NoteReuseMiss();
        return false;
    }
    out = std::move(*it);
    pooled_bytes -= out.bytes;
    entries.erase(it);
    VideoCore::ImageStats::SetReusePool(pooled_bytes);
    VideoCore::ImageStats::NoteReuseHit();
    return true;
}

bool ImageReusePool::Release(Entry&& entry) {
    if (!entry.image || !Enabled() || !Poolable(entry.shape.image_ci) ||
        !EligibleSize(entry.bytes, MinBytes())) {
        return false;
    }
    std::scoped_lock lock{mutex};
    if (shutdown || !FitsBounds(static_cast<u32>(entries.size()), pooled_bytes, entry.bytes,
                                MaxEntries(), MaxBytes())) {
        return false;
    }
    entries.push_back(std::move(entry));
    if (!entries.back().shape.queue_family_indices.empty()) {
        entries.back().shape.image_ci.pQueueFamilyIndices =
            entries.back().shape.queue_family_indices.data();
    }
    pooled_bytes += entries.back().bytes;
    VideoCore::ImageStats::SetReusePool(pooled_bytes);
    return true;
}

void ImageReusePool::Shutdown() {
    std::scoped_lock lock{mutex};
    shutdown = true;
    for (Entry& entry : entries) {
        if (entry.image && entry.allocator) {
            vmaDestroyImage(entry.allocator, static_cast<VkImage>(entry.image), entry.allocation);
            VideoCore::ImageStats::NoteImageDestroyed();
            VideoCore::ImageStats::NoteReuseDestroyed();
        }
    }
    entries.clear();
    pooled_bytes = 0;
    VideoCore::ImageStats::SetReusePool(0);
}

u64 ImageReusePool::PooledBytes() const {
    std::scoped_lock lock{mutex};
    return pooled_bytes;
}

} // namespace Vulkan
