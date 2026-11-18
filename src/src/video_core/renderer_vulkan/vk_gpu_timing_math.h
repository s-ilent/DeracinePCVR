// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Vulkan {

/// The arithmetic behind the submission profiler's numbers, kept away from the Vulkan calls that
/// feed it so it can be tested without a device.
///
/// Why it is worth its own header: every way this can be wrong still produces a plausible number.
/// A gap counted where there was none makes the GPU look idle; an interval dropped because a pair
/// read back reversed quietly loses busy time; scaling a raw counter without the device's period
/// yields milliseconds that look like anything. None of that shows up as a failure at runtime, so
/// the rules are pinned down here and tested directly.
///
/// Both values are GPU timestamps, which are raw counters: the tick a device counts in is not a
/// nanosecond (it comes from VkPhysicalDeviceLimits::timestampPeriod), and only the low bits mean
/// anything (timestampValidBits), so a pair is scaled and masked before it is used.
class GpuTimelineMath {
public:
    /// What one retired submission contributed. `counted` is false when the pair was unusable and
    /// nothing at all was measured from it - not a zero-length interval, which is counted.
    struct Measured {
        u64 busy_ns{};
        u64 gap_ns{};
        bool counted{};
    };

    /// Measures one submission from its raw pair. The gap is from the end of the previous
    /// measurement to this submission's start, and only when there was a previous one and this
    /// submission really did start after it: submissions of this scheduler are in order, but a
    /// counter that wrapped or a pair read at the wrong moment must not turn into a gap of
    /// centuries, and a start before the previous end is no gap at all.
    Measured Add(u64 start_raw, u64 end_raw, double tick_ns, u64 valid_mask) noexcept {
        const u64 start = start_raw & valid_mask;
        const u64 end = end_raw & valid_mask;
        if (end < start) {
            // The device is not giving timestamps back in a usable state. Counting the interval
            // as zero would drag every average down; it is left out and reported as reversed.
            return {};
        }
        Measured measured;
        measured.busy_ns = Scale(end - start, tick_ns);
        if (have_previous_ && start > last_end_) {
            measured.gap_ns = Scale(start - last_end_, tick_ns);
        }
        last_end_ = end;
        have_previous_ = true;
        measured.counted = true;
        return measured;
    }

private:
    /// Ticks to nanoseconds. The period is a double, so this cannot be integer arithmetic.
    static u64 Scale(u64 ticks, double tick_ns) noexcept {
        return static_cast<u64>(static_cast<double>(ticks) * tick_ns);
    }

    u64 last_end_{};
    /// A zero timestamp is a real reading, so the first submission needs its own flag rather than
    /// last_end_ == 0 standing in for "nothing before it".
    bool have_previous_{};
};

} // namespace Vulkan
