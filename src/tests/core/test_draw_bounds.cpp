// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the draw bounds check.
//
// The defect these exist to prevent: a mutation fixture that reads past the end of a buffer still
// executes, because robust buffer access returns zeros rather than faulting. The test would appear
// to pass while exercising an invalid draw, which proves nothing about the mutation.

#include <gtest/gtest.h>

#include "draw_bounds.h"

namespace {

using bounds::CheckDraw;
using bounds::DrawRange;

/// The observed fixture: 1101 indices whose maximum value is 806, and 807 vertices.
std::vector<bounds::u32> MakeIndices() {
    std::vector<bounds::u32> indices(1101);
    for (bounds::u32 i = 0; i < indices.size(); ++i) {
        indices[i] = i % 807;
    }
    return indices;
}

DrawRange MakeRange() {
    DrawRange range{};
    range.first_index = 0;
    range.index_count = 1101;
    range.base_vertex = 0;
    range.index_elements = 1101;
    range.vertex_count = 807;
    return range;
}

TEST(DrawBounds, TheBaselineDrawIsInBounds) {
    const auto indices = MakeIndices();
    const auto check = CheckDraw(MakeRange(), indices);
    EXPECT_TRUE(check.valid) << check.reason;
    EXPECT_EQ(check.max_index, 806u);
    EXPECT_EQ(check.min_vertex, 0);
    EXPECT_EQ(check.max_vertex, 806);
}

TEST(DrawBounds, AStartIndexPastTheEndIsRefused) {
    // 168 + 1101 = 1269, but the buffer holds 1101 elements. This is the exact fixture defect the
    // phase asked to verify.
    auto range = MakeRange();
    range.first_index = 168;
    const auto indices = MakeIndices();
    const auto check = CheckDraw(range, indices);
    EXPECT_FALSE(check.valid);
    EXPECT_NE(check.reason.find("1269"), std::string::npos);
}

TEST(DrawBounds, AStartIndexThatFitsIsAccepted) {
    // 168 + 600 = 768, which is inside the buffer.
    auto range = MakeRange();
    range.first_index = 168;
    range.index_count = 600;
    const auto indices = MakeIndices();
    const auto check = CheckDraw(range, indices);
    EXPECT_TRUE(check.valid) << check.reason;
}

TEST(DrawBounds, ANegativeBaseVertexThatAddressesBeforeTheStreamIsRefused) {
    // The baseline window starts at index 0, so a base vertex of -7 addresses vertex -7.
    auto range = MakeRange();
    range.base_vertex = -7;
    const auto indices = MakeIndices();
    const auto check = CheckDraw(range, indices);
    EXPECT_FALSE(check.valid);
    EXPECT_NE(check.reason.find("before the start"), std::string::npos);
}

TEST(DrawBounds, ANegativeBaseVertexIsAcceptedWhenEveryIndexAllowsIt) {
    // A window whose minimum index is 7 can take a base vertex of -7 and still address vertex 0.
    auto range = MakeRange();
    range.first_index = 0;
    range.index_count = 600;
    range.base_vertex = -7;
    std::vector<bounds::u32> indices(1101);
    for (bounds::u32 i = 0; i < indices.size(); ++i) {
        indices[i] = 7 + (i % 800);
    }
    const auto check = CheckDraw(range, indices);
    EXPECT_TRUE(check.valid) << check.reason;
    EXPECT_EQ(check.min_vertex, 0);
    EXPECT_EQ(check.max_index, 606u);
}

TEST(DrawBounds, AnIndexPastTheVertexStreamIsRefused) {
    // The index buffer can be large and still contain a value past the end of the vertex stream.
    auto range = MakeRange();
    range.index_count = 10;
    std::vector<bounds::u32> indices(1101, 0);
    indices[5] = 900; // only 807 vertices exist
    const auto check = CheckDraw(range, indices);
    EXPECT_FALSE(check.valid);
    EXPECT_NE(check.reason.find("900"), std::string::npos);
}

TEST(DrawBounds, AZeroIndexCountIsRefused) {
    auto range = MakeRange();
    range.index_count = 0;
    const auto indices = MakeIndices();
    const auto check = CheckDraw(range, indices);
    EXPECT_FALSE(check.valid);
}

TEST(DrawBounds, TheCheckUsesTheFetchedValuesNotTheAllocationSize) {
    // A buffer large enough for the range is not enough on its own: the values it contains decide
    // which vertices are addressed.
    auto range = MakeRange();
    range.index_count = 4;
    std::vector<bounds::u32> indices(1101, 0);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    indices[3] = 806;
    EXPECT_TRUE(CheckDraw(range, indices).valid);

    indices[3] = 807;
    EXPECT_FALSE(CheckDraw(range, indices).valid);
}

} // namespace
