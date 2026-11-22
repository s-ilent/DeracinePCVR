// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The GNM shader-metadata parser is pure, so these tests exercise it without an emulator, a GPU or
// a guest memory manager. They pin the original decoder's own table arithmetic (Atlas A-027) and
// the fail-closed behaviour that keeps a malformed trailer from being reported as a finding.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "core/gnm_shader_metadata_parse.h"

namespace Core::GnmShaderMetadata {
namespace {

using Slots = std::vector<std::array<u8, 4>>;

/// Builds a span holding a declaration table followed by an OrbShdr trailer, laid out the way the
/// original decoder expects: the table ends where the trailer begins.
std::vector<u8> Build(std::span<const std::array<u8, 4>> slots, u8 chunk_base = 0, u8 version = 3,
                      u32 length = 0x190, u8 shader_type = 2, u8 flags = 0,
                      u64 hash = 0x0123456789abcdefull, u32 crc32 = 0xdeadbeef) {
    const size_t table_bytes = (static_cast<size_t>(chunk_base) + slots.size()) * 4;
    std::vector<u8> blob(table_bytes + TrailerSize, 0);

    for (size_t i = 0; i < slots.size(); ++i) {
        std::memcpy(blob.data() + i * 4, slots[i].data(), 4);
    }

    u8* trailer = blob.data() + table_bytes;
    std::memcpy(trailer, OrbShdrSignature.data(), OrbShdrSignature.size());
    trailer[OffVersion] = version;
    const u32 packed = (length << 8) | (static_cast<u32>(shader_type) << 2) | 1u;
    std::memcpy(trailer + OffPacked, &packed, sizeof(packed));
    trailer[OffChunkBase] = chunk_base;
    trailer[OffSlotCount] = static_cast<u8>(slots.size());
    trailer[OffFlags] = flags;
    std::memcpy(trailer + OffHash, &hash, sizeof(hash));
    std::memcpy(trailer + OffCrc32, &crc32, sizeof(crc32));
    return blob;
}

TEST(GnmShaderMetadata, ParsesTrailerFields) {
    const Slots slots = {{2, 0, 4, 0}};
    const auto blob = Build(slots, 0, 3, 0x190, 2, 5);
    const auto result = Parse(blob, blob.size() - TrailerSize);

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.version, 3);
    EXPECT_EQ(result.length, 0x190u);
    EXPECT_EQ(result.shader_type, 2);
    EXPECT_EQ(result.chunk_usage_base_offset_in_dw, 0);
    EXPECT_EQ(result.num_input_usage_slots, 1);
    EXPECT_EQ(result.flags, 5);
    EXPECT_TRUE(result.is_srt);
    EXPECT_TRUE(result.is_extended_usage_info);
    EXPECT_EQ(result.shader_hash, 0x0123456789abcdefull);
    EXPECT_EQ(result.crc32, 0xdeadbeefu);
}

TEST(GnmShaderMetadata, TableSitsBeforeTheTrailer) {
    const Slots slots = {{2, 0, 4, 0}, {0x15, 1, 8, 1}, {0x22, 2, 7, 0}};
    const auto blob = Build(slots, 2);
    const size_t trailer_offset = blob.size() - TrailerSize;
    const auto result = Parse(blob, trailer_offset);

    ASSERT_TRUE(result.valid) << result.error;
    // chunk base 2 plus 3 slots is 5 dwords, so the table starts 20 bytes before the trailer.
    EXPECT_EQ(result.table_bytes, 20u);
    EXPECT_EQ(result.table_offset, trailer_offset - 20);
    ASSERT_EQ(result.slot_count, 3);
    EXPECT_EQ(result.slots[0].usage_type, 2);
    EXPECT_EQ(result.slots[1].usage_type, 0x15);
    EXPECT_EQ(result.slots[2].usage_type, 0x22);
    EXPECT_EQ(result.slots[1].api_slot, 1);
    EXPECT_EQ(result.slots[1].start_register, 8);
    EXPECT_EQ(result.slots[1].flags, 1);
}

TEST(GnmShaderMetadata, RejectsMissingSignature) {
    auto blob = Build(Slots{{2, 0, 4, 0}});
    blob[blob.size() - TrailerSize] = 0;
    const auto result = Parse(blob, blob.size() - TrailerSize);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.error, "OrbShdr signature absent");
}

