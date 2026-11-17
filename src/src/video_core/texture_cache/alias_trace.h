// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore {

/// One guest allocation can be looked at through more than one Vulkan image of different formats:
/// the texture cache keeps the representation the guest asked for last, and re-creates the image
/// (carrying its whole contents over) when the guest asks for the other one.
///
/// This trace follows a single allocation through both the texture cache and the renderer, so a run
/// shows what the guest actually does with the memory: which passes write the depth representation,
/// what the colour one is bound as, and whether it is written.
///
///   SHADPS4_TRACE_ADDR=0x2068000000   guest address to follow (hex); switches the trace on
///   SHADPS4_TRACE_SIZE=0x6000000      guest size of the allocation (hex, default 96 MiB)
///
/// Detail lines start with "ALIAS" and a sequence number, so the lines of a frame can be read in the
/// order they happened without relying on the logger's timestamps. A detail line is written the
/// first two times a distinct situation is seen, not once per draw: `FirstTime()`. Per-second totals
/// are written as "ALIASSTAT" lines by Report().
class AliasTrace {
public:
    static bool Enabled() {
        static const bool enabled = std::getenv("SHADPS4_TRACE_ADDR") != nullptr;
        return enabled;
    }

    static VAddr Start() {
        static const VAddr start = [] {
            const char* value = std::getenv("SHADPS4_TRACE_ADDR");
            return value != nullptr ? static_cast<VAddr>(std::strtoull(value, nullptr, 0)) : VAddr{};
        }();
        return start;
    }

    static u64 Span() {
        static const u64 span = [] {
            const char* value = std::getenv("SHADPS4_TRACE_SIZE");
            return value != nullptr ? static_cast<u64>(std::strtoull(value, nullptr, 0))
                                    : static_cast<u64>(96_MB);
        }();
        return span;
    }

    /// Whether a guest range of `size` bytes overlaps the traced allocation.
    static bool Hits(VAddr address, u64 size) {
        if (!Enabled() || address == 0) {
            return false;
        }
        const VAddr end = Start() + Span();
        const VAddr last = address + std::max<u64>(size, 1u);
        return address < end && last > Start();
    }

