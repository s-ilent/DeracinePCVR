// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Input {

/// How many times the guest has polled its controllers since the emulator started. This is the
/// closest thing a scripted test has to a guest frame: it moves when the title reads its pads, so
/// input can be timed against what the title itself has done rather than against the wall clock.
inline std::atomic<std::uint64_t> g_guest_frame{};

inline void NoteGuestFrame() {
    g_guest_frame.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t GuestFrame() {
    return g_guest_frame.load(std::memory_order_relaxed);
}

} // namespace Input
