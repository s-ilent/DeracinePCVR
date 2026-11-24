// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "reference_oracle.h"

#include <algorithm>
#include <cmath>

namespace oracle {

namespace {

/// A vertex after the perspective divide, in viewport coordinates.
struct ScreenVertex {
    float x{};
    float y{};
    float z{};
    float r{};
    float g{};
    float b{};
    float a{};
};

/// The signed area of the triangle, which decides winding and whether it is degenerate.
float Edge(const ScreenVertex& a, const ScreenVertex& b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

u8 Quantize(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<u8>(std::lround(clamped * 255.0f));
}

} // namespace

bool Rasterize(const Fixture& fixture, Image& out, std::string& error) {
    out = {};
    if (fixture.width == 0 || fixture.height == 0) {
        error = "the fixture has no extent";
        return false;
    }
    if (fixture.indices.size() % 3 != 0) {
        error = "the index count is not a multiple of three";
        return false;
    }
    for (const u32 index : fixture.indices) {
        if (index >= fixture.vertices.size()) {
            error = "an index addresses a vertex the fixture does not carry";
            return false;
        }
    }

    out.width = fixture.width;
    out.height = fixture.height;
    out.pixels.resize(static_cast<size_t>(fixture.width) * fixture.height * 4);
    for (u64 i = 0; i < static_cast<u64>(fixture.width) * fixture.height; ++i) {
        out.pixels[i * 4 + 0] = fixture.background[0];
        out.pixels[i * 4 + 1] = fixture.background[1];
        out.pixels[i * 4 + 2] = fixture.background[2];
        out.pixels[i * 4 + 3] = fixture.background[3];
    }
    if (!fixture.color_write) {
        // A draw with colour writes disabled leaves the target exactly as it was.
        return true;
    }

    // Clip space to viewport coordinates. Vulkan's viewport maps y downward, so the y flip is part
    // of the transform rather than a separate step.
    std::vector<ScreenVertex> screen(fixture.vertices.size());
    for (size_t i = 0; i < fixture.vertices.size(); ++i) {
        const Vertex& v = fixture.vertices[i];
        if (std::fabs(v.w) < 1e-6f) {
            error = "a vertex has a w of zero, which cannot be divided";
            return false;
        }
        const float ndc_x = v.x / v.w;
        const float ndc_y = v.y / v.w;
        const float ndc_z = v.z / v.w;
        screen[i].x = (ndc_x * 0.5f + 0.5f) * static_cast<float>(fixture.width);
        screen[i].y = (ndc_y * 0.5f + 0.5f) * static_cast<float>(fixture.height);
        screen[i].z = ndc_z;
        screen[i].r = v.r;
        screen[i].g = v.g;
        screen[i].b = v.b;
        screen[i].a = v.a;
    }

    for (size_t t = 0; t + 2 < fixture.indices.size(); t += 3) {
        const ScreenVertex& a = screen[fixture.indices[t + 0]];
        const ScreenVertex& b = screen[fixture.indices[t + 1]];
        const ScreenVertex& c = screen[fixture.indices[t + 2]];

        const float area = Edge(a, b, c.x, c.y);
        if (std::fabs(area) < 1e-9f) {
            // A degenerate triangle covers nothing.
            continue;
        }
        // The provoking vertex supplies the flat colour, which is what a flat-shaded pipeline does.
        const ScreenVertex& provoking = c;

        const float min_x = std::min({a.x, b.x, c.x});
        const float max_x = std::max({a.x, b.x, c.x});
        const float min_y = std::min({a.y, b.y, c.y});
        const float max_y = std::max({a.y, b.y, c.y});

        const int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
        const int x1 = std::min(static_cast<int>(fixture.width) - 1,
                                static_cast<int>(std::ceil(max_x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
        const int y1 = std::min(static_cast<int>(fixture.height) - 1,
                                static_cast<int>(std::ceil(max_y)));

        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                // Sample at the pixel centre.
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                const float w0 = Edge(b, c, px, py);
                const float w1 = Edge(c, a, px, py);
                const float w2 = Edge(a, b, px, py);
                const bool inside = area > 0.0f ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                                : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
                if (!inside) {
                    continue;
                }
                const size_t base = (static_cast<size_t>(y) * fixture.width + x) * 4;
                out.pixels[base + 0] = Quantize(provoking.r);
                out.pixels[base + 1] = Quantize(provoking.g);
                out.pixels[base + 2] = Quantize(provoking.b);
                out.pixels[base + 3] = Quantize(provoking.a);
            }
        }
    }

    error.clear();
    return true;
}

} // namespace oracle
