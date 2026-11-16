// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "common/backing_write.h"
#include "common/types.h"

namespace {

std::vector<u8> Pattern(std::size_t size, u8 seed) {
    std::vector<u8> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>((seed + i * 37 + i / 7) & 0xFF);
    }
    return bytes;
}

} // Anonymous namespace

TEST(BackingWriteSegments, CopiesIntoOneSegment) {
    const auto source = Pattern(64, 3);
    std::vector<u8> backing(64, 0);

    const std::array<Common::BackingWriteSegment, 1> segments{{{backing.data(), backing.size()}}};
    const auto copied = Common::WriteBackingSegments(source.data(), source.size(), segments);

    EXPECT_EQ(copied, source.size());
    EXPECT_EQ(backing, source);
}

// A guest write that spans two backing allocations must keep the source offsets of the second
// allocation; each byte of the destination has to match its own offset in the source.
TEST(BackingWriteSegments, AdvancesSourceAcrossDissimilarSegments) {
    const auto source = Pattern(4096, 11);
    std::vector<u8> first(1000, 0xAA);
    std::vector<u8> second(3096, 0xBB);

    const std::array<Common::BackingWriteSegment, 2> segments{
        {{first.data(), first.size()}, {second.data(), second.size()}}};
    const auto copied = Common::WriteBackingSegments(source.data(), source.size(), segments);

    ASSERT_EQ(copied, source.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        ASSERT_EQ(first[i], source[i]) << "first segment byte " << i;
    }
    for (std::size_t i = 0; i < second.size(); ++i) {
        ASSERT_EQ(second[i], source[first.size() + i]) << "second segment byte " << i;
    }
}

TEST(BackingWriteSegments, StopsAfterTheBytesGiven) {
    const auto source = Pattern(500, 29);
    std::vector<u8> first(400, 0xAA);
    std::vector<u8> second(400, 0xBB);

    const std::array<Common::BackingWriteSegment, 2> segments{
        {{first.data(), first.size()}, {second.data(), second.size()}}};
    const auto copied = Common::WriteBackingSegments(source.data(), source.size(), segments);

    ASSERT_EQ(copied, source.size());
    EXPECT_TRUE(std::equal(first.begin(), first.end(), source.begin()));
    EXPECT_EQ(second[0], source[400]);
    for (std::size_t i = 100; i < second.size(); ++i) {
        ASSERT_EQ(second[i], 0xBB) << "untouched tail byte " << i;
    }
}

TEST(BackingWriteSegments, SkipsZeroSizedSegments) {
    const auto source = Pattern(32, 7);
    std::vector<u8> first(32, 0);

    const std::array<Common::BackingWriteSegment, 2> segments{
        {{first.data(), 0}, {first.data(), first.size()}}};
    const auto copied = Common::WriteBackingSegments(source.data(), source.size(), segments);

    EXPECT_EQ(copied, source.size());
    EXPECT_EQ(first, source);
}

TEST(BackingWriteSegments, CopiesNothingForAZeroByteWrite) {
    std::vector<u8> backing(16, 0xAA);

    const std::array<Common::BackingWriteSegment, 1> segments{{{backing.data(), backing.size()}}};
    const u8 dummy = 0;
    const auto copied = Common::WriteBackingSegments(&dummy, 0, segments);

    EXPECT_EQ(copied, 0u);
    for (const auto byte : backing) {
        ASSERT_EQ(byte, 0xAA);
    }
}

TEST(BackingWriteSegments, ReportsBytesBeyondSegmentCapacity) {
    const auto source = Pattern(32, 5);
    std::vector<u8> backing(8, 0);

    const std::array<Common::BackingWriteSegment, 1> segments{{{backing.data(), backing.size()}}};
    const auto copied = Common::WriteBackingSegments(source.data(), source.size(), segments);

    EXPECT_EQ(copied, 8u);
    EXPECT_TRUE(std::equal(backing.begin(), backing.end(), source.begin()));
}
