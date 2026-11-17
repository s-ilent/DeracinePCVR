// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

/// Whether the texture cache's per-second counters are reported. One place, because both the
/// counters and the timers that feed them ask, and the answer has to be the same for both.
/// SHADPS4_DBG_OVERLAP=1 turns them on; the existing OVERLAPS line uses the same variable.
inline bool ImageStatsEnabled() {
    static const bool enabled = std::getenv("SHADPS4_DBG_OVERLAP") != nullptr;
    return enabled;
}

/// Adds the wall time of its scope to a counter, in nanoseconds. Cheap enough (two clock reads)
/// to sit around a call that is suspected of costing something, which is the only honest way to
/// answer "is this resource churn worth recycling".
///
/// Nothing is read and nothing is added while the counters are off: a diagnostic that costs the
/// normal path two clock reads per texture-cache call is a diagnostic that distorts what it
/// measures, and this one wraps `ResolveOverlap`, which runs hundreds of times a frame.
class ScopedTimer {
public:
    explicit ScopedTimer(std::atomic<u64>& counter)
        : target{ImageStatsEnabled() ? &counter : nullptr},
          start{target != nullptr ? std::chrono::steady_clock::now()
                                  : std::chrono::steady_clock::time_point{}} {}

    ~ScopedTimer() {
        if (target == nullptr) {
            return;
        }
        const auto done = std::chrono::steady_clock::now();
        target->fetch_add(static_cast<u64>(
                              std::chrono::duration_cast<std::chrono::nanoseconds>(done - start)
                                  .count()),
                          std::memory_order_relaxed);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::atomic<u64>* target;
    std::chrono::steady_clock::time_point start;
};

/// Per-second counts of the Vulkan images the texture cache creates and destroys, and of the
/// whole-image work it records for them.
///
/// These are the numbers an "is this resource churn worth recycling?" question needs, and the
/// ones a copy-versus-upload trade needs: how many images come and go, how many bytes they are,
/// how many whole-image copies were recorded and how much of the guest-memory upload the copies
/// made unnecessary. Function counters alone never answered that — they say a path ran, not how
/// much work a frame paid for.
///
///   SHADPS4_DBG_OVERLAP=1   one RENDERSTAT line a second, next to the OVERLAPS line
class ImageStats {
public:
    /// Whether the counters are being reported.
    static bool Enabled() {
        return ImageStatsEnabled();
    }

