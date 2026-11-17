// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>

namespace Core::Vr {

/// The projection a title was given for one eye, as tangents: how far out towards the temple
/// the picture reaches, how far in towards the nose, and up and down from straight ahead.
struct GuestProjection {
    float tan_out{1.0f};
    float tan_in{1.0f};
    float tan_top{0.75f};
    float tan_bottom{0.75f};
};

/// A point looked at in an eye's picture: 0..1 across it and down it.
struct GuestGazePoint {
    float u{0.5f};
    float v{0.5f};
    bool valid{false};
};

/// Where a direction, given in the frame the title drew with (its eyes looking down -Z), falls
/// in one eye's picture.
///
/// The two eyes look the same way in a title's VR rendering; what differs is the projection:
/// the left eye's picture reaches further towards the left temple (its "out") and less towards
/// the nose, and the right eye's the other way round. So the same direction lands at different
/// places in each eye's picture, which is exactly what the shading-rate map has to follow.
inline GuestGazePoint MapGazeToEye(float dx, float dy, float dz, int eye,
                                   const GuestProjection& projection) {
    GuestGazePoint point{};
    const float forward = -dz;
    if (!(forward > 1e-4f) || !std::isfinite(dx) || !std::isfinite(dy)) {
        return point;
    }
    const float tan_x = dx / forward;
    const float tan_y = dy / forward;
    const float out = eye == 0 ? projection.tan_out : projection.tan_in;
    const float in = eye == 0 ? projection.tan_in : projection.tan_out;
    const float across = out + in;
    const float down = projection.tan_top + projection.tan_bottom;
    if (!(across > 1e-4f) || !(down > 1e-4f)) {
        return point;
    }
    // Across: "out" is on the left of the left eye and the right of the right eye.
    const float u = (tan_x + out) / across;
    const float v = (projection.tan_top - tan_y) / down;
    point.u = std::clamp(u, 0.0f, 1.0f);
    point.v = std::clamp(v, 0.0f, 1.0f);
    point.valid = true;
    return point;
}

} // namespace Core::Vr
