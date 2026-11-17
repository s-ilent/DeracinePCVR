// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include "common/types.h"

namespace Libraries::Camera {

/// The rate the virtual PlayStation Camera sensor delivers frames at, in microseconds.
constexpr u64 VirtualCameraPeriodUs(u32 hz) {
    return 1000000 / (hz == 0 ? 60 : hz);
}

/// Paces the guest's virtual camera sensor.
///
/// The sensor has its own clock. A read that asks for the next frame waits for that clock only,
/// never for the guest to present a frame: titles read the camera from a thread that can run
/// before their next present, so a sensor that waited for guest progress could hold up the very
/// present that was supposed to release it. Every read advances the sensor by at most one frame,
/// which is what keeps it from running ahead of the title that consumes its frames.
class VirtualCameraClock {
public:
    explicit VirtualCameraClock(u64 period_us)
        : period_us_(period_us == 0 ? VirtualCameraPeriodUs(60) : period_us) {}

    u64 period_us() const {
        return period_us_;
    }
    u64 frame_count() const {
        return frame_count_;
    }
    u64 frame_time_us() const {
        return frame_time_us_;
    }
    u64 next_frame_time_us() const {
        return next_frame_time_us_;
    }

    /// Starts a new sensor session. `period_us` = 0 keeps the current rate.
    void Reset(u64 period_us = 0) {
        if (period_us != 0) {
            period_us_ = period_us;
        }
        frame_count_ = 0;
        frame_time_us_ = 0;
        next_frame_time_us_ = 0;
    }

    /// Delivers the frame a read gets. `now_us` is the guest process time; `wait_next_frame` is the
    /// read mode that asks for the frame after the one it got last. `sleep_for_us` is called with
    /// the time still to wait before that frame is delivered (never with 0).
    template <typename SleepForUs>
    u64 Read(u64 now_us, bool wait_next_frame, SleepForUs&& sleep_for_us) {
        if (frame_count_ == 0) {
            frame_time_us_ = now_us;
            next_frame_time_us_ = now_us + period_us_;
            ++frame_count_;
            return frame_time_us_;
        }
        if (wait_next_frame) {
            if (next_frame_time_us_ > now_us) {
                sleep_for_us(next_frame_time_us_ - now_us);
                now_us = next_frame_time_us_;
            }
            frame_time_us_ = std::max(next_frame_time_us_, now_us);
            next_frame_time_us_ = frame_time_us_ + period_us_;
            ++frame_count_;
        } else if (now_us >= next_frame_time_us_) {
            frame_time_us_ = now_us;
            next_frame_time_us_ = now_us + period_us_;
            ++frame_count_;
        }
        return frame_time_us_;
    }

private:
    u64 period_us_;
    u64 frame_count_ = 0;
    u64 frame_time_us_ = 0;
    u64 next_frame_time_us_ = 0;
};

} // namespace Libraries::Camera
