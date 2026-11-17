// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <span>
#include <vector>

#include <boost/container/static_vector.hpp>

// vk_common.h first: it defines VK_ENABLE_BETA_EXTENSIONS before including vulkan.hpp, and
// vk_mem_alloc.h pulls vulkan_core.h in, so a vk_mem_alloc.h first would leave the beta enums
// vulkan.hpp expects undeclared.
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

/// Keeps the Vulkan images the texture cache lets go of, so the next image asked for with the same
/// creation parameters can be handed one back instead of asking the driver to create another.
///
/// Measured on Déraciné's classroom frame at 300 %: four Vulkan images are created and destroyed
/// every frame, 12.9 GB/s worth of allocation churn, and the create/destroy calls themselves take
/// about 3.3 ms of the GPU thread's frame — 0.8 ms creating, 2.5 ms destroying. Creating and
/// destroying the same shapes every frame is what the pool removes.
///
/// What a recycled image is *not*: an image with contents. Only the driver object and its memory
/// are reused; everything that says what is in it belongs to the new owner. Each `BackingImage`
/// starts at `vk::ImageLayout::eUndefined`, so the first barrier legally discards the old
/// contents, and `UniqueImage::Create` stamps the new create info and view-format list over the
/// recycled object.
///
/// Reuse rules, all of which exist to keep "same parameters" from meaning "compatible enough":
///  * the whole `vk::ImageCreateInfo` shape and the view-format list must match exactly,
///  * nothing is reused until whoever released it said the GPU was done with it (the texture cache
///    erases images from a deferred operation, which the scheduler only runs when the master
///    semaphore says that tick has completed),
///  * the pool is bounded in images and bytes, and an image that does not fit is destroyed.
class ImageRecycler {
public:
    /// One reusable image: the shape it was created with, and what to hand back on a hit.
    struct Entry {
        vk::ImageCreateInfo image_ci{};
        /// Empty when views may have any compatible format. Part of the key: an image created for
        /// a particular format list may not be viewed in another one.
        boost::container::static_vector<vk::Format, 4> view_formats{};
        vk::Image image{};
        VmaAllocation allocation{};
        /// The allocator the image came from. Kept per entry so a pool full of images from one
        /// device can still destroy them after the caller that released them is gone.
        VmaAllocator allocator{};
        u64 bytes{};
    };

    /// Whether `SHADPS4_DBG_IMAGE_RECYCLER` asks for it. Unset means on; `=0` turns it off so a
    /// measurement can put the driver's own create/destroy path back.
    static bool Enabled();

    /// The pool. A process-wide pool is enough: there is one Vulkan device.
    static ImageRecycler& Get();

    /// Takes an image with exactly this shape out of the pool. False when there is none.
    bool Acquire(const vk::ImageCreateInfo& image_ci, std::span<const vk::Format> view_formats,
                 Entry& out);

    /// Hands an image back. It replaces a pooled image of the same shape (the older one is
    /// destroyed and this one kept), joins the pool, or is destroyed when the pool is full.
    void Release(Entry&& entry);

    /// Destroys everything in the pool and refuses further releases. Must run before the VMA
    /// allocator it hands to `vmaDestroyImage` is destroyed.
    void Shutdown();

    /// Pool contents, for the per-second report.
    struct State {
        u32 images{};
        u64 bytes{};
        u64 hits{};
        u64 misses{};
        u64 released{};
        u64 destroyed{};
    };
    State Snapshot() const;

private:
    bool SameShape(const Entry& entry, const vk::ImageCreateInfo& image_ci,
                   std::span<const vk::Format> view_formats) const;
    void DestroyReleased(Entry& entry);

    /// Slots and bytes the pool may hold. Sized from the title's measured shapes: Déraciné keeps
    /// several 5760x3240 images (74.6 MB each) in flight between the colour and depth
    /// representations and the half-size copies, so a pool smaller than that only thrashes — 8
    /// slots measured a 44 % hit rate and kept evicting the expensive images.
    static constexpr u32 MaxImages = 16;
    static constexpr u64 MaxBytes = 768_MB;

    mutable std::mutex mutex;
    std::vector<Entry> entries;
    u64 pooled_bytes{};
    bool shutdown{};
    u64 hits{};
    u64 misses{};
    u64 released{};
    u64 destroyed{};
};

} // namespace Vulkan
