// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_image_recycler.h"
#include "video_core/texture_cache/image_stats.h"

namespace Vulkan {

bool ImageRecycler::Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DBG_IMAGE_RECYCLER");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

ImageRecycler& ImageRecycler::Get() {
    static ImageRecycler recycler;
    return recycler;
}

bool ImageRecycler::SameShape(const Entry& entry, const vk::ImageCreateInfo& image_ci,
                              std::span<const vk::Format> view_formats) const {
    if (entry.view_formats.size() != view_formats.size() ||
        !std::ranges::equal(entry.view_formats, view_formats)) {
        return false;
    }
    const vk::ImageCreateInfo& have = entry.image_ci;
    // pNext is excluded on purpose: the only pNext an image is created with is a format list
    // built on the stack for the create call, and the list itself is compared above.
    if (have.flags != image_ci.flags || have.imageType != image_ci.imageType ||
        have.format != image_ci.format || have.extent != image_ci.extent ||
        have.mipLevels != image_ci.mipLevels || have.arrayLayers != image_ci.arrayLayers ||
        have.samples != image_ci.samples || have.tiling != image_ci.tiling ||
        have.usage != image_ci.usage || have.sharingMode != image_ci.sharingMode ||
        have.initialLayout != image_ci.initialLayout) {
        return false;
    }
    if (have.queueFamilyIndexCount != image_ci.queueFamilyIndexCount) {
        return false;
    }
    if (have.queueFamilyIndexCount != 0) {
        for (u32 i = 0; i < have.queueFamilyIndexCount; ++i) {
            if (have.pQueueFamilyIndices[i] != image_ci.pQueueFamilyIndices[i]) {
                return false;
            }
        }
    }
    return true;
}

bool ImageRecycler::Acquire(const vk::ImageCreateInfo& image_ci,
                            std::span<const vk::Format> view_formats, Entry& out) {
    if (!Enabled()) {
        return false;
    }
    std::scoped_lock lock{mutex};
    const auto it =
        std::ranges::find_if(entries, [&](const Entry& entry) {
            return SameShape(entry, image_ci, view_formats);
        });
    if (it == entries.end()) {
        ++misses;
        return false;
    }
    out = std::move(*it);
    pooled_bytes -= out.bytes;
    entries.erase(it);
    // The new owner stamps its own create info over the recycled one; what the image holds is
    // nobody's business but the new owner's, and it starts at eUndefined.
    out.image_ci = image_ci;
    out.view_formats.assign(view_formats.begin(), view_formats.end());
    ++hits;
    return true;
}

void ImageRecycler::Release(Entry&& entry) {
    if (!entry.image) {
        return;
    }
    // The allocator is needed to destroy what the pool does not keep. UniqueImage always fills it
    // in, and the entry carries its allocation, so the destructor of the *caller* is not needed.
    std::scoped_lock lock{mutex};
    if (!Enabled() || shutdown) {
        ++destroyed;
        DestroyReleased(entry);
        return;
    }
    ++released;
    // An image of the same shape already pooled cannot be reused twice; the one that has been
    // waiting longer goes.
    const auto it = std::ranges::find_if(entries, [&](const Entry& pooled) {
        return SameShape(pooled, entry.image_ci, entry.view_formats);
    });
    if (it != entries.end()) {
        pooled_bytes -= it->bytes;
        ++destroyed;
        vmaDestroyImage(it->allocator, it->image, it->allocation);
        *it = std::move(entry);
        pooled_bytes += it->bytes;
        return;
    }
    // A queue-family split image's create info points at caller memory, so its shape cannot be
    // compared later without risking a dangling read. Those are not pooled at all.
    if (entry.image_ci.queueFamilyIndexCount != 0) {
        ++destroyed;
        DestroyReleased(entry);
        return;
    }
    if (entries.size() >= MaxImages || pooled_bytes + entry.bytes > MaxBytes) {
        // The pool is full of something. Cheap images are not worth a slot when an expensive one
        // is on offer: Déraciné's frame releases a 74.6 MB depth image and several 4 KB ones, and
        // a first-in-first-out pool kept the 4 KB ones and threw the depth image away (measured:
        // 8 pooled images totalling 16.9 MB, zero hits).
        const auto smallest = std::ranges::min_element(
            entries, [](const Entry& a, const Entry& b) { return a.bytes < b.bytes; });
        if (smallest != entries.end() && pooled_bytes - smallest->bytes + entry.bytes <= MaxBytes &&
            entry.bytes > smallest->bytes) {
            pooled_bytes -= smallest->bytes;
            ++destroyed;
            vmaDestroyImage(smallest->allocator, smallest->image, smallest->allocation);
            *smallest = std::move(entry);
            pooled_bytes += smallest->bytes;
            return;
        }
        ++destroyed;
        DestroyReleased(entry);
        return;
    }
    pooled_bytes += entry.bytes;
    entries.push_back(std::move(entry));
}

void ImageRecycler::DestroyReleased(Entry& entry) {
    if (entry.image && entry.allocator) {
        VideoCore::ImageStats::NoteImageDestroyed();
        vmaDestroyImage(entry.allocator, entry.image, entry.allocation);
        entry.image = vk::Image{};
        entry.allocation = {};
    }
}

void ImageRecycler::Shutdown() {
    std::scoped_lock lock{mutex};
    shutdown = true;
    for (Entry& entry : entries) {
        ++destroyed;
        DestroyReleased(entry);
    }
    entries.clear();
    pooled_bytes = 0;
}

ImageRecycler::State ImageRecycler::Snapshot() const {
    std::scoped_lock lock{mutex};
    return State{.images = static_cast<u32>(entries.size()),
                 .bytes = pooled_bytes,
                 .hits = hits,
                 .misses = misses,
                 .released = released,
                 .destroyed = destroyed};
}

} // namespace Vulkan
