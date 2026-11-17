// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

namespace Core::Vr {

/// Where the eyes are looking, in each eye's own picture, for eye-tracked foveated rendering.
///
/// The host writes it once a frame from the runtime's gaze pose (XR_EXT_eye_gaze_interaction),
/// projected into each eye's own view; the renderer reads it when it draws the shading-rate map
/// that says how coarsely to shade each part of an eye's scene. One instance, because the eyes
/// are the headset's, not a session's.
struct GazeState {
    /// The point looked at in each eye's picture: 0..1 across and down it.
    std::atomic<float> u[2]{0.5f, 0.5f};
    std::atomic<float> v[2]{0.5f, 0.5f};
    /// Whether the runtime gave a gaze this frame that was aimed and fresh enough to trust.
    std::atomic<bool> valid{false};
    /// A counter that changes when the gaze has moved enough to be worth a new map.
    std::atomic<u32> revision{0};

    static GazeState& Instance() {
        static GazeState state;
        return state;
    }

    /// Writes one eye's point, and counts a new revision when either eye moves appreciably.
    void SetEye(int eye, float eye_u, float eye_v) {
        const float old_u = u[eye].load(std::memory_order_relaxed);
        const float old_v = v[eye].load(std::memory_order_relaxed);
        u[eye].store(eye_u, std::memory_order_relaxed);
        v[eye].store(eye_v, std::memory_order_relaxed);
        // A map is a few hundred kilobytes to rewrite: only do it for a real move.
        if (std::abs(eye_u - old_u) > 0.01f || std::abs(eye_v - old_v) > 0.01f) {
            revision.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void SetValid(bool is_valid) {
        if (valid.exchange(is_valid, std::memory_order_relaxed) != is_valid) {
            revision.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

} // namespace Core::Vr
