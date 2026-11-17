// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The presentation size policy: what one eye is sent to the headset, next to what the title drew.
// The numbers in the acceptance table are the fixture: a Steam Frame runtime recommends
// 3036x3036 an eye while the title renders 2880x3240, 3072x3456 or 3840x4320.

#include "core/vr/presentation_size.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "vr_projection_math.h"

namespace {

using Core::Vr::PlanPresentationSize;
using Core::Vr::PresentationSizeLimits;
using Core::Vr::PresentationSizeMode;
using Core::Vr::PresentationSizeRequest;

// What SteamVR/OpenXR reported for a Steam Frame: 3036x3036 an eye, up to 8192x8192 an image.
PresentationSizeLimits SteamFrameLimits() {
    return PresentationSizeLimits{
        .max_eye_width = 8192,
        .max_eye_height = 8192,
        .max_image_width = 8192,
        .max_image_height = 8192,
    };
}

PresentationSizeRequest Recommended(float supersample = 1.0f) {
    return PresentationSizeRequest{
        .mode = PresentationSizeMode::Recommended,
        .supersample = supersample,
    };
}

PresentationSizeRequest Explicit(std::uint32_t width, std::uint32_t height = 0) {
    return PresentationSizeRequest{
        .mode = PresentationSizeMode::Explicit,
        .explicit_width = width,
        .explicit_height = height,
    };
}

// The three internal sizes of the acceptance table, and the one output they must share.
constexpr std::array<std::array<std::uint32_t, 2>, 3> kGuestSizes{{
    {2880, 3240},
    {3072, 3456},
    {3840, 4320},
}};

TEST(VrPresentationSize, RecommendationIsIndependentOfTheTitlesRenderSize) {
    for (const auto& guest : kGuestSizes) {
        const auto size =
            PlanPresentationSize(guest[0], guest[1], 3036, 3036, SteamFrameLimits(), Recommended());
        EXPECT_EQ(size.mode, PresentationSizeMode::Recommended) << guest[0] << "x" << guest[1];
        // The runtime's own numbers, not rounded to a multiple of 16 and not derived from the
        // title's 8:9 aspect.
        EXPECT_EQ(size.eye_width, 3036u);
        EXPECT_EQ(size.eye_height, 3036u);
        EXPECT_FALSE(size.clamped);
    }
}

TEST(VrPresentationSize, GuestModeFollowsTheTitleAndKeepsTheOldAlignment) {
    const auto size = PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(),
                                           PresentationSizeRequest{
                                               .mode = PresentationSizeMode::Guest,
                                           });
    EXPECT_EQ(size.mode, PresentationSizeMode::Guest);
    EXPECT_EQ(size.eye_width, 2880u);
    EXPECT_EQ(size.eye_height, 3248u); // 3240 rounded up to 16, exactly as before
}

TEST(VrPresentationSize, ExplicitSizeKeepsTheTitlesAspectWhenOnlyAWidthIsGiven) {
    const auto size =
        PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(), Explicit(3600));
    EXPECT_EQ(size.mode, PresentationSizeMode::Explicit);
    EXPECT_EQ(size.eye_width, 3600u);
    EXPECT_EQ(size.eye_height, 4064u); // round(3240 * 3600 / 2880) = 4050, up to a multiple of 16
}

TEST(VrPresentationSize, ExplicitBothAxesAreTakenAsGiven) {
    const auto size =
        PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(), Explicit(3036, 3036));
    EXPECT_EQ(size.eye_width, 3040u); // a number somebody typed is aligned; a recommendation is not
    EXPECT_EQ(size.eye_height, 3040u);
}

TEST(VrPresentationSize, SupersamplingMultipliesTheChoiceAndKeepsItsMode) {
    const auto size =
        PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(), Recommended(1.25f));
    EXPECT_EQ(size.mode, PresentationSizeMode::Recommended);
    EXPECT_EQ(size.eye_width, 3795u); // 3036 * 1.25, not aligned for the same reason
    EXPECT_EQ(size.eye_height, 3795u);
}

