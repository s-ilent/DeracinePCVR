// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

namespace VideoCore {

/// Remembers guest allocations whose contents were deliberately not carried over when an image
/// was re-created for the other representation, so the renderer can be checked against itself:
/// if any of those images is later bound as an attachment that *loads* its previous contents
/// rather than clearing them, dropping the copy was wrong.
class PreserveTracker {
public:
    static constexpr u32 Capacity = 64;

    static void NoteSkipped(VAddr address) {
        const u32 slot = head.fetch_add(1, std::memory_order_relaxed) % Capacity;
        skipped[slot].store(address, std::memory_order_relaxed);
        ++skipped_total;
    }

    static bool WasSkipped(VAddr address) {
        if (address == 0) {
            return false;
        }
        for (u32 i = 0; i < Capacity; ++i) {
            if (skipped[i].load(std::memory_order_relaxed) == address) {
                return true;
            }
        }
        return false;
    }

    /// An attachment that was seen to clear its image after a skipped preservation.
    static inline std::atomic<u32> cleared_after_skip{};
    /// An attachment that was seen to *load* contents that were never preserved. Must stay 0.
    static inline std::atomic<u32> loaded_after_skip{};
    static inline std::atomic<u32> skipped_total{};

private:
    static inline std::atomic<VAddr> skipped[Capacity]{};
    static inline std::atomic<u32> head{};
};

/// Remembers allocations that were re-created as a 3D (volume) image from a 2D one, so the
/// next use of that memory can be classified: drawn into fresh, loaded with its previous
/// contents, or sampled as data.
class Alias3DTracker {
public:
    static constexpr u32 Capacity = 32;

    static void NoteExpanded(VAddr address) {
        const u32 slot = head.fetch_add(1, std::memory_order_relaxed) % Capacity;
        addresses[slot].store(address, std::memory_order_relaxed);
    }

    static bool WasExpanded(VAddr address) {
        if (address == 0) {
            return false;
        }
        for (u32 i = 0; i < Capacity; ++i) {
            if (addresses[i].load(std::memory_order_relaxed) == address) {
                return true;
            }
        }
        return false;
    }

    static inline std::atomic<u32> used_cleared{};
    static inline std::atomic<u32> used_loaded{};
    static inline std::atomic<u32> used_sampled{};

private:
    static inline std::atomic<VAddr> addresses[Capacity]{};
    static inline std::atomic<u32> head{};
};

} // namespace VideoCore
