// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

using VideoCore::ImageInfo;

namespace {

// An 8-level, 8-layer RGBA16F array whose small mips keep a tile-aligned pitch of 8, the way
// PS4 render targets are laid out, plus a child view factory that reproduces the subresource
// aliases the texture cache has to recognise.
class ArrayTexture : public ::testing::Test {
protected:
    void SetUp() override {
        parent.type = AmdGpu::ImageType::Color2D;
        parent.pixel_format = vk::Format::eR16G16B16A16Sfloat;
        parent.num_bits = 64;
        parent.size = {128, 128, 1};
        parent.pitch = 128;
        parent.resources = {8, 8};
        parent.guest_address = 0x100000;
        parent.array_mode = AmdGpu::ArrayMode::Array1DTiledThin1;

        u32 offset = 0;
        for (u32 mip = 0; mip < 8; ++mip) {
            const u32 dim = 128u >> mip;
            const u32 pitch = dim < 8 ? 8 : dim;
            const u32 bytes = pitch * pitch * 8 * 8;
            parent.mips_layout[mip] = {bytes, pitch, pitch, offset};
            offset += bytes;
        }
        parent.guest_size = offset;
    }

    ImageInfo Child(u32 mip, u32 layer, u32 layer_count) const {
        ImageInfo child = parent;
        child.resources = {1, layer_count};
        child.size = {128u >> mip, 128u >> mip, 1};
        child.pitch = parent.mips_layout[mip].pitch;
        const u32 slice = parent.mips_layout[mip].size / parent.resources.layers;
        child.guest_address += parent.mips_layout[mip].offset + slice * layer;
        child.guest_size = slice * layer_count;
        return child;
    }

    ImageInfo parent{};
};

} // Anonymous namespace

TEST_F(ArrayTexture, MatchesEveryMipAndLayer) {
    for (u32 mip = 0; mip < 8; ++mip) {
        for (u32 layer = 0; layer < 8; ++layer) {
            const auto child = Child(mip, layer, 1);
            EXPECT_EQ(child.MipOf(parent), static_cast<s32>(mip)) << "mip " << mip;
            EXPECT_EQ(child.SliceOf(parent, static_cast<s32>(mip)), static_cast<s32>(layer))
                << "mip " << mip << " layer " << layer;
        }
    }
}

// Small uncompressed mips keep a tile-aligned pitch; a bit-shifted parent pitch would reject
// them, so the parent's stored mip layout has to be compared instead.
TEST_F(ArrayTexture, MatchesPaddedSmallMips) {
    for (u32 mip = 4; mip < 8; ++mip) {
        const auto child = Child(mip, 0, 1);
        EXPECT_EQ(child.MipOf(parent), static_cast<s32>(mip)) << "padded mip " << mip;
        EXPECT_EQ(child.SliceOf(parent, static_cast<s32>(mip)), 0) << "padded mip " << mip;
    }
}

// The layer index is measured in individual slices, not in the size of the whole view.
TEST_F(ArrayTexture, MultiLayerViewStartsAtItsOwnSlice) {
    const auto pair = Child(1, 2, 2);
    EXPECT_EQ(pair.SliceOf(parent, 1), 2);
}

TEST_F(ArrayTexture, MultiLayerViewNeedNotAlignToItsTotalSize) {
    const auto pair = Child(1, 1, 2);
    EXPECT_EQ(pair.SliceOf(parent, 1), 1);
}

TEST_F(ArrayTexture, RejectsRangeCrossingEndOfMip) {
    const auto invalid = Child(1, 7, 2);
    EXPECT_EQ(invalid.SliceOf(parent, 1), -1);
}

TEST_F(ArrayTexture, RejectsMisalignedAddress) {
    auto invalid = Child(1, 1, 1);
    ++invalid.guest_address;
    EXPECT_EQ(invalid.SliceOf(parent, 1), -1);
}

TEST_F(ArrayTexture, RejectsMipBeyondTheParentLevels) {
    const auto child = Child(1, 1, 1);
    EXPECT_EQ(child.SliceOf(parent, 8), -1);
}

TEST_F(ArrayTexture, RejectsNegativeMip) {
    const auto child = Child(1, 1, 1);
    EXPECT_EQ(child.SliceOf(parent, -1), -1);
}