TEST(VrPresentationSize, ShrinkingTheTitleByHalfStillAlignsAsItAlwaysDid) {
    const auto size = PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(),
                                           PresentationSizeRequest{
                                               .mode = PresentationSizeMode::Guest,
                                               .supersample = 0.5f,
                                           });
    EXPECT_EQ(size.eye_width, 1440u);
    EXPECT_EQ(size.eye_height, 1632u); // 1620 rounded up, as the scale factor always behaved
}

TEST(VrPresentationSize, RuntimeMaximumsCutTheRequestAndSaySo) {
    // A 1.5x supersampled 3036 would need 9108 pixels across for both eyes; the runtime offers
    // 8192, so an eye gets half of that.
    const auto clipped =
        PlanPresentationSize(2880, 3240, 3036, 3036, SteamFrameLimits(), Recommended(1.5f));
    EXPECT_TRUE(clipped.clamped);
    EXPECT_EQ(clipped.eye_width, 4096u);
    EXPECT_LE(clipped.eye_width * 2, 8192u);
    EXPECT_EQ(clipped.eye_height, 4554u); // under the 8192 height limit, untouched

    // A per-eye maximum below the recommendation wins as well.
    auto narrow = SteamFrameLimits();
    narrow.max_eye_width = 2048;
    const auto capped = PlanPresentationSize(2880, 3240, 3036, 3036, narrow, Recommended());
    EXPECT_TRUE(capped.clamped);
    EXPECT_EQ(capped.eye_width, 2048u);
}

TEST(VrPresentationSize, NoRecommendationFallsBackToTheTitleAndSaysSo) {
    const auto size = PlanPresentationSize(2880, 3240, 0, 0, SteamFrameLimits(), Recommended());
    EXPECT_EQ(size.mode, PresentationSizeMode::Guest);
    EXPECT_EQ(size.eye_width, 2880u);
    EXPECT_EQ(size.eye_height, 3248u);
    EXPECT_FALSE(size.clamped);
}

TEST(VrPresentationSize, ADegenerateTitleSizeProducesNothing) {
    const auto size = PlanPresentationSize(0, 0, 3036, 3036, SteamFrameLimits(), Recommended());
    EXPECT_EQ(size.eye_width, 0u);
    EXPECT_EQ(size.eye_height, 0u);
}

// The geometry half of the acceptance test: whatever the presentation size, the picture is still
// the title's frustum edge to edge, so a full-rect resample into the new size is exact. What the
// size does change is the sampling grid, and it changes it per axis: a square output for an 8:9
// source is anamorphic (more pixels across, fewer up) and still geometrically exact.
TEST(VrPresentationSize, EveryAcceptedPresentationSizeKeepsTheGeometryExact) {
    using namespace VrProj;
    const FovTan psvr = FovFromHalfAnglesDeg(-50.368309, 49.752363, 51.626199, -51.626199);
    const double horizontal_deg = TotalHorizontalDeg(psvr);
    const double vertical_deg = TotalVerticalDeg(psvr);
    for (const auto& guest : kGuestSizes) {
        const auto size =
            PlanPresentationSize(guest[0], guest[1], 3036, 3036, SteamFrameLimits(), Recommended());
        ASSERT_EQ(size.eye_width, 3036u);
        ASSERT_EQ(size.eye_height, 3036u);

        const CompareResult result = Compare(psvr, FullRect(), psvr, FullRect(), 65);
        EXPECT_LT(result.max_deg, 1e-9) << guest[0] << "x" << guest[1];

        // Pixels per degree, source against output: the two axes are redistributed by the ratio of
        // the output aspect to the source's, and the geometry is untouched by it.
        const double src_x = guest[0] / horizontal_deg;
        const double src_y = guest[1] / vertical_deg;
        const double dst_x = size.eye_width / horizontal_deg;
        const double dst_y = size.eye_height / vertical_deg;
        EXPECT_NEAR((dst_x / dst_y) / (src_x / src_y),
                    (static_cast<double>(size.eye_width) / size.eye_height) *
                        (static_cast<double>(guest[1]) / guest[0]),
                    1e-12);
    }
}

} // namespace
