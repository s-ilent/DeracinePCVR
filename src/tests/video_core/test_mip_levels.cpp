// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "video_core/amdgpu/resource.h"

namespace {

// Guest image descriptors store the extent minus one and can declare a last_level past the
// extent's full chain; Vulkan rejects those levels and the uploads that use them.
AmdGpu::Image MakeImage(AmdGpu::ImageType type, u32 width, u32 height, u32 depth,
                        u32 declared_levels) {
    AmdGpu::Image image{};
    image.type = u64(type);
    image.width = u64(width - 1);
    image.height = u64(height - 1);
    image.depth = u64(depth - 1);
    image.last_level = u64(declared_levels - 1);
    return image;
}

using AmdGpu::ImageType;

TEST(MipChain, DeclaredLevelsAreClampedToTheExtent) {
    const auto image = MakeImage(ImageType::Color2D, 64, 64, 1, 12);
    EXPECT_EQ(image.NumLevels(), 7u);
}

TEST(MipChain, FewerDeclaredLevelsAreKept) {
    const auto image = MakeImage(ImageType::Color2D, 64, 64, 1, 3);
    EXPECT_EQ(image.NumLevels(), 3u);
}

TEST(MipChain, NonPowerOfTwoExtent) {
    const auto image = MakeImage(ImageType::Color2D, 100, 100, 1, 12);
    EXPECT_EQ(image.NumLevels(), 7u);
}

TEST(MipChain, SmallestImageHasOneLevel) {
    const auto image = MakeImage(ImageType::Color2D, 1, 1, 1, 12);
    EXPECT_EQ(image.NumLevels(), 1u);
}

TEST(MipChain, ThreeDimensionalExtentCountsDepth) {
    const auto image = MakeImage(ImageType::Color3D, 64, 64, 64, 12);
    EXPECT_EQ(image.NumLevels(), 7u);
}

TEST(MipChain, MultisampledImageHasOneLevel) {
    const auto image = MakeImage(ImageType::Color2DMsaa, 128, 128, 1, 4);
    EXPECT_EQ(image.NumLevels(), 1u);
}

} // namespace
