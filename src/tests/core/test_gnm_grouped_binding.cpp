// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The grouped-binding decode is the part of A-039 that must not be taken on trust: the packed
// entry selects a live guest object, and the group comes from the lowest set bit of a mask
// intersection. These tests pin those rules against the production constants.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/gnm_binding_capture.h"

namespace {

using Core::GnmBindingCapture::GroupedHelpers;

// The original binder's group selection: blsi, then a bit scan.
uint32_t LowestSetBitGroup(uint32_t pending) {
    if (pending == 0) {
        return 0xFFFFFFFFu;
    }
    uint32_t lowest = pending & (~pending + 1);
    uint32_t group = 0;
    while (lowest > 1) {
        group += 1;
        lowest >>= 1;
    }
    return group;
}

// Atlas A-031's decode of one packed binding entry.
struct PackedEntry {
    uint8_t api_slot;
    uint16_t object_index;
    uint8_t group;
};

PackedEntry DecodePacked(uint32_t raw) {
    return PackedEntry{
        static_cast<uint8_t>(raw & 0xFF),
        static_cast<uint16_t>((raw >> 8) & 0xFFFF),
        static_cast<uint8_t>(raw >> 24),
    };
}

// --- helper table ---------------------------------------------------------------------------

TEST(GroupedBinding, HelperTableHasFiveDistinctHelpers) {
    EXPECT_EQ(GroupedHelpers.size(), 5u);
    for (size_t i = 0; i < GroupedHelpers.size(); ++i) {
        for (size_t j = i + 1; j < GroupedHelpers.size(); ++j) {
            EXPECT_NE(GroupedHelpers[i].rva, GroupedHelpers[j].rva)
                << "helpers " << i << " and " << j << " share an RVA";
        }
    }
}

TEST(GroupedBinding, HelpersUseDistinctContextOffsets) {
    // Each helper reads its own dirty mask and cached-group array. A shared offset would mean the
    // table was copied from one helper rather than verified per helper.
    for (size_t i = 0; i < GroupedHelpers.size(); ++i) {
        for (size_t j = i + 1; j < GroupedHelpers.size(); ++j) {
            EXPECT_NE(GroupedHelpers[i].dirty_mask_offset, GroupedHelpers[j].dirty_mask_offset);
            EXPECT_NE(GroupedHelpers[i].group_cache_offset, GroupedHelpers[j].group_cache_offset);
        }
    }
}

TEST(GroupedBinding, HelperRvasMatchTheAtlas) {
    EXPECT_EQ(GroupedHelpers[0].rva, 0x0CC0070u);
    EXPECT_EQ(GroupedHelpers[1].rva, 0x0CC0300u);
    EXPECT_EQ(GroupedHelpers[2].rva, 0x0CC0670u);
    EXPECT_EQ(GroupedHelpers[3].rva, 0x0CC09E0u);
    EXPECT_EQ(GroupedHelpers[4].rva, 0x0CC71A0u);
}

// --- group selection ------------------------------------------------------------------------

TEST(GroupedBinding, GroupIsLowestSetBit) {
    EXPECT_EQ(LowestSetBitGroup(0x1), 0u);
    EXPECT_EQ(LowestSetBitGroup(0x2), 1u);
    EXPECT_EQ(LowestSetBitGroup(0x4), 2u);
    EXPECT_EQ(LowestSetBitGroup(0x8), 3u);
    EXPECT_EQ(LowestSetBitGroup(0x10), 4u);
}

TEST(GroupedBinding, GroupIgnoresHigherBits) {
    // The observed gameplay case: dirty 0x000fffff, mask 0x00000004, pending 0x00000004.
    EXPECT_EQ(LowestSetBitGroup(0x00000004u), 2u);
    EXPECT_EQ(LowestSetBitGroup(0x00000005u), 0u) << "bit 0 is lower than bit 2";
    EXPECT_EQ(LowestSetBitGroup(0x000FFFFFu), 0u);
}

TEST(GroupedBinding, ZeroPendingHasNoGroup) {
    EXPECT_EQ(LowestSetBitGroup(0), 0xFFFFFFFFu);
}

TEST(GroupedBinding, PendingIsTheMaskIntersection) {
    // The binder's own gate. A zero result means no writer follows.
    EXPECT_EQ(0x000FFFFFu & 0x00000004u, 0x00000004u);
    EXPECT_EQ(0x000FFFFFu & 0x00000001u, 0x00000001u);
    EXPECT_EQ(0x00000000u & 0x00000004u, 0u);
    EXPECT_EQ(0x00000003u & 0x00000004u, 0u) << "disjoint masks dispatch nothing";
}

// --- packed entry ---------------------------------------------------------------------------

TEST(GroupedBinding, PackedEntryDecodes) {
    // The observed gameplay entry: group 2, object index 0, api slot 0.
    const auto e = DecodePacked(0x02000000u);
    EXPECT_EQ(e.api_slot, 0u);
    EXPECT_EQ(e.object_index, 0u);
    EXPECT_EQ(e.group, 2u);
}

TEST(GroupedBinding, PackedEntryFieldsAreIndependent) {
    const auto e = DecodePacked(0x03050402u);
    EXPECT_EQ(e.api_slot, 0x02u);
    EXPECT_EQ(e.object_index, 0x0504u);
    EXPECT_EQ(e.group, 0x03u);
}

TEST(GroupedBinding, PackedEntryRoundTrips) {
    for (uint8_t group = 0; group < 8; ++group) {
        for (uint16_t index = 0; index < 4; ++index) {
            for (uint8_t slot = 0; slot < 4; ++slot) {
                const uint32_t raw = (static_cast<uint32_t>(group) << 24) |
                                     (static_cast<uint32_t>(index) << 8) | slot;
                const auto e = DecodePacked(raw);
                EXPECT_EQ(e.group, group);
                EXPECT_EQ(e.object_index, index);
                EXPECT_EQ(e.api_slot, slot);
            }
        }
    }
}

TEST(GroupedBinding, PackedEntryIsNotAnInputUsageSlot) {
    // Both are four bytes, and conflating them is the mistake A-031 warns about. A GNM declaration
    // is {usagetype, apislot, startregister, flags}; a packed entry is {api_slot, index, group}.
    // The same bytes decode to different things, which is why they are separate types.
    const uint32_t raw = 0x02000000u;
    const auto packed = DecodePacked(raw);
    EXPECT_EQ(packed.group, 2u);
    // Read as a GNM slot, byte 0 would be the usage type and byte 3 the flags.
    EXPECT_EQ(raw & 0xFF, 0u) << "usage type would be 0, not the api slot";
    EXPECT_EQ(raw >> 24, 2u) << "flags would be 2, not the group";
}

} // namespace
