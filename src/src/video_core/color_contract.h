// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

/// The transfer-function contract of the presenter's post-processing pass.
///
/// The guest hands the emulator pictures that are already encoded for a display: videoout
/// buffers declare an sRGB pixel format, and the PSVR eye pictures are the title's final,
/// display-referred ones (8-bit bytes, which the headset system shows as they are). The host
/// pass may move such a picture to a target, but it must not encode it a second time. The
/// pure decisions and the exact curve live here so the renderer and the tests share one
/// definition; post_process.frag implements the same curve.
namespace VideoCore::Color {

// The sRGB transfer function, IEC 61966-2-1: the encode the pass's gamma() is for a gamma
// setting of 1, and the decode its linear_out applies.
constexpr float SrgbCutoff = 0.0031308f;
constexpr float SrgbSlope = 12.92f;
constexpr float SrgbA = 1.055f;
constexpr float SrgbB = 0.055f;

[[nodiscard]] inline float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    return x <= SrgbCutoff ? SrgbSlope * x : SrgbA * std::pow(x, 1.0f / 2.4f) - SrgbB;
}

[[nodiscard]] inline float SrgbToLinear(const float encoded) {
    const float x = std::clamp(encoded, 0.0f, 1.0f);
    return x <= 0.04045f ? x / SrgbSlope : std::pow((x + SrgbB) / SrgbA, 2.4f);
}

/// The byte a UNORM8 attachment stores for a shader output of `value` (round to nearest).
[[nodiscard]] inline std::uint8_t Unorm8Byte(const float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/// The shader value a UNORM8 byte is read as.
[[nodiscard]] inline float Unorm8Value(const std::uint8_t value) {
    return static_cast<float>(value) / 255.0f;
}

/// How an 8-bit picture reaches the pass.
enum class ByteView : std::uint8_t {
    /// Not an 8-bit Color format, or no view of it returns the stored bytes (an sRGB view
    /// without an allowed UNORM sibling): the pass works with what the hardware decoded.
    Unavailable,
    /// The sampled view already returns the stored bytes (the guest asked for a byte format).
    StoredBytes,
    /// The guest asked for an sRGB view and the backing image allows its UNORM sibling: the
    /// pass reads the stored bytes through the sibling, so the hardware decodes nothing.
    SrgbViewWithSibling,
};

/// What the pass must do with the picture it was handed.
struct Presentation {
    /// Sample the stored bytes instead of letting the hardware decode an sRGB view.
    bool read_stored_bytes;
    /// The shader must not encode what it sampled (post_process.frag's `encoded_in`).
    bool encoded_in;
    /// Hand the target linear light: its sRGB attachment encodes on write (`linear_out`).
    bool linear_out;
};

/// The contract for one picture.
///
/// `gamma_one` is the pass's gamma setting at its neutral 1.0 (the only setting under which
/// the bytes are taken as they are), `hdr` the HDR path (which carries no SDR transfer
/// function), and `a_b_off` the A/B measuring aid asking for the old behaviour.
[[nodiscard]] constexpr Presentation Plan(const ByteView view, const bool target_srgb,
                                          const bool gamma_one, const bool hdr,
                                          const bool a_b_off) noexcept {
    // A picture the guest encoded for a display is read as the bytes it holds and the shader
    // does not encode it again. On a byte target those bytes are already what the display
    // shows; on an sRGB target the shader hands back linear light for the hardware to encode,
    // which stores the same bytes again. Where the bytes cannot be read (an sRGB view with no
    // sibling), decoding in hardware and encoding in the shader is the same round trip.
    const bool display_encoded = view != ByteView::Unavailable && gamma_one && !hdr && !a_b_off;
    return Presentation{
        .read_stored_bytes = display_encoded,
        .encoded_in = display_encoded,
        .linear_out = target_srgb && !hdr,
    };
}

/// The byte a target stores for a source picture whose stored byte is `stored`, under `plan`.
/// `hardware_decodes` is whether the sampled view decodes sRGB (an sRGB view) rather than
/// returning the bytes (a UNORM view). `target_srgb` is whether the attachment is sRGB.
[[nodiscard]] inline std::uint8_t PresentedByte(const std::uint8_t stored,
                                                const bool hardware_decodes,
                                                const Presentation& plan, const bool target_srgb) {
    const float sampled =
        hardware_decodes ? SrgbToLinear(Unorm8Value(stored)) : Unorm8Value(stored);
    float here = plan.encoded_in ? sampled : LinearToSrgb(sampled);
    if (plan.linear_out) {
        here = SrgbToLinear(here);
    }
    return Unorm8Byte(target_srgb ? LinearToSrgb(here) : here);
}

/// The byte the old pass left for a display-encoded picture it treated as linear: the
/// shader's gamma() encodes bytes that were already encoded, which is the defect this
/// contract removes.
[[nodiscard]] inline std::uint8_t DoubleEncodedByte(const std::uint8_t stored) {
    return Unorm8Byte(LinearToSrgb(Unorm8Value(stored)));
}

} // namespace VideoCore::Color
