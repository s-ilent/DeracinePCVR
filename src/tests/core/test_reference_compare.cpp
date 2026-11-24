// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for reference-image comparison.
//
// The distinction these exist to preserve: a rendered image that differs from the reference is a
// rendering mismatch, not a successful replay. A comparison that reported success for a wrong
// extent or a missing reference would make every downstream claim unreliable.

#include <gtest/gtest.h>

#include "reference_compare.h"

namespace {

using reference::Comparison;
using reference::Rendered;
using reference::Status;

/// A package carrying a reference of the given extent, filled with a known pattern.
package::Package MakePackage(package::u32 width, package::u32 height, package::u32 bpp,
                             package::u8 seed = 0) {
    package::Package package{};
    package.schema_version = package::SchemaEffectiveShader;
    package.reference.width = width;
    package.reference.height = height;
    package.reference.bytes_per_pixel = bpp;
    package.reference.format = 37;
    package.reference.post_bytes.resize(static_cast<size_t>(width) * height * bpp);
    for (size_t i = 0; i < package.reference.post_bytes.size(); ++i) {
        package.reference.post_bytes[i] = static_cast<package::u8>((i + seed) & 0xFF);
    }
    package.pipeline_state.samples = 1;
    return package;
}

Rendered MakeRendered(const package::Package& package) {
    Rendered rendered{};
    rendered.width = package.reference.width;
    rendered.height = package.reference.height;
    rendered.bytes_per_pixel = package.reference.bytes_per_pixel;
    rendered.pixels = package.reference.post_bytes;
    return rendered;
}

TEST(ReferenceCompare, AnIdenticalImageMatchesExactly) {
    const auto package = MakePackage(4, 4, 4);
    const auto comparison = reference::Compare(package, MakeRendered(package));
    EXPECT_EQ(comparison.status, Status::ExactMatch);
    EXPECT_TRUE(comparison.exact_match);
    EXPECT_EQ(comparison.different_pixels, 0u);
    EXPECT_EQ(comparison.max_channel_error, 0u);
    EXPECT_EQ(comparison.compared_pixels, 16u);
}

TEST(ReferenceCompare, OneCorruptedChannelIsDetected) {
    auto package = MakePackage(4, 4, 4);
    auto rendered = MakeRendered(package);
    // Byte 5 holds 5, so inverting it gives 250 and the delta is 245.
    rendered.pixels[5] ^= 0xFF;
    const auto comparison = reference::Compare(package, rendered);
    EXPECT_EQ(comparison.status, Status::PixelMismatch);
    EXPECT_FALSE(comparison.exact_match);
    EXPECT_EQ(comparison.different_pixels, 1u);
    EXPECT_EQ(comparison.max_channel_error, 245u);
}

TEST(ReferenceCompare, AWrongExtentIsIncompatibleNotAMismatch) {
    // Comparing images of different extents would produce a pixel count that looks like a rendering
    // failure when it is a configuration failure.
    auto package = MakePackage(4, 4, 4);
    auto rendered = MakeRendered(package);
    rendered.width = 8;
    const auto comparison = reference::Compare(package, rendered);
    EXPECT_EQ(comparison.status, Status::IncompatibleReference);
    EXPECT_NE(comparison.reason.find("but the reference is"), std::string::npos);
}

TEST(ReferenceCompare, AWrongPixelSizeIsIncompatible) {
    auto package = MakePackage(4, 4, 4);
    auto rendered = MakeRendered(package);
    rendered.bytes_per_pixel = 3;
    const auto comparison = reference::Compare(package, rendered);
    EXPECT_EQ(comparison.status, Status::IncompatibleReference);
}

TEST(ReferenceCompare, ATruncatedReferenceIsIncompatible) {
    // A reference shorter than its declared extent would be read past its end.
    auto package = MakePackage(4, 4, 4);
    package.reference.post_bytes.resize(10);
    const auto comparison = reference::Compare(package, MakeRendered(package));
    EXPECT_EQ(comparison.status, Status::IncompatibleReference);
    EXPECT_NE(comparison.reason.find("bytes but its extent"), std::string::npos);
}

TEST(ReferenceCompare, AMissingReferenceIsReported) {
    package::Package package{};
    package.schema_version = package::SchemaEffectiveShader;
    Rendered rendered{};
    const auto comparison = reference::Compare(package, rendered);
    EXPECT_EQ(comparison.status, Status::MissingReference);
}

TEST(ReferenceCompare, ADeclaredButAbsentReferenceIsAnIncompleteCapture) {
    auto package = MakePackage(4, 4, 4);
    package.reference.post_bytes.clear();
    const auto comparison = reference::Compare(package, MakeRendered(package));
    EXPECT_EQ(comparison.status, Status::IncompleteCapture);
}

TEST(ReferenceCompare, AMissingPreDrawImageIsIncompleteWhenRequired) {
    // A draw whose output depends on what the target held before it cannot be verified without the
    // pre-draw image. Treating a missing one as a cleared target would compare against a state the
    // draw never saw.
    auto package = MakePackage(4, 4, 4);
    package.reference.has_pre = false;
    const auto comparison = reference::Compare(package, MakeRendered(package),
                                               /*require_pre=*/true);
    EXPECT_EQ(comparison.status, Status::IncompleteCapture);
    EXPECT_NE(comparison.reason.find("pre-draw"), std::string::npos);
}

TEST(ReferenceCompare, AMultisampledReferenceIsUnsupported) {
    auto package = MakePackage(4, 4, 4);
    package.pipeline_state.samples = 4;
    const auto comparison = reference::Compare(package, MakeRendered(package));
    EXPECT_EQ(comparison.status, Status::UnsupportedConfiguration);
    EXPECT_NE(comparison.reason.find("samples"), std::string::npos);
}

TEST(ReferenceCompare, TheErrorMetricsAreComputedOverEveryChannel) {
    // Two pixels differ by one in one channel each, so the mean is over all channels of all pixels.
    auto package = MakePackage(2, 2, 4);
    auto rendered = MakeRendered(package);
    rendered.pixels[0] = static_cast<package::u8>(rendered.pixels[0] + 2);
    rendered.pixels[4] = static_cast<package::u8>(rendered.pixels[4] + 2);
    const auto comparison = reference::Compare(package, rendered);
    EXPECT_EQ(comparison.different_pixels, 2u);
    EXPECT_EQ(comparison.max_channel_error, 2u);
    // 4 total error over 4 pixels x 4 channels = 0.25.
    EXPECT_NEAR(comparison.mean_absolute_error, 0.25, 1e-9);
}

} // namespace
