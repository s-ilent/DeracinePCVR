// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The texture cache uploads a newly created image from guest memory and then copies the image it
// replaced over it. Where the copy lands, the upload was wasted work. `CoveringCopyOf` is the
// proof that decides that, so it is tested against the shapes the copy can and cannot reach — a
// wrong "covers" answer loses guest data, which is exactly the black screen this had when it was
// tried without a predicate.

#include <gtest/gtest.h>

#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_coverage.h"

namespace {

using VideoCore::CopyCoverage;
using VideoCore::CoveringCopyOf;
using VideoCore::ImageInfo;

ImageInfo Make(u32 width, u32 height, u32 depth, u32 levels, u32 layers, u32 bits,
               AmdGpu::ImageType type, bool is_depth = false, bool has_stencil = false) {
    ImageInfo info{};
    info.size = {width, height, depth};
    info.resources = {.levels = levels, .layers = layers};
    info.num_bits = bits;
    info.num_samples = 1;
    info.type = type;
    info.props.is_volume = type == AmdGpu::ImageType::Color3D ? 1u : 0u;
    info.props.is_depth = is_depth ? 1u : 0u;
    info.props.has_stencil = has_stencil ? 1u : 0u;
    return info;
}

ImageInfo Color2D(u32 width, u32 height, u32 levels = 1, u32 layers = 1) {
    return Make(width, height, 1, levels, layers, 32, AmdGpu::ImageType::Color2D);
}

// The expand that matters: the guest asks for the same allocation with more levels.
TEST(ImageCoverage, SameExtentWithMoreDestinationLevelsCoversTheSourceLevels) {
    const auto src = Color2D(2048, 2048, 1, 1);
    const auto dst = Color2D(2048, 2048, 11, 1);

    const CopyCoverage covered = CoveringCopyOf(src, dst);
    EXPECT_EQ(covered.mips, 1u);
    EXPECT_EQ(covered.layers, 1u);
    EXPECT_TRUE(covered.CoversLevel(0, dst.resources.layers));
    EXPECT_FALSE(covered.CoversLevel(1, dst.resources.layers));
    EXPECT_FALSE(covered.CoversNothing());
}

TEST(ImageCoverage, EqualImagesCoverEveryLevel) {
    const auto info = Color2D(5760, 3240, 5, 2);

    const CopyCoverage covered = CoveringCopyOf(info, info);
    EXPECT_EQ(covered.mips, 5u);
    EXPECT_EQ(covered.layers, 2u);
    for (u32 level = 0; level < 5; ++level) {
        EXPECT_TRUE(covered.CoversLevel(level, 2));
    }
    EXPECT_FALSE(covered.CoversLevel(5, 2));
}

// A source with more levels than the destination: the copy reaches every destination level.
TEST(ImageCoverage, MoreSourceLevelsCoverTheDestination) {
    const auto src = Color2D(1024, 512, 11, 1);
    const auto dst = Color2D(1024, 512, 4, 1);

    const CopyCoverage covered = CoveringCopyOf(src, dst);
    EXPECT_EQ(covered.mips, 4u);
    EXPECT_EQ(covered.layers, 1u);
}

// The destination has more layers than the source. The copy writes layer 0 of each level only,
// so no level is overwritten whole and the refresh must still read every level.
TEST(ImageCoverage, MoreDestinationLayersLeaveEveryLevelPartlyUnwritten) {
    const auto src = Color2D(5760, 3240, 1, 1);
    const auto dst = Color2D(5760, 3240, 1, 2);

    const CopyCoverage covered = CoveringCopyOf(src, dst);
    EXPECT_EQ(covered.layers, 1u);
    EXPECT_FALSE(covered.CoversLevel(0, dst.resources.layers));
}

TEST(ImageCoverage, MoreDestinationLevelsWithFewerLayersCoverNothingWhole) {
    const auto src = Color2D(2048, 2048, 2, 1);
    const auto dst = Color2D(2048, 2048, 6, 4);

    const CopyCoverage covered = CoveringCopyOf(src, dst);
    EXPECT_EQ(covered.mips, 2u);
    EXPECT_EQ(covered.layers, 1u);
    EXPECT_FALSE(covered.CoversLevel(0, dst.resources.layers));
    EXPECT_FALSE(covered.CoversLevel(1, dst.resources.layers));
}

TEST(ImageCoverage, NarrowerSourceCoversNothing) {
    const auto src = Color2D(1280, 720, 1, 1);
    const auto dst = Color2D(1920, 1080, 1, 1);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

TEST(ImageCoverage, ShorterSourceCoversNothing) {
    const auto src = Color2D(1920, 720, 1, 1);
    const auto dst = Color2D(1920, 1080, 1, 1);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

// The copy would be a reinterpretation, not a transfer of the same texels.
TEST(ImageCoverage, DifferentTexelSizeCoversNothing) {
    const auto src = Make(1024, 1024, 1, 1, 1, 32, AmdGpu::ImageType::Color2D);
    const auto dst = Make(1024, 1024, 1, 1, 1, 8, AmdGpu::ImageType::Color2D);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

TEST(ImageCoverage, DifferentSampleCountCoversNothing) {
    auto src = Color2D(1024, 1024);
    auto dst = Color2D(1024, 1024);
    dst.num_samples = 2;

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

// Colour to depth is the representation change ResolveDepthOverlap handles; the copy's aspects do
// not line up, so this predicate must not guess.
TEST(ImageCoverage, ColourIntoDepthCoversNothing) {
    const auto src = Color2D(5760, 3240);
    const auto dst = Make(5760, 3240, 1, 1, 1, 32, AmdGpu::ImageType::Color2D, true, true);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

TEST(ImageCoverage, StencilMismatchCoversNothing) {
    const auto src = Make(2048, 2048, 1, 1, 1, 32, AmdGpu::ImageType::Color2D, true, false);
    const auto dst = Make(2048, 2048, 1, 1, 1, 32, AmdGpu::ImageType::Color2D, true, true);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

// Depth (and anything with a stencil aspect) is deliberately outside the predicate: the copy does
// not carry the tile metadata or the stencil beside the texels, and Déraciné's depth aliasing is
// handled by ResolveDepthOverlap instead.
TEST(ImageCoverage, DepthImagesCoverNothing) {
    const auto src = Make(2048, 2048, 1, 1, 1, 32, AmdGpu::ImageType::Color2D, true, false);
    const auto dst = Make(2048, 2048, 1, 3, 1, 32, AmdGpu::ImageType::Color2D, true, false);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

TEST(ImageCoverage, StencilImagesCoverNothing) {
    const auto src = Make(2048, 2048, 1, 1, 1, 32, AmdGpu::ImageType::Color2D, true, true);
    const auto dst = Make(2048, 2048, 1, 3, 1, 32, AmdGpu::ImageType::Color2D, true, true);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

// 2D to 3D and the reverse trade array layers for slices; the copy's region is not the source's
// extent there, so nothing is proven.
TEST(ImageCoverage, MismatchedDimensionalityCoversNothing) {
    const auto src = Color2D(256, 256, 1, 1);
    const auto dst =
        Make(256, 256, 1, 1, 1, 32, AmdGpu::ImageType::Color3D);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
    EXPECT_TRUE(CoveringCopyOf(dst, src).CoversNothing());
}

TEST(ImageCoverage, OneDimensionalImagesCoverNothing) {
    const auto src = Make(1024, 1, 1, 1, 1, 32, AmdGpu::ImageType::Color1D);

    EXPECT_TRUE(CoveringCopyOf(src, src).CoversNothing());
}

TEST(ImageCoverage, VolumesCoverWhenTheDepthReaches) {
    const auto src = Make(256, 256, 64, 1, 1, 32, AmdGpu::ImageType::Color3D);
    const auto dst = Make(256, 256, 32, 1, 1, 32, AmdGpu::ImageType::Color3D);

    const CopyCoverage covered = CoveringCopyOf(src, dst);
    EXPECT_EQ(covered.mips, 1u);
    EXPECT_EQ(covered.layers, 1u);
    EXPECT_TRUE(covered.CoversLevel(0, 1));
}

TEST(ImageCoverage, ShallowVolumeCoversNothing) {
    const auto src = Make(256, 256, 16, 1, 1, 32, AmdGpu::ImageType::Color3D);
    const auto dst = Make(256, 256, 32, 1, 1, 32, AmdGpu::ImageType::Color3D);

    EXPECT_TRUE(CoveringCopyOf(src, dst).CoversNothing());
}

// The decision the refresh takes per level, stated once: a level is skipped only when the copy
// writes it whole, in every layer.
TEST(ImageCoverage, CoversLevelIsFalseWithoutALayerForEveryDestinationLayer) {
    CopyCoverage covered;
    covered.mips = 4;
    covered.layers = 1;

    EXPECT_FALSE(covered.CoversLevel(0, 2));
    EXPECT_TRUE(covered.CoversLevel(0, 1));
    EXPECT_FALSE(covered.CoversLevel(4, 1));
}

TEST(ImageCoverage, EmptyCoverageCoversNoLevel) {
    const CopyCoverage covered{};

    EXPECT_TRUE(covered.CoversNothing());
    EXPECT_FALSE(covered.CoversLevel(0, 1));
}

} // namespace