    /// A Vulkan image was created by `vkCreateImage` through VMA.
    static void NoteImageCreated(u64 bytes) {
        if (!Enabled()) {
            return;
        }
        images_created.fetch_add(1, std::memory_order_relaxed);
        image_bytes_created.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// A Vulkan image was destroyed through VMA. With the recycler on, this counts what the pool
    /// could not keep, which is what makes the hit rate worth reading next to it.
    static void NoteImageDestroyed() {
        if (!Enabled()) {
            return;
        }
        images_destroyed.fetch_add(1, std::memory_order_relaxed);
    }

    /// The image recycler handed back an image instead of the driver creating one.
    static void NoteRecyclerHit() {        if (!Enabled()) {
            return;
        }
        recycler_hits.fetch_add(1, std::memory_order_relaxed);
    }

    /// No image of that shape was in the recycler, so one has to be created.
    static void NoteRecyclerMiss() {
        if (!Enabled()) {
            return;
        }
        recycler_misses.fetch_add(1, std::memory_order_relaxed);
    }

    /// What the recycler is holding right now. A gauge, not a rate.
    static void SetRecyclerPool(u32 images, u64 bytes, u64 destroys) {
        if (!Enabled()) {
            return;
        }
        recycler_pool_images.store(images, std::memory_order_relaxed);
        recycler_pool_bytes.store(bytes, std::memory_order_relaxed);
        recycler_destroys.store(destroys, std::memory_order_relaxed);
    }

    /// A whole-image copy was recorded, and how many bytes of destination it writes.
    static void NoteCopy(u64 bytes) {
        if (!Enabled()) {
            return;
        }
        copies.fetch_add(1, std::memory_order_relaxed);
        copy_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }

    /// A whole-image copy that goes through a staging buffer instead of one `vkCmdCopyImage`.
    static void NoteBufferedCopy() {
        if (!Enabled()) {
            return;
        }
        buffered_copies.fetch_add(1, std::memory_order_relaxed);
    }

    /// A destination image was refreshed from guest memory: the levels that were uploaded and
    /// their bytes, and the levels a following copy made unnecessary.
    static void NoteRefresh(u32 levels_uploaded, u64 bytes_uploaded, u32 levels_skipped,
                            u64 bytes_skipped) {
        if (!Enabled()) {
            return;
        }
        refreshes.fetch_add(1, std::memory_order_relaxed);
        upload_mips.fetch_add(levels_uploaded, std::memory_order_relaxed);
        upload_bytes.fetch_add(bytes_uploaded, std::memory_order_relaxed);
        skipped_mips.fetch_add(levels_skipped, std::memory_order_relaxed);
        skipped_bytes.fetch_add(bytes_skipped, std::memory_order_relaxed);
    }

    /// An image was expanded, and whether a covering copy was found for it.
    static void NoteExpand(bool covered) {
        if (!Enabled()) {
            return;
        }
        expands.fetch_add(1, std::memory_order_relaxed);
        if (covered) {
            covered_expands.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /// The allocator the images come from, so the per-second line can say how much device memory
    /// the emulator's own allocations hold. Set once, at texture-cache construction.
    static void SetAllocator(VmaAllocator vma_allocator) {
        allocator = vma_allocator;
    }

    /// Writes and clears the second's totals.
    static void Report() {
        if (!Enabled()) {
            return;
        }
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        long long last = last_report.load();
        if (now - last < 1000 || !last_report.compare_exchange_strong(last, now)) {
            return;
        }
        constexpr double ToMiB = 1.0 / (1024.0 * 1024.0);
        constexpr double ToMs = 1.0 / 1e6;
        // How much device memory the emulator's own VMA allocations hold. Slow to compute (it
        // walks every allocation), which is why it is here once a second and not in the path.
        u64 vram_bytes = 0;
        u64 vram_blocks = 0;
        if (allocator != VK_NULL_HANDLE) {
            VmaTotalStatistics statistics{};
            vmaCalculateStatistics(allocator, &statistics);
            for (u32 heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
                vram_bytes += statistics.memoryHeap[heap].statistics.blockBytes;
                vram_blocks += statistics.memoryHeap[heap].statistics.blockCount;
            }
        }
        LOG_INFO(Render_Vulkan,
                 "RENDERSTAT expands={} covered_expands={} refreshes={} upload_mips={} "
                 "upload_mb={:.1f} skipped_mips={} skipped_mb={:.1f} images_created={} "
                 "images_destroyed={} image_mb={:.1f} recycler_hits={} recycler_misses={} "
                 "pool_images={} pool_mb={:.1f} recycler_destroys={} copies={} copy_mb={:.1f} "
                 "buffered_copies={} create_ms={:.3f} destroy_ms={:.3f} overlap_ms={:.3f} "
                 "depth_ms={:.3f} expand_ms={:.3f} refresh_ms={:.3f} vram_mb={:.1f} "
                 "vram_blocks={}",
                 expands.exchange(0), covered_expands.exchange(0), refreshes.exchange(0),
                 upload_mips.exchange(0),
                 static_cast<double>(upload_bytes.exchange(0)) * ToMiB, skipped_mips.exchange(0),
                 static_cast<double>(skipped_bytes.exchange(0)) * ToMiB,
                 images_created.exchange(0), images_destroyed.exchange(0),
                 static_cast<double>(image_bytes_created.exchange(0)) * ToMiB,
                 recycler_hits.exchange(0), recycler_misses.exchange(0),
                 recycler_pool_images.load(), static_cast<double>(recycler_pool_bytes.load()) * ToMiB,
                 recycler_destroys.load(), copies.exchange(0),
                 static_cast<double>(copy_bytes.exchange(0)) * ToMiB,
                 buffered_copies.exchange(0),
                 static_cast<double>(create_ns.exchange(0)) * ToMs,
                 static_cast<double>(destroy_ns.exchange(0)) * ToMs,
                 static_cast<double>(overlap_ns.exchange(0)) * ToMs,
                 static_cast<double>(depth_ns.exchange(0)) * ToMs,
                 static_cast<double>(expand_ns.exchange(0)) * ToMs,
                 static_cast<double>(refresh_ns.exchange(0)) * ToMs,
                 static_cast<double>(vram_bytes) * ToMiB, vram_blocks);
    }

    /// Where the texture cache's own time goes, in nanoseconds. Public because the timer needs a
    /// counter to add to; nothing else should touch them.
    static inline std::atomic<u64> create_ns{};
    static inline std::atomic<u64> destroy_ns{};
    static inline std::atomic<u64> overlap_ns{};
    static inline std::atomic<u64> depth_ns{};
    static inline std::atomic<u64> expand_ns{};
    static inline std::atomic<u64> refresh_ns{};

private:
    static inline VmaAllocator allocator{};
    static inline std::atomic<u64> images_created{};
    static inline std::atomic<u64> images_destroyed{};
    static inline std::atomic<u64> image_bytes_created{};
    static inline std::atomic<u64> recycler_hits{};
    static inline std::atomic<u64> recycler_misses{};
    static inline std::atomic<u32> recycler_pool_images{};
    static inline std::atomic<u64> recycler_pool_bytes{};
    static inline std::atomic<u64> recycler_destroys{};
    static inline std::atomic<u64> copies{};
    static inline std::atomic<u64> copy_bytes{};
    static inline std::atomic<u64> buffered_copies{};
    static inline std::atomic<u64> refreshes{};
    static inline std::atomic<u64> upload_mips{};
    static inline std::atomic<u64> upload_bytes{};
    static inline std::atomic<u64> skipped_mips{};
    static inline std::atomic<u64> skipped_bytes{};
    static inline std::atomic<u64> expands{};
    static inline std::atomic<u64> covered_expands{};
    static inline std::atomic<long long> last_report{};
};

} // namespace VideoCore
