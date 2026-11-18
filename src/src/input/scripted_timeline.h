// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

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

/// "frames=<n>": how many guest frames a frame-numbered line lasts (0: the line stays in effect
/// and the script does not wait longer for it; see ScriptLifetime).
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

/// When a whole script is over. A script is timed in two domains at once and lasts until both
/// are spent: every clock-numbered line has passed its start + hold, and every frame-numbered
/// line has reached its frame (and lasted its `frames=`, when it declares one). A frame-numbered
/// line without a count is active from its frame on, but the script does not wait beyond that
/// frame for it. This is what keeps a frame-only script from ending on the wall clock before its
/// frame is reached.
///
/// A title can stop polling its pads (a hang, a paused scene, a long load), which would leave
/// frame-numbered work pending forever. The lifetime therefore watches the guest frame counter
/// and reports a stall once it has not moved for the configured bound while frame-numbered work
/// is pending, so the replay can give up, release its inputs and log why.
class ScriptLifetime {
public:
    /// Seconds the guest frames may stand still, with frame-numbered work pending, before the
    /// script is given up on. Zero or less disables the watchdog.
    static constexpr double DefaultStallSeconds = 30.0;

    enum class State {
        Running,
        Finished, ///< Every line has run its course.
        Stalled,  ///< The guest frames stopped moving with frame-numbered work still pending.
    };

    ScriptLifetime() = default;
    explicit ScriptLifetime(const std::vector<StepTiming>& steps,
                            double stall_seconds = DefaultStallSeconds)
        : stall_seconds{stall_seconds} {
        for (const StepTiming& step : steps) {
            if (!step.ByFrame()) {
                clock_end = std::max(clock_end, step.end);
                continue;
            }
            any_frame = true;
            // `frames=` is a count, so the line runs out at its frame plus that many frames.
            // Saturate rather than wrap on an absurd script.
            const std::uint64_t frame = *step.frame;
            if (frame > std::numeric_limits<std::uint64_t>::max() - step.frames) {
                frame_end = std::numeric_limits<std::uint64_t>::max();
            } else {
                frame_end = std::max(frame_end, frame + step.frames);
            }
        }
    }

    /// Feeds the two clocks (seconds since the script began, and the title's own frame count) and
    /// returns what the script is doing now. Once finished or stalled it stays so.
    State Update(double now, std::uint64_t guest_frame) {
        if (state != State::Running) {
            return state;
        }
        if (!seen_frame) {
            seen_frame = true;
            last_frame = guest_frame;
            last_progress = now;
        } else if (guest_frame != last_frame) {
            last_frame = guest_frame;
            last_progress = now;
        }
        if (now >= clock_end && guest_frame >= frame_end) {
            state = State::Finished;
            return state;
        }
        if (stall_seconds > 0.0 && FrameWorkPending(guest_frame) &&
            now - last_progress > stall_seconds) {
            state = State::Stalled;
        }
        return state;
    }

    State Current() const {
        return state;
    }

    /// The last moment a clock-numbered line is in effect (0 when there is none).
    double ClockEnd() const {
        return clock_end;
    }

    /// The first guest frame every frame-numbered line has run its course by (0 when there is
    /// none).
    std::uint64_t FrameEnd() const {
        return frame_end;
    }

    /// Whether a frame-numbered line has not run its course yet at `guest_frame`.
    bool FrameWorkPending(std::uint64_t guest_frame) const {
        return any_frame && guest_frame < frame_end;
    }

    double StallSeconds() const {
        return stall_seconds;
    }

private:
    double clock_end{};
    std::uint64_t frame_end{};
    bool any_frame{};
    double stall_seconds{DefaultStallSeconds};
    State state{State::Running};
    bool seen_frame{};
    std::uint64_t last_frame{};
    double last_progress{};
};

} // namespace Input::Scripted
