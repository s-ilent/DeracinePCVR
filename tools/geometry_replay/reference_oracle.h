// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// An independent reference oracle.
//
// The comparator's own tests seed a reference from the replay's output, which verifies the
// comparison implementation but is not evidence that the renderer is correct: a renderer that
// produced the wrong image would seed the wrong reference and match itself.
//
// This module computes the expected image from the geometry and the shader's declared behaviour,
// on the CPU, without touching Vulkan. It is deliberately restricted to a fixture simple enough to
// audit by hand: one triangle or quad, a flat colour per vertex, no texturing, no blending.
//
// It is not a general rasterizer and does not try to be. A fixture it cannot describe is refused
// rather than approximated.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oracle {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// A vertex in clip space with a flat colour.
struct Vertex {
    /// Clip-space position. The oracle divides by w, so a w of 1 is the ordinary case.
    float x{};
    float y{};
    float z{};
    float w{1.0f};
    /// The colour this vertex contributes, in 0..1 per channel.
    float r{};
    float g{};
    float b{};
    float a{1.0f};
};

/// What to rasterize.
struct Fixture {
    u32 width{};
    u32 height{};
    /// The vertices, in the order the index buffer addresses them.
    std::vector<Vertex> vertices;
    /// Triangle-list indices.
    std::vector<u32> indices;
    /// The colour the target holds before the draw. The oracle starts from this rather than from a
    /// clear, because a draw that loads its target inherits it.
    u8 background[4]{0, 0, 0, 255};
    /// Whether the draw writes colour at all. A disabled write leaves the background untouched.
    bool color_write{true};
};

/// The expected image, tightly packed RGBA.
struct Image {
    u32 width{};
    u32 height{};
    std::vector<u8> pixels;
};

/// Rasterizes the fixture on the CPU.
///
/// The rules are the ones a GPU applies for this restricted case: clip-space to normalised device
/// coordinates, a top-left fill rule, and flat colour taken from the provoking vertex. Anything the
/// fixture asks for that this cannot describe is refused with a reason rather than approximated.
[[nodiscard]] bool Rasterize(const Fixture& fixture, Image& out, std::string& error);

} // namespace oracle
