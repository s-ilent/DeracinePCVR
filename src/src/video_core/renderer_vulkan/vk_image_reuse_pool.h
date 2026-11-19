// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

/// Reuses only idle VMA-backed images with an identical Vulkan creation shape.
class ImageReusePool {
public:
    struct Shape {
        vk::ImageCreateInfo image_ci{};
        std::vector<u32> queue_family_indices;
        std::vector<vk::Format> view_formats;
    };

    /// Contains allocation ownership only; image contents and layout state remain with UniqueImage.
    struct Entry {
        Shape shape;
        vk::Image image{};
        VmaAllocation allocation{};
        VmaAllocator allocator{};
        u64 bytes{};
    };

    /// The single opt-in switch. Unset or zero means the existing immediate create/destroy path.
    static bool Enabled();
    static ImageReusePool& Get();

    /// Pure allocation-shape checks, also usable by tests that do not create a Vulkan device.
    static bool SameCreateShape(const vk::ImageCreateInfo& a, std::span<const vk::Format> a_views,
                                const vk::ImageCreateInfo& b,
                                std::span<const vk::Format> b_views) {
        if (a.pNext || b.pNext || (a.flags & vk::ImageCreateFlagBits::eAlias) ||
            (b.flags & vk::ImageCreateFlagBits::eAlias) ||
            (a.sharingMode != vk::SharingMode::eExclusive &&
             a.sharingMode != vk::SharingMode::eConcurrent) ||
            (b.sharingMode != vk::SharingMode::eExclusive &&
             b.sharingMode != vk::SharingMode::eConcurrent) ||
            a.flags != b.flags || a.imageType != b.imageType || a.format != b.format ||
            a.extent.width != b.extent.width || a.extent.height != b.extent.height ||
            a.extent.depth != b.extent.depth || a.mipLevels != b.mipLevels ||
            a.arrayLayers != b.arrayLayers || a.samples != b.samples || a.tiling != b.tiling ||
            a.usage != b.usage || a.sharingMode != b.sharingMode ||
            a.initialLayout != b.initialLayout ||
            a.queueFamilyIndexCount != b.queueFamilyIndexCount ||
            (a.queueFamilyIndexCount != 0 &&
             (!a.pQueueFamilyIndices || !b.pQueueFamilyIndices))) {
            return false;
        }
        for (u32 i = 0; i < a.queueFamilyIndexCount; ++i) {
            if (a.pQueueFamilyIndices[i] != b.pQueueFamilyIndices[i]) {
                return false;
            }
        }
        if (a_views.size() != b_views.size()) {
            return false;
        }
        std::vector<vk::Format> sorted_a{a_views.begin(), a_views.end()};
        std::vector<vk::Format> sorted_b{b_views.begin(), b_views.end()};
        std::ranges::sort(sorted_a);
        std::ranges::sort(sorted_b);
        return sorted_a == sorted_b;
    }
    static bool EligibleSize(u64 allocation_bytes, u64 minimum_bytes) {
        return allocation_bytes >= minimum_bytes;
    }
    static bool FitsBounds(u32 entries, u64 pooled_bytes, u64 incoming_bytes, u32 max_entries,
                           u64 max_bytes) {
        return entries < max_entries && pooled_bytes <= max_bytes &&
               incoming_bytes <= max_bytes - pooled_bytes;
    }

    bool Acquire(const vk::ImageCreateInfo& image_ci, std::span<const vk::Format> view_formats,
                 VmaAllocator allocator, Entry& out);
    /// On true, ownership moved into the pool. On false, caller retains ownership and destroys it.
    bool Release(Entry&& entry);
    /// Destroys pooled images before the owning VMA allocator is destroyed; sticky thereafter.
    void Shutdown();

    u64 PooledBytes() const;

private:
    ImageReusePool();
    static bool Poolable(const vk::ImageCreateInfo& image_ci);
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    u64 pooled_bytes{};
    bool shutdown{};
};

} // namespace Vulkan
