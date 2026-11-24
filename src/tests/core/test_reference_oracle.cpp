// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the independent reference oracle.
//
// The comparator's other tests seed a reference from the replay's own output, which verifies the
// comparison but not the renderer: a renderer that produced the wrong image would seed the wrong
// reference and match itself. These tests compute the expected image on the CPU from the geometry,
// so a disagreement is evidence about the renderer rather than about the comparator.

#include <gtest/gtest.h>

#include "reference_oracle.h"

namespace {

using oracle::Fixture;
using oracle::Image;
using oracle::u32;
using oracle::Vertex;

/// A quad covering the whole viewport, in clip space, with a flat colour.
Fixture FullScreenQuad(float r, float g, float b) {
    Fixture fixture{};
    fixture.width = 8;
    fixture.height = 8;
    fixture.vertices = {
        {-1.0f, -1.0f, 0.0f, 1.0f, r, g, b, 1.0f},
        {1.0f, -1.0f, 0.0f, 1.0f, r, g, b, 1.0f},
        {1.0f, 1.0f, 0.0f, 1.0f, r, g, b, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f, r, g, b, 1.0f},
    };
    fixture.indices = {0, 1, 2, 2, 3, 0};
    return fixture;
}

TEST(ReferenceOracle, AFullScreenQuadCoversEveryPixel) {
    // The expected image is known by construction: every pixel is the quad's colour.
    Fixture fixture = FullScreenQuad(1.0f, 0.5f, 0.25f);
    Image image{};
    std::string error;
    ASSERT_TRUE(oracle::Rasterize(fixture, image, error)) << error;
    ASSERT_EQ(image.pixels.size(), 8u * 8u * 4u);
    for (size_t i = 0; i < 8u * 8u; ++i) {
        EXPECT_EQ(image.pixels[i * 4 + 0], 255);
        EXPECT_EQ(image.pixels[i * 4 + 1], 128);
        EXPECT_EQ(image.pixels[i * 4 + 2], 64);
        EXPECT_EQ(image.pixels[i * 4 + 3], 255);
    }
}

TEST(ReferenceOracle, AHalfScreenQuadCoversExactlyHalf) {
    // A quad covering the left half. The covered count is known without running any renderer.
    Fixture fixture{};
    fixture.width = 8;
    fixture.height = 8;
    fixture.vertices = {
        {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    fixture.indices = {0, 1, 2, 2, 3, 0};
    Image image{};
    std::string error;
    ASSERT_TRUE(oracle::Rasterize(fixture, image, error)) << error;

    u32 covered = 0;
    for (size_t i = 0; i < 8u * 8u; ++i) {
        if (image.pixels[i * 4 + 1] == 255) {
            ++covered;
        }
    }
    EXPECT_EQ(covered, 32u);
}

TEST(ReferenceOracle, TheBackgroundIsPreservedWhereNothingIsDrawn) {
    // A draw that covers part of the target leaves the rest as it was. This is the property a
    // cleared-target assumption would get wrong.
    Fixture fixture{};
    fixture.width = 4;
    fixture.height = 4;
    fixture.background[0] = 10;
    fixture.background[1] = 20;
    fixture.background[2] = 30;
    fixture.background[3] = 255;
    fixture.vertices = {
        {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    fixture.indices = {0, 1, 2};
    Image image{};
    std::string error;
    ASSERT_TRUE(oracle::Rasterize(fixture, image, error)) << error;

    // The right half keeps the background.
    const size_t right = (0 * 4 + 3) * 4;
    EXPECT_EQ(image.pixels[right + 0], 10);
    EXPECT_EQ(image.pixels[right + 1], 20);
    EXPECT_EQ(image.pixels[right + 2], 30);
}

TEST(ReferenceOracle, ADisabledColourWriteLeavesTheTargetUntouched) {
    Fixture fixture = FullScreenQuad(1.0f, 1.0f, 1.0f);
    fixture.background[0] = 7;
    fixture.background[1] = 8;
    fixture.background[2] = 9;
    fixture.color_write = false;
    Image image{};
    std::string error;
    ASSERT_TRUE(oracle::Rasterize(fixture, image, error)) << error;
    for (size_t i = 0; i < 8u * 8u; ++i) {
        EXPECT_EQ(image.pixels[i * 4 + 0], 7);
        EXPECT_EQ(image.pixels[i * 4 + 1], 8);
        EXPECT_EQ(image.pixels[i * 4 + 2], 9);
    }
}

TEST(ReferenceOracle, ADegenerateTriangleCoversNothing) {
    Fixture fixture{};
    fixture.width = 4;
    fixture.height = 4;
    fixture.vertices = {
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
    };
    fixture.indices = {0, 1, 2};
    Image image{};
    std::string error;
    ASSERT_TRUE(oracle::Rasterize(fixture, image, error)) << error;
    for (size_t i = 0; i < 4u * 4u; ++i) {
        EXPECT_EQ(image.pixels[i * 4 + 0], 0);
    }
}

TEST(ReferenceOracle, AnOutOfRangeIndexIsRefused) {
    Fixture fixture = FullScreenQuad(1.0f, 1.0f, 1.0f);
    fixture.indices[0] = 99;
    Image image{};
    std::string error;
    EXPECT_FALSE(oracle::Rasterize(fixture, image, error));
    EXPECT_NE(error.find("addresses a vertex"), std::string::npos);
}

TEST(ReferenceOracle, ANonMultipleOfThreeIndexCountIsRefused) {
    Fixture fixture = FullScreenQuad(1.0f, 1.0f, 1.0f);
    fixture.indices.pop_back();
    Image image{};
    std::string error;
    EXPECT_FALSE(oracle::Rasterize(fixture, image, error));
    EXPECT_NE(error.find("multiple of three"), std::string::npos);
}

TEST(ReferenceOracle, AZeroWVertexIsRefused) {
    Fixture fixture = FullScreenQuad(1.0f, 1.0f, 1.0f);
    fixture.vertices[0].w = 0.0f;
    Image image{};
    std::string error;
    EXPECT_FALSE(oracle::Rasterize(fixture, image, error));
    EXPECT_NE(error.find("w of zero"), std::string::npos);
}

} // namespace
