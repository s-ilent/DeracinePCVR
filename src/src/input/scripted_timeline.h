// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace Input::Scripted {

/// When one line of an input script takes effect. By default a line starts and ends at the times
/// its first two fields give, counted in seconds from when the script started. A line may instead
/// say which guest frame it belongs to ("frame=<n>", with "frames=<m>" for how many frames it
/// lasts), which is what makes an unattended run follow the title's frames rather than the wall
/// clock. The two kinds of line mix freely; a line with a frame number uses only that.
struct StepTiming {
    double start{};
    double end{};
    std::optional<std::uint64_t> frame;
    std::uint32_t frames{};
    /// Whether the line carries a frame number (and so ignores the clock).
    bool ByFrame() const {
        return frame.has_value();
    }
};

/// "frame=<n>": the guest frame the line starts on. False for anything else.
inline bool ParseFrameToken(std::string_view token, std::optional<std::uint64_t>& frame) {
    constexpr std::string_view Prefix = "frame=";
    if (!token.starts_with(Prefix)) {
        return false;
    }
    std::uint64_t value{};
    const std::string_view digits = token.substr(Prefix.size());
    if (digits.empty()) {
        return false;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    frame = value;
    return true;
}

/// "frames=<n>": how many guest frames a frame-numbered line lasts (0: until the run ends).
inline bool ParseFramesToken(std::string_view token, std::uint32_t& frames) {
    constexpr std::string_view Prefix = "frames=";
    if (!token.starts_with(Prefix)) {
        return false;
    }
    std::uint64_t value{};
    const std::string_view digits = token.substr(Prefix.size());
    if (digits.empty()) {
        return false;
    }
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > 0xffffffffull) {
            return false;
        }
    }
    frames = static_cast<std::uint32_t>(value);
    return true;
}

/// Whether the line has begun. Poses and one-off actions persist once begun, so they ask this.
inline bool Begun(const StepTiming& step, double now, std::uint64_t guest_frame) {
    if (step.frame) {
        return guest_frame >= *step.frame;
    }
    return now >= step.start;
}

/// Whether `a` begins after `b`, in whichever domain each uses. A frame-numbered line counts as
/// later than a clock-numbered one, so it wins when both kinds describe the same thing.
inline bool Later(const StepTiming& a, const StepTiming& b) {
    if (a.ByFrame() != b.ByFrame()) {
        return a.ByFrame();
    }
    if (a.ByFrame()) {
        return *a.frame >= *b.frame;
    }
    return a.start >= b.start;
}

/// Whether the line is in effect right now: begun, and either before its clock end or within its
/// frame count. A frame-numbered line without a count lasts until the run ends.
inline bool Active(const StepTiming& step, double now, std::uint64_t guest_frame) {
    if (step.frame) {
        const std::uint64_t elapsed = guest_frame - *step.frame;
        return guest_frame >= *step.frame && (step.frames == 0 || elapsed < step.frames);
    }
    return now >= step.start && now < step.end;
}

} // namespace Input::Scripted
