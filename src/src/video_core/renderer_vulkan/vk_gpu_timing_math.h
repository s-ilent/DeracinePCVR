// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>

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

/// Attribute one submission's operation intervals to the marks that opened them.
///
/// Why this is separate from the class above and from the Vulkan calls: the interesting failure
/// here is a plausible-looking misattribution. An interval there are n + 2 stamps - the
/// submission's top of pipe, one per mark, and its bottom - so:
///
///   top -> first mark      the part of the submission before anything marked: unattributed
///   mark i -> the stamp after it
///                          charged to mark i: the next mark, or the submission's own bottom
///
/// The head is reported on its own and is never added to an operation. A barrier or a copy that was
/// recorded between two operations lands inside the interval of the operation before it - that is
/// what "time to the next mark" means, and it is why copies and barriers are marked in their own
/// right wherever the emulator knows it recorded one.
///
/// A pair that came back reversed is counted as unusable and measures nothing, rather than being
/// added as a huge interval or silently dropped.
struct OpAttribution {
    u64 head_ns{};              ///< Top of pipe to the first mark: no operation owns it.
    std::vector<u64> mark_ns;   ///< One per mark, in the order the marks were made.
    u64 unusable{};             ///< Intervals whose stamps came back reversed.
};

/// `stamps` holds the submission's top of pipe, then one stamp per mark, then its bottom of pipe.
[[nodiscard]] inline OpAttribution AttributeIntervals(std::span<const u64> stamps, double tick_ns,
                                                      u64 valid_mask) {
    OpAttribution attribution;
    // n marks means n + 2 stamps; anything shorter cannot be attributed at all.
    if (stamps.size() < 2) {
        return attribution;
    }
    const size_t marks = stamps.size() - 2;
    attribution.mark_ns.assign(marks, 0);
    const auto interval = [&](size_t from, size_t to) -> std::optional<u64> {
        const u64 start = stamps[from] & valid_mask;
        const u64 end = stamps[to] & valid_mask;
        if (end < start) {
            ++attribution.unusable;
            return std::nullopt;
        }
        return static_cast<u64>(static_cast<double>(end - start) * tick_ns);
    };
    // With no marks this is the whole submission, which is then unowned: the busy figure already
    // covers it, so it must not be charged to an operation as well.
    if (const auto head = interval(0, 1)) {
        attribution.head_ns = *head;
    }
    for (size_t i = 0; i < marks; ++i) {
        // Stamp i + 1 opened mark i; the stamp after it closes the interval.
        if (const auto span = interval(i + 1, i + 2)) {
            attribution.mark_ns[i] = *span;
        }
    }
    return attribution;
}

} // namespace Vulkan
