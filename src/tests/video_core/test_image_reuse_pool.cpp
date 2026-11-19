// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_image_reuse_pool.h"

namespace {

using Vulkan::ImageReusePool;

vk::ImageCreateInfo MakeCreateInfo() {
    return vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR32Sfloat,
        .extent = {5760, 3240, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
}

bool Same(const vk::ImageCreateInfo& a, const vk::ImageCreateInfo& b) {
    return ImageReusePool::SameCreateShape(a, {}, b, {});
}

TEST(ImageReusePool, ExactCreateShapeMatches) {
    const auto a = MakeCreateInfo();
    const auto b = MakeCreateInfo();
    EXPECT_TRUE(Same(a, b));
}

TEST(ImageReusePool, EveryCreateFieldParticipatesInKey) {
    const auto base = MakeCreateInfo();
    auto changed = base;
    changed.flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.imageType = vk::ImageType::e3D;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.format = vk::Format::eD32SfloatS8Uint;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    ++changed.extent.width;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    ++changed.extent.height;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    ++changed.extent.depth;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    ++changed.mipLevels;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    ++changed.arrayLayers;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.samples = vk::SampleCountFlagBits::e4;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.tiling = vk::ImageTiling::eLinear;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.usage |= vk::ImageUsageFlagBits::eStorage;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.sharingMode = vk::SharingMode::eConcurrent;
    EXPECT_FALSE(Same(base, changed));
    changed = base;
    changed.initialLayout = vk::ImageLayout::eGeneral;
    EXPECT_FALSE(Same(base, changed));
}

TEST(ImageReusePool, QueueFamilyListLengthAndContentsParticipateInKey) {
    const std::array<u32, 1> family_a{2};
    const std::array<u32, 1> family_b{3};
    const std::array<u32, 2> family_c{2, 4};
    auto a = MakeCreateInfo();
    auto b = MakeCreateInfo();
    a.sharingMode = b.sharingMode = vk::SharingMode::eConcurrent;
    a.queueFamilyIndexCount = b.queueFamilyIndexCount = 1;
    a.pQueueFamilyIndices = family_a.data();
    b.pQueueFamilyIndices = family_b.data();
    EXPECT_FALSE(Same(a, b));
    b.pQueueFamilyIndices = family_a.data();
    EXPECT_TRUE(Same(a, b));
    b.queueFamilyIndexCount = 2;
    b.pQueueFamilyIndices = family_c.data();
    EXPECT_FALSE(Same(a, b));
}

TEST(ImageReusePool, ViewFormatOrderIsIgnored) {
    const auto ci = MakeCreateInfo();
    const std::array a{vk::Format::eR32Sfloat, vk::Format::eR8Unorm};
    const std::array b{vk::Format::eR8Unorm, vk::Format::eR32Sfloat};
    EXPECT_TRUE(ImageReusePool::SameCreateShape(ci, a, ci, b));
}

TEST(ImageReusePool, PNextAndAliasImagesAreRejected) {
    const auto ci = MakeCreateInfo();
    auto with_pnext = ci;
    const u32 chain_marker{};
    with_pnext.pNext = &chain_marker;
    EXPECT_FALSE(Same(ci, with_pnext));
    auto alias = ci;
    alias.flags |= vk::ImageCreateFlagBits::eAlias;
    EXPECT_FALSE(Same(ci, alias));
}

TEST(ImageReusePool, AllocationThresholdUsesActualByteCount) {
    constexpr u64 minimum = 16 * 1024 * 1024;
    EXPECT_FALSE(ImageReusePool::EligibleSize(minimum - 1, minimum));
    EXPECT_TRUE(ImageReusePool::EligibleSize(minimum, minimum));
    EXPECT_TRUE(ImageReusePool::EligibleSize(minimum + 1, minimum));
}

TEST(ImageReusePool, BoundsRejectCapacityAndByteOverflow) {
    constexpr u64 budget = 256 * 1024 * 1024;
    EXPECT_TRUE(ImageReusePool::FitsBounds(5, budget - 32, 32, 6, budget));
    EXPECT_FALSE(ImageReusePool::FitsBounds(6, budget - 32, 32, 6, budget));
    EXPECT_FALSE(ImageReusePool::FitsBounds(5, budget - 31, 32, 6, budget));
    EXPECT_FALSE(ImageReusePool::FitsBounds(0, budget + 1, 0, 6, budget));
}

} // namespace
