// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// How big the picture handed to the headset is, kept apart from how big the title draws it.
//
// The two are independent: the title decides its own eye render size (Unreal's screen percentage,
// a PlayStation VR's 960x1080 times whatever it asks for), while an OpenXR runtime asks an eye to
// be a size of its own (Steam Frame: 3036x3036, square). Resampling between them is a change of
// sampling density only - the projected field of view is what it is either way - so this file
// decides pixels and nothing else, which is why it can be tested without a GPU.
//
// No dependency beyond the standard library: the tests link it directly and the renderer includes
// it for the policy alone.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Core::Vr {

/// Who decides the size of one eye's picture sent to the headset.
enum class PresentationSizeMode {
    /// The title's own eye size, resampled only if a supersampling factor asks for it.
    Guest,
    /// What the runtime recommends an eye to be, from xrEnumerateViewConfigurationViews.
    Recommended,
    /// A width and height somebody chose; a height of 0 keeps the title's aspect.
    Explicit,
};

struct PresentationSizeRequest {
    PresentationSizeMode mode{PresentationSizeMode::Recommended};
    std::uint32_t explicit_width{};
    std::uint32_t explicit_height{};
    /// Multiplies the chosen size on both axes: 1.0 sends what was chosen, 1.25 sends a quarter
    /// more pixels each way.
    float supersample{1.0f};
};

/// What the runtime will not go past. Zero means "it did not say".
struct PresentationSizeLimits {
    /// XrViewConfigurationView::maxImageRectWidth/Height, one eye.
    std::uint32_t max_eye_width{};
    std::uint32_t max_eye_height{};
    /// XrSystemGraphicsProperties::maxSwapchainImageWidth/Height, the whole swapchain image. Both
    /// eyes travel side by side in one image, so half of the width is one eye's share.
    std::uint32_t max_image_width{};
    std::uint32_t max_image_height{};
};

struct PresentationSize {
    std::uint32_t eye_width{};
    std::uint32_t eye_height{};
    /// The mode that was actually used: a recommendation nobody made falls back to the title's own
    /// size, and says so here.
    PresentationSizeMode mode{PresentationSizeMode::Guest};
    /// The runtime's limits cut the request down.
    bool clamped{};
};

/// One eye's presentation size for this frame.
///
/// The recommendation is used as the runtime states it, without rounding up to a multiple of 16:
/// 3036 is what Steam Frame asks for, and 3040 would be asking for more than it recommended. A
/// size the title chose, or one somebody typed, is rounded up to 16 as the presenter always did,
/// because those numbers are arbitrary rather than what a device wants.
///
/// Supersampling multiplies the chosen size; it never changes which mode was chosen, so a
/// recommendation at 1.25x is still the recommendation, scaled.
inline PresentationSize PlanPresentationSize(std::uint32_t guest_width, std::uint32_t guest_height,
                                             std::uint32_t recommended_width,
                                             std::uint32_t recommended_height,
                                             const PresentationSizeLimits& limits,
                                             const PresentationSizeRequest& request) {
    constexpr std::uint32_t Align = 16;
    constexpr std::uint32_t Floor = 16;
    const auto round_up = [](std::uint32_t value) { return (value + Align - 1) / Align * Align; };

    PresentationSize size{};
    if (guest_width == 0 || guest_height == 0) {
        return size;
    }

    std::uint32_t width = guest_width;
    std::uint32_t height = guest_height;
    bool align = true;
    size.mode = request.mode;

    if (request.mode == PresentationSizeMode::Recommended && recommended_width > 0 &&
        recommended_height > 0) {
        width = recommended_width;
        height = recommended_height;
        align = false;
    } else if (request.mode == PresentationSizeMode::Explicit && request.explicit_width > 0) {
        width = request.explicit_width;
        height = request.explicit_height;
        if (height == 0) {
            height = static_cast<std::uint32_t>(
                std::llround(static_cast<double>(width) * guest_height / guest_width));
        }
    } else {
        // The title's own size: what the headset showed before any of this existed, and the only
        // answer when the runtime has no recommendation to offer.
        size.mode = PresentationSizeMode::Guest;
    }

    const float supersample = std::clamp(request.supersample, 0.1f, 8.0f);
    if (supersample != 1.0f) {
        width = static_cast<std::uint32_t>(std::llround(width * supersample));
        height = static_cast<std::uint32_t>(std::llround(height * supersample));
    }
    if (align) {
        width = round_up(width);
        height = round_up(height);
    }
    width = std::max(width, Floor);
    height = std::max(height, Floor);

    const auto cut = [&size](std::uint32_t value, std::uint32_t limit, std::uint32_t& out) {
        if (limit > 0 && value > limit) {
            out = limit;
            size.clamped = true;
        } else {
            out = value;
        }
    };
    std::uint32_t eye_width = width;
    std::uint32_t eye_height = height;
    cut(width, limits.max_eye_width, eye_width);
    cut(height, limits.max_eye_height, eye_height);
    cut(eye_width, limits.max_image_width > 1 ? limits.max_image_width / 2 : 0u, eye_width);
    cut(eye_height, limits.max_image_height, eye_height);
    size.eye_width = std::max(eye_width, Floor);
    size.eye_height = std::max(eye_height, Floor);
    return size;
}

} // namespace Core::Vr