TEST(GnmShaderMetadata, RejectsTruncatedTrailer) {
    const auto blob = Build(Slots{{2, 0, 4, 0}});
    // One byte short of a whole trailer.
    const auto result = Parse(blob, blob.size() - TrailerSize + 1);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.error, "trailer outside the supplied span");
}

TEST(GnmShaderMetadata, RejectsTableOutsideTheSpan) {
    auto blob = Build(Slots{{2, 0, 4, 0}});
    // Claim far more declarations than the span holds. The parser must refuse rather than read
    // whatever happens to precede the span.
    blob[blob.size() - TrailerSize + OffSlotCount] = 200;
    const auto result = Parse(blob, blob.size() - TrailerSize);
    EXPECT_FALSE(result.valid);
    EXPECT_STREQ(result.error, "declaration table precedes the supplied span");
}

TEST(GnmShaderMetadata, ZeroSlotsIsValid) {
    const auto blob = Build(Slots{});
    const auto result = Parse(blob, blob.size() - TrailerSize);
    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.slot_count, 0);
    EXPECT_EQ(result.table_bytes, 0u);
}

TEST(GnmShaderMetadata, TruncatesBeyondTheRecordBound) {
    Slots slots;
    for (u32 i = 0; i < MaxSlots + 5; ++i) {
        slots.push_back({2, static_cast<u8>(i), 4, 0});
    }
    const auto blob = Build(slots);
    const auto result = Parse(blob, blob.size() - TrailerSize);
    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.slot_count, MaxSlots);
    EXPECT_EQ(result.truncated_slots, 5);
    EXPECT_EQ(result.num_input_usage_slots, MaxSlots + 5);
}

TEST(GnmShaderMetadata, NamesKnownUsageTypes) {
    EXPECT_STREQ(UsageTypeName(0x00), "IMM_RESOURCE");
    EXPECT_STREQ(UsageTypeName(0x02), "IMM_CONSTBUFFER");
    EXPECT_STREQ(UsageTypeName(0x03), "IMM_VERTEXBUFFER");
    EXPECT_STREQ(UsageTypeName(0x15), "PTR_SAMPLERTABLE");
    EXPECT_STREQ(UsageTypeName(0x22), "IMM_GDS_KICK_RING_OFFSET");
    EXPECT_STREQ(UsageTypeName(0x25), "IMM_DISPATCH_DRAW_INSTANCES");
}

TEST(GnmShaderMetadata, UnknownUsageTypeHasNoName) {
    // A type with no public name must stay unnamed rather than be given an invented one.
    EXPECT_EQ(UsageTypeName(0x0B), nullptr);
    EXPECT_EQ(UsageTypeName(0xFE), nullptr);
}

TEST(GnmShaderMetadata, DispatchDrawRange) {
    EXPECT_FALSE(IsDispatchDrawUsage(0x21));
    EXPECT_TRUE(IsDispatchDrawUsage(0x22));
    EXPECT_TRUE(IsDispatchDrawUsage(0x25));
    EXPECT_FALSE(IsDispatchDrawUsage(0x26));
}

TEST(GnmShaderMetadata, LengthIsNotTheTableOffset) {
    // The trailer's own length field and its position inside the program are different numbers.
    // A parser that conflated them would report a plausible but wrong address.
    const auto blob = Build(Slots{{2, 0, 4, 0}}, 0, 3, 0x400);
    const auto result = Parse(blob, blob.size() - TrailerSize);
    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(result.length, 0x400u);
    EXPECT_NE(static_cast<size_t>(result.length), blob.size() - TrailerSize);
}

TEST(GnmShaderMetadata, ParsesTwoShadersInOneSpan) {
    const auto first = Build(Slots{{2, 0, 4, 0}}, 0, 3, 0x100, 2, 0, 0x1111);
    const auto second = Build(Slots{{0x15, 1, 8, 1}}, 0, 3, 0x200, 1, 0, 0x2222);

    std::vector<u8> blob = first;
    blob.insert(blob.end(), second.begin(), second.end());

    const auto a = Parse(blob, first.size() - TrailerSize);
    const auto b = Parse(blob, blob.size() - TrailerSize);
    ASSERT_TRUE(a.valid) << a.error;
    ASSERT_TRUE(b.valid) << b.error;
    EXPECT_EQ(a.shader_hash, 0x1111u);
    EXPECT_EQ(b.shader_hash, 0x2222u);
    EXPECT_EQ(a.shader_type, 2);
    EXPECT_EQ(b.shader_type, 1);
}

} // namespace
} // namespace Core::GnmShaderMetadata