    static u32 NextSeq() {
        return seq.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// True the first two times a distinct situation (an opaque key of the caller's choosing) is
    /// asked about, false for the thousands of repeats after that.
    static bool FirstTime(u64 key) {
        static constexpr u32 Capacity = 256;
        static std::array<std::atomic<u64>, Capacity> keys{};
        static std::atomic<u32> next{};
        for (u32 i = 0; i < Capacity; ++i) {
            const u64 current = keys[i].load(std::memory_order_relaxed);
            if (current == key) {
                return count[i].fetch_add(1, std::memory_order_relaxed) < 2;
            }
            if (current == 0) {
                u64 expected = 0;
                if (keys[i].compare_exchange_strong(expected, key)) {
                    count[i].fetch_add(1, std::memory_order_relaxed);
                    next.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if (expected != key) {
                    continue;
                }
                count[i].fetch_add(1, std::memory_order_relaxed);
                return count[i].load() < 2;
            }
        }
        return next.load() < Capacity;
    }

    /// What the guest asked the allocation for, counted per second.
    static inline std::atomic<u32> requests{};
    static inline std::atomic<u32> binds_sampled{};
    static inline std::atomic<u32> binds_storage{};
    static inline std::atomic<u32> depth_targets{};
    static inline std::atomic<u32> depth_clears{};
    static inline std::atomic<u32> depth_loads{};
    static inline std::atomic<u32> color_targets{};
    static inline std::atomic<u32> color_clears{};
    static inline std::atomic<u32> depth_copies{};
    static inline std::atomic<u32> expands{};
    static inline std::atomic<u32> recreations{};
    static inline std::atomic<u32> recreations_preserved{};
    static inline std::atomic<u64> copied_bytes{};
    static inline std::atomic<u32> stencil_enabled_passes{};
    static inline std::atomic<u32> stencil_write_passes{};

    /// Writes and clears the per-second totals, then starts a new second.
    static void Report() {
        if (!Enabled()) {
            return;
        }
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        static std::atomic<long long> last_report{};
        long long last = last_report.load();
        if (now - last < 1000 || !last_report.compare_exchange_strong(last, now)) {
            return;
        }
        LOG_INFO(Render_Vulkan,
                 "ALIASSTAT requests={} sampled={} storage={} depth_targets={} depth_clears={} "
                 "depth_loads={} color_targets={} color_clears={} depth_copies={} expands={} "
                 "recreations={} preserved={} copy_mb={:.1f} stencil_passes={} stencil_writes={}",
                 requests.exchange(0), binds_sampled.exchange(0), binds_storage.exchange(0),
                 depth_targets.exchange(0), depth_clears.exchange(0), depth_loads.exchange(0),
                 color_targets.exchange(0), color_clears.exchange(0), depth_copies.exchange(0),
                 expands.exchange(0), recreations.exchange(0), recreations_preserved.exchange(0),
                 static_cast<double>(copied_bytes.exchange(0)) / (1024.0 * 1024.0),
                 stencil_enabled_passes.exchange(0), stencil_write_passes.exchange(0));
    }

private:
    static inline std::atomic<u32> seq{};
    static inline std::array<std::atomic<u32>, 256> count{};
};

/// Names the guest allocations that are re-created in another representation often, so a run says
/// *which* memory the title is swapping between depth and colour rather than only how often.
///
///   SHADPS4_DBG_REPR=1   one REPR line per second with the busiest allocations of that second
class ReprTrace {
public:
    static bool Enabled() {
        static const bool enabled = std::getenv("SHADPS4_DBG_REPR") != nullptr;
        return enabled;
    }

    static void Note(VAddr address, u32 from_fmt, u32 to_fmt, u32 binding, u64 bytes) {
        if (!Enabled() || address == 0) {
            return;
        }
        for (u32 i = 0; i < Capacity; ++i) {
            const VAddr current = address_slot[i].load(std::memory_order_relaxed);
            if (current == address) {
                count_slot[i].fetch_add(1, std::memory_order_relaxed);
                bytes_slot[i].fetch_add(bytes, std::memory_order_relaxed);
                Report();
                return;
            }
            if (current == 0) {
                VAddr expected = 0;
                if (address_slot[i].compare_exchange_strong(expected, address)) {
                    from_slot[i].store(from_fmt, std::memory_order_relaxed);
                    to_slot[i].store(to_fmt, std::memory_order_relaxed);
                    binding_slot[i].store(binding, std::memory_order_relaxed);
                    count_slot[i].fetch_add(1, std::memory_order_relaxed);
                    bytes_slot[i].fetch_add(bytes, std::memory_order_relaxed);
                    Report();
                    return;
                }
            }
        }
    }

    static void Report() {
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
        static std::atomic<long long> last_report{};
        long long last = last_report.load();
        if (now - last < 1000 || !last_report.compare_exchange_strong(last, now)) {
            return;
        }
        for (u32 i = 0; i < Capacity; ++i) {
            const u32 count = count_slot[i].exchange(0);
            const u64 bytes = bytes_slot[i].exchange(0);
            if (count == 0) {
                continue;
            }
            LOG_INFO(Render_Vulkan,
                     "REPR addr={:#x} count={} mb={:.1f} from={} to={} binding={}",
                     address_slot[i].load(), count,
                     static_cast<double>(bytes) / (1024.0 * 1024.0), from_slot[i].load(),
                     to_slot[i].load(), binding_slot[i].load());
        }
    }

private:
    static constexpr u32 Capacity = 24;
    static inline std::array<std::atomic<VAddr>, Capacity> address_slot{};
    static inline std::array<std::atomic<u32>, Capacity> from_slot{};
    static inline std::array<std::atomic<u32>, Capacity> to_slot{};
    static inline std::array<std::atomic<u32>, Capacity> binding_slot{};
    static inline std::array<std::atomic<u32>, Capacity> count_slot{};
    static inline std::array<std::atomic<u64>, Capacity> bytes_slot{};
};

} // namespace VideoCore