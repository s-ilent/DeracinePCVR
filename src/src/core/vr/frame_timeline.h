// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/logging/log.h"
#include "common/types.h"

namespace Core::Vr {

/// SHADPS4_FRAME_TIMELINE=1 writes one line per frame event so the guest, the GPU thread and the
/// OpenXR host can be lined up on a monotonic clock: "TIMELINE <ns> <event> <detail>".
class FrameTimeline {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("SHADPS4_FRAME_TIMELINE");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }

    /// Counts render passes, for lining up what a frame consists of.
    static void CountRenderPass() {
        passes.fetch_add(1, std::memory_order_relaxed);
    }

    static u32 TakeRenderPasses() {
        return passes.exchange(0, std::memory_order_relaxed);
    }

    static inline std::atomic<u32> passes{};

    static u64 Now() {
        static const auto start = std::chrono::steady_clock::now();
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }

    static void Mark(const char* event, const char* detail = "") {
        if (!Enabled()) {
            return;
        }
        LOG_INFO(Core_Vr, "TIMELINE {} {} {}", Now(), event, detail);
    }

    static void MarkStr(const char* event, const char* detail, u32 first = 0, u32 second = 0) {
        if (!Enabled()) {
            return;
        }
        LOG_INFO(Core_Vr, "TIMELINE {} {} {} {} {}", Now(), event, detail, first, second);
    }

    static void Mark(const char* event, u32 value) {
        if (!Enabled()) {
            return;
        }
        LOG_INFO(Core_Vr, "TIMELINE {} {} {}", Now(), event, value);
    }
};

} // namespace Core::Vr
