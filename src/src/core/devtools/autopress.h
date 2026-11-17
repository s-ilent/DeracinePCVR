// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>

#include "common/types.h"

namespace Core::Devtools {

/// Which guest input device the headless test input is injected into.
enum class AutoPressDevice : u32 {
    None = 0,
    Pad = 1 << 0,
    Move = 1 << 1,
    All = Pad | Move,
};

/// Reads SHADPS4_DBG_AUTOPRESS. Unset, "0" or anything else is off; "1" and "all" press both
/// devices; "pad" or "move" inject into one device only, which is how the minimum needed for a
/// headless scene is established. This env variable is a development harness switch: normal input
/// is never touched unless it is explicitly set to an enabled value.
constexpr AutoPressDevice ParseAutoPressDevices(const char* value) {
    if (value == nullptr) {
        return AutoPressDevice::None;
    }
    if (value[0] == '1' && value[1] == '\0') {
        return AutoPressDevice::All;
    }
    if (value[0] == 'p' && value[1] == 'a' && value[2] == 'd' && value[3] == '\0') {
        return AutoPressDevice::Pad;
    }
    if (value[0] == 'm' && value[1] == 'o' && value[2] == 'v' && value[3] == 'e' &&
        value[4] == '\0') {
        return AutoPressDevice::Move;
    }
    if (value[0] == 'a' && value[1] == 'l' && value[2] == 'l' && value[3] == '\0') {
        return AutoPressDevice::All;
    }
    return AutoPressDevice::None;
}

constexpr bool AutoPressDevicesInclude(AutoPressDevice devices, AutoPressDevice device) {
    return (static_cast<u32>(devices) & static_cast<u32>(device)) != 0;
}

/// When the headless test input presses: a hold every cycle, inside a bounded window after the
/// first read. The timing is part of the benchmark procedure, so it lives in one place.
struct AutoPressSchedule {
    u64 begin_ms = 8000;
    u64 end_ms = 45000;
    u64 cycle_ms = 3000;
    u64 hold_ms = 600;

    bool Pressing(u64 elapsed_ms) const {
        return elapsed_ms >= begin_ms && elapsed_ms < end_ms && (elapsed_ms % cycle_ms) < hold_ms;
    }
};

/// State of one device's test input. A device keeps its own instance, started on its first read,
/// so enabling the harness cannot perturb anything else in the input path.
class AutoPressInjector {
public:
    AutoPressInjector(AutoPressDevice devices, u64 start_ms)
        : devices_(devices), start_ms_(start_ms) {}

    bool enabled() const {
        return devices_ != AutoPressDevice::None;
    }
    bool includes(AutoPressDevice device) const {
        return AutoPressDevicesInclude(devices_, device);
    }
    bool pressing() const {
        return pressing_;
    }
    u64 elapsed_ms(u64 now_ms) const {
        return now_ms >= start_ms_ ? now_ms - start_ms_ : 0;
    }

    /// Advances the schedule to `now_ms`. Returns whether the device should press now; sets
    /// `began` on the first tick of a press so a caller can log one line per burst.
    bool Tick(u64 now_ms, bool* began = nullptr) {
        const bool pressing = enabled() && schedule_.Pressing(elapsed_ms(now_ms));
        if (began != nullptr) {
            *began = pressing && !pressing_;
        }
        pressing_ = pressing;
        return pressing;
    }

    static AutoPressDevice DevicesFromEnv() {
        return ParseAutoPressDevices(std::getenv("SHADPS4_DBG_AUTOPRESS"));
    }
    static u64 NowMs() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }

private:
    AutoPressDevice devices_;
    u64 start_ms_;
    AutoPressSchedule schedule_;
    bool pressing_ = false;
};

} // namespace Core::Devtools
