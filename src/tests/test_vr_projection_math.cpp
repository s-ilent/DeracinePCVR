// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Independent math harness for the PSVR -> OpenXR projection mapping question.
// Nothing here touches the emulator, Vulkan or OpenXR: it only proves when a
// full-rectangle UV resample of a guest eye image is geometrically exact and when
// it is not.
//
// Fixture numbers (test fixtures, not measured device values):
//   guest eye image   2880 x 3240  (8:9)
//   OpenXR swapchain  3036 x 3036  (1:1)
//   nominal eye fov   100 deg horizontal, 100 deg vertical unless stated

#include "vr_projection_math.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace VrProj;

constexpr int kSrcWidth = 2880;
constexpr int kSrcHeight = 3240;
constexpr int kDstWidth = 3036;
constexpr double kIpdM = 0.064;

// Loose enough for double round-off in the composed map (two lerps, one divide),
// tight enough that any real modelling error shows up.
constexpr double kExactDeg = 1e-9;
// Exact algebraic equalities that never normalise twice.
constexpr double kAlgebraicTol = 1e-12;

FovTan EyeFov() {
    return FovSymmetricDeg(100.0, 100.0);
}

// Frustum used by the asymmetric tests: 40 deg left, 50 deg right, 20 up, 25 down.
FovTan AsymmetricFov() {
    return FovFromHalfAnglesDeg(-40.0, 50.0, 20.0, -25.0);
}

// Destination-view u at which an affine full-rect resample of `rect_src` reproduces
// the source tangent `tx`. This is the closed form of the affine composition, used
// so expectations are not read back out of the implementation.
double DstUForSourceTangent(const FovTan& fov_src, const Rect& rect_src, const Rect& dst_rect,
                            double tx) {
    const double s = UFromTanX(FovOfRect(fov_src, rect_src), tx);
    return dst_rect.u0 + s * RectWidth(dst_rect);
}

std::string Deg(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9f", v);
    return std::string(buf);
}

std::string Px(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return std::string(buf);
}

// RecordProperty keys end up as XML attribute names, so keep them identifier-safe.
std::string Key(const char* prefix, const char* name) {
    std::string out = prefix;
    for (const char* p = name; *p != '\0'; ++p) {
        out.push_back((*p == ' ' || *p == ':' || *p == '(' || *p == ')' || *p == '/' || *p == ',' ||
                       *p == '-')
                          ? '_'
                          : *p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1. Identity
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, Identity_FullRectIsExact) {
    const FovTan fov = EyeFov();
    const Rect full = FullRect();

    ASSERT_TRUE(ResampleIsExact(fov, full, fov, full));

    const CompareResult r = Compare(fov, full, fov, full, 65);
    EXPECT_LT(r.max_deg, kExactDeg);
    EXPECT_LT(r.mean_abs_deg, kExactDeg);
    EXPECT_EQ(r.samples, 65 * 65);

    // The composed map reproduces the destination ray exactly at every sample,
    // including the four frustum corners.
    for (int j = 0; j < 5; ++j) {
        for (int i = 0; i < 5; ++i) {
            const double ndc_x = -1.0 + 0.5 * i;
            const double ndc_y = -1.0 + 0.5 * j;
            const Uv uv = ResampleSourceUvForDest(fov, full, ndc_x, ndc_y, full);
            const Ray claimed = RayFromXrFov(fov, ndc_x, ndc_y);
            const Ray actual = RayFromSourceUv(fov, full, uv.u, uv.v);
            EXPECT_LT(AngleBetweenDeg(claimed, actual), kExactDeg)
                << "ndc " << ndc_x << "," << ndc_y;
            EXPECT_NEAR(uv.u, 0.5 * (ndc_x + 1.0), kAlgebraicTol);
            EXPECT_NEAR(uv.v, 0.5 * (1.0 - ndc_y), kAlgebraicTol);
        }
    }
}

// ---------------------------------------------------------------------------
// 2. 8:9 source -> 1:1 destination with matching FOV
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, EightNineSource_SquareDestination_MatchingFov_IsExact) {
    // Both images present the same frustum. Pixel aspect is 8:9 on one side and
    // 1:1 on the other, which is irrelevant to geometry.
    const FovTan fov = EyeFov();
    const Rect full = FullRect();

    ASSERT_TRUE(ResampleIsExact(fov, full, fov, full));
    const CompareResult r = Compare(fov, full, fov, full, 129);
    EXPECT_LT(r.max_deg, kExactDeg);
    EXPECT_LT(r.mean_abs_deg, kExactDeg);

    // The resample deliberately stretches horizontally by 1.0542 and squashes
    // vertically by 0.9370; the 1.125 ratio is exactly the 8:9 -> 1:1 pixel-grid
    // difference, and the geometry is still perfect.
    const double scale_x = static_cast<double>(kDstWidth) / kSrcWidth;
    const double scale_y = static_cast<double>(kDstWidth) / kSrcHeight;
    EXPECT_NEAR(scale_x / scale_y, 1.125, 1e-12);

    RecordProperty("matching_fov_max_deg", Deg(r.max_deg));
}

TEST(VrProjectionMath, EightNineSource_SquareDestination_PillarboxIsWrong) {
    // Same source, same destination FOV, but the source is written into an 8:9
    // sub-rectangle of the 1:1 view: the "obvious" aspect-preserving choice.
    const FovTan fov = EyeFov();
    const Rect full = FullRect();
    const double half_gap = 0.5 * (1.0 - 8.0 / 9.0);
    const Rect pillarbox{half_gap, 0.0, 1.0 - half_gap, 1.0};

    EXPECT_FALSE(ResampleIsExact(fov, full, fov, pillarbox));
    EXPECT_GT(MaxTangentDelta(FovOfRect(fov, full), FovOfRect(fov, pillarbox)), 0.13);

    // Hand-computable edge error: the bar edge claims atan(1.0593376) = 46.6505 deg
    // while the outermost source column really is 50 deg.
    const double edge_claim_deg = ToDegrees(std::atan(std::abs(TanX(fov, pillarbox.u0))));
    const double edge_true_deg = ToDegrees(std::atan(std::abs(fov.l)));
    EXPECT_NEAR(edge_true_deg - edge_claim_deg, 3.3496, 1e-3);

    const CompareResult r = Compare(fov, full, fov, pillarbox, 129);
    EXPECT_GT(r.max_deg, 3.36);
    EXPECT_LT(r.max_deg, 3.45); // slightly above 3.3704: the bar band is extrapolated
    EXPECT_NEAR(r.max_left_deg, r.max_right_deg, 1e-9);
    EXPECT_GT(r.mean_abs_deg, 1.0);
    EXPECT_LT(r.mean_abs_deg, 2.5);
    EXPECT_GT(std::abs(r.worst_ndc_x), 0.88);

    // Letterboxing the other way round is equally wrong: read a 1:1 window out of
    // the 8:9 source and stretch it over the square view.
    const Rect letterbox_src{0.0, half_gap, 1.0, 1.0 - half_gap};
    EXPECT_FALSE(ResampleIsExact(fov, letterbox_src, fov, full));
    const CompareResult r2 = Compare(fov, letterbox_src, fov, full, 129);
    EXPECT_GT(r2.max_deg, 3.36);
    EXPECT_LT(r2.max_deg, 3.45);
    EXPECT_GT(std::abs(r2.worst_ndc_y), 0.88);

    std::printf("[pillarbox] exact edge error %.6f deg, measured max %.6f deg, letterbox-src "
                "max %.6f deg\n",
                edge_true_deg - edge_claim_deg, r.max_deg, r2.max_deg);
    RecordProperty("pillarbox_edge_deg", Deg(edge_true_deg - edge_claim_deg));
    RecordProperty("pillarbox_max_deg", Deg(r.max_deg));
    RecordProperty("letterbox_src_max_deg", Deg(r2.max_deg));
}

// ---------------------------------------------------------------------------
// 3. 8:9 source -> 1:1 destination with differing FOV
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, DifferingFov_ErrorIsSmoothEdgePeakedAndZeroAtCentre) {
    const FovTan src = FovSymmetricDeg(100.0, 100.0);
    const FovTan dst = FovSymmetricDeg(90.0, 100.0); // 10 deg narrower horizontally
    const Rect full = FullRect();

    const CompareResult r = Compare(src, full, dst, full, 129);
    // The frustum edges are off by exactly 50 - 45 deg; the measured maximum is a
    // shade above that because atan is nonlinear on both sides (see below).
    const double edge_err = ToDegrees(std::atan(src.r)) - ToDegrees(std::atan(dst.r));
    EXPECT_NEAR(edge_err, 5.0, 1e-12);
    EXPECT_GT(r.max_deg, 5.0);
    EXPECT_LT(r.max_deg, 5.05);
    EXPECT_GT(r.mean_abs_deg, 3.0);
    EXPECT_LT(r.mean_abs_deg, 4.5);

    // The vertical mapping is untouched: the vertical tangent the resample reads is
    // bit-identical to the one the destination pixel claims. (The reported elevation
    // of the two rays still differs slightly, because elevation is measured relative
    // to the ray's own azimuth, which is what is mismatched here.)
    for (int j = 0; j <= 16; ++j) {
        const double ndc_y = -1.0 + j / 8.0;
        const Uv uv = ResampleSourceUvForDest(src, full, 0.33, ndc_y, full);
        EXPECT_NEAR(uv.v, 0.5 * (1.0 - ndc_y), 1e-15);
        EXPECT_NEAR(TanY(src, uv.v), std::lerp(dst.b, dst.t, 0.5 * (ndc_y + 1.0)), 1e-15);
    }

    // Zero at the centre, monotonically increasing outwards over the interior.
    double previous = -1.0;
    for (int i = 0; i <= 52; ++i) { // ndc_x in [0, 0.9]
        const double ndc_x = 0.9 * i / 52.0;
        const double claimed = AzimuthDeg(RayFromXrFov(dst, ndc_x, 0.0));
        const Uv uv = ResampleSourceUvForDest(src, full, ndc_x, 0.0, full);
        const double actual = AzimuthDeg(RayFromSourceUv(src, full, uv.u, uv.v));
        const double err = std::abs(actual - claimed);
        EXPECT_GE(err, previous - 1e-12) << "not monotone at ndc_x=" << ndc_x;
        previous = err;
    }
    // Exact zero on the axis the two frusta share.
    const Uv centre = ResampleSourceUvForDest(src, full, 0.0, 0.0, full);
    EXPECT_LT(AngleBetweenDeg(RayFromXrFov(dst, 0.0, 0.0),
                              RayFromSourceUv(src, full, centre.u, centre.v)),
              1e-12);

    std::printf("[fov-mismatch] edge error %.6f deg, measured max %.6f deg at ndc_x=%.4f, mean "
                "%.6f deg\n",
                edge_err, r.max_deg, r.worst_ndc_x, r.mean_abs_deg);
    RecordProperty("fov_mismatch_edge_deg", Deg(edge_err));
    RecordProperty("fov_mismatch_max_deg", Deg(r.max_deg));
    RecordProperty("fov_mismatch_mean_deg", Deg(r.mean_abs_deg));
}

TEST(VrProjectionMath, DifferingFov_MinimalCropRestoresExactness) {
    const FovTan src = FovSymmetricDeg(100.0, 100.0);
    const FovTan dst = FovSymmetricDeg(90.0, 100.0);
    const Rect full = FullRect();

    const RectMatchPlan plan = PlanCrop(src, full, dst, full, kSrcWidth, kSrcHeight);
    ASSERT_TRUE(plan.possible);
    EXPECT_TRUE(ResampleIsExact(src, plan.src_rect, dst, plan.dst_rect, 1e-12));

    // Horizontal crop only, equal on both sides: the required source column is where
    // the destination's +-45 deg edges land inside the source's +-50 deg image.
    const double expected_u = UFromTanX(src, dst.l);
    EXPECT_NEAR(expected_u, 0.0804502, 1e-6);
    EXPECT_NEAR(plan.src_rect.u0, expected_u, 1e-12);
    EXPECT_NEAR(plan.src_rect.u1, 1.0 - expected_u, 1e-12);
    EXPECT_NEAR(plan.src_rect.v0, 0.0, 1e-12);
    EXPECT_NEAR(plan.src_rect.v1, 1.0, 1e-12);
    EXPECT_NEAR(plan.crop_px[0], expected_u * kSrcWidth, 1e-9);
    EXPECT_NEAR(plan.crop_px[1], expected_u * kSrcWidth, 1e-9);
    EXPECT_NEAR(plan.crop_px[2], 0.0, 1e-12);
    EXPECT_NEAR(plan.crop_px[3], 0.0, 1e-12);
    EXPECT_NEAR(plan.kept_area_frac, 1.0 - 2.0 * expected_u, 1e-12);

    // Destination fills the whole view, so no bars there.
    EXPECT_NEAR(plan.dst_rect.u0, 0.0, 1e-12);
    EXPECT_NEAR(plan.dst_rect.u1, 1.0, 1e-12);

    const CompareResult cropped = Compare(src, plan.src_rect, dst, plan.dst_rect, 129);
    EXPECT_LT(cropped.max_deg, kExactDeg);

    // The plan is a no-op when the frusta already agree.
    const RectMatchPlan same = PlanCrop(src, full, src, full, kSrcWidth, kSrcHeight);
    ASSERT_TRUE(same.possible);
    EXPECT_NEAR(same.crop_px[0], 0.0, 1e-12);
    EXPECT_NEAR(same.crop_px[1], 0.0, 1e-12);
    EXPECT_NEAR(same.kept_area_frac, 1.0, 1e-15);

    // Mirror case: the source is the *narrower* side, so it is not cropped at all and
    // the bars land on the destination instead (dst_rect inset, crop_px zero).
    const RectMatchPlan widest =
        PlanCrop(dst, full, src, full, kSrcWidth, kSrcHeight); // 90 deg source, 100 deg dest
    ASSERT_TRUE(widest.possible);
    EXPECT_NEAR(widest.src_rect.u0, 0.0, 1e-12);
    EXPECT_NEAR(widest.src_rect.u1, 1.0, 1e-12);
    EXPECT_NEAR(widest.crop_px[0], 0.0, 1e-12);
    EXPECT_NEAR(widest.kept_area_frac, 1.0, 1e-15);
    EXPECT_NEAR(widest.dst_rect.u0, expected_u, 1e-12);
    EXPECT_NEAR(widest.dst_rect.u1, 1.0 - expected_u, 1e-12);
    const CompareResult padded = Compare(dst, widest.src_rect, src, widest.dst_rect, 129);
    EXPECT_LT(padded.max_deg, kExactDeg);

    // Non-overlapping frusta cannot be fixed by any crop.
    const RectMatchPlan none = PlanCrop(src, full, FovFromHalfAnglesDeg(60.0, 120.0, 50.0, -50.0),
                                        full, kSrcWidth, kSrcHeight);
    EXPECT_FALSE(none.possible);

    std::printf("[crop] %.3f px cropped per side, kept area %.6f, residual max %.3e deg\n",
                plan.crop_px[0], plan.kept_area_frac, cropped.max_deg);
    RecordProperty("crop_px_per_side", Px(plan.crop_px[0]));
    RecordProperty("crop_kept_area_frac", Deg(plan.kept_area_frac));
    RecordProperty("crop_residual_max_deg", Deg(cropped.max_deg));
}

// ---------------------------------------------------------------------------
// 4. Asymmetric left/right FOV
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, AsymmetricFov_CornerTangentsMatchHandComputation) {
    const FovTan fov = AsymmetricFov();
    const Rect full = FullRect();

    const double tl = std::tan(ToRadians(-40.0));
    const double tr = std::tan(ToRadians(50.0));
    const double tt = std::tan(ToRadians(20.0));
    const double tb = std::tan(ToRadians(-25.0));

    struct Corner {
        double u, v, tx, ty;
    };
    const std::array<Corner, 4> corners{
        {{0.0, 0.0, tl, tt}, {1.0, 0.0, tr, tt}, {0.0, 1.0, tl, tb}, {1.0, 1.0, tr, tb}}};
    for (const Corner& c : corners) {
        const Ray expected = RayFromTangent(c.tx, c.ty);
        const Ray via_uv = RayFromSourceUv(fov, full, c.u, c.v);
        const Ray via_ndc = RayFromXrFov(fov, 2.0 * c.u - 1.0, 1.0 - 2.0 * c.v);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(via_uv[k], expected[k], kAlgebraicTol)
                << "corner " << c.u << "," << c.v << " component " << k;
            EXPECT_NEAR(via_ndc[k], expected[k], kAlgebraicTol);
        }
        const auto back = UvFromRay(fov, full, expected);
        ASSERT_TRUE(back.has_value());
        EXPECT_NEAR(back->u, c.u, 1e-13);
        EXPECT_NEAR(back->v, c.v, 1e-13);
    }

    EXPECT_NEAR(TotalHorizontalDeg(fov), 90.0, 1e-12);
    EXPECT_NEAR(TotalVerticalDeg(fov), 45.0, 1e-12);
}

TEST(VrProjectionMath, AsymmetricFov_ImageCentreIsNotTheFrustumAxis) {
    const FovTan fov = AsymmetricFov();
    const Rect full = FullRect();
    const double l = std::tan(ToRadians(-40.0));
    const double r = std::tan(ToRadians(50.0));

    // Image column of the frustum's angular bisector, derived without BisectorU():
    // tan of the mean edge angle, mapped back through the tangent ramp.
    const double centre_angle = 0.5 * (ToRadians(-40.0) + ToRadians(50.0));
    const double expected_bisector = (std::tan(centre_angle) - l) / (r - l);
    EXPECT_NEAR(BisectorU(fov), expected_bisector, 1e-12);
    EXPECT_NEAR(expected_bisector, 0.4562, 1e-3);
    EXPECT_GT(std::abs(expected_bisector - 0.5), 0.04);

    // Image column of the view-space forward direction (tangent 0).
    const double expected_forward = -l / (r - l);
    EXPECT_NEAR(ForwardU(fov), expected_forward, 1e-12);
    EXPECT_GT(std::abs(expected_forward - 0.5), 0.08);

    const double bisector_px = (expected_bisector - 0.5) * kSrcWidth;
    const double forward_px = (expected_forward - 0.5) * kSrcWidth;

    // A resample that interpolates the half-angle instead of the tangent agrees with
    // the correct one only at the two rectangle edges (where both maps are pinned).
    // In between, its horizontal error field is not symmetric about the image centre:
    // the peak sits in the right half and its mirror column is almost error-free.
    double peak_err = 0.0, peak_u = 0.0;
    for (int i = 0; i <= 2048; ++i) {
        const double u = i / 2048.0;
        const double err = std::abs(AzimuthDeg(RayFromSourceUv(fov, full, u, 0.5)) -
                                    AzimuthDeg(RayFromSourceUvSpherical(fov, full, u, 0.5)));
        if (err > peak_err) {
            peak_err = err;
            peak_u = u;
        }
    }
    const double mirror_err =
        std::abs(AzimuthDeg(RayFromSourceUv(fov, full, 1.0 - peak_u, 0.5)) -
                 AzimuthDeg(RayFromSourceUvSpherical(fov, full, 1.0 - peak_u, 0.5)));
    EXPECT_GT(peak_u, 0.6);
    EXPECT_LT(peak_u, 0.8);
    EXPECT_GT(peak_err, 6.0);
    EXPECT_LT(mirror_err, 1.0);
    EXPECT_GT(peak_err / mirror_err, 10.0);

    std::printf("[asym] bisector u=%.6f (%.2f px off centre), forward u=%.6f (%.2f px); "
                "spherical error peak %.4f deg @u=%.4f vs %.4f deg at mirror u=%.4f\n",
                expected_bisector, bisector_px, expected_forward, forward_px, peak_err, peak_u,
                mirror_err, 1.0 - peak_u);
    RecordProperty("bisector_u", Deg(expected_bisector));
    RecordProperty("bisector_offset_px", Px(bisector_px));
    RecordProperty("forward_u", Deg(expected_forward));
    RecordProperty("spherical_err_peak_deg", Deg(peak_err));
    RecordProperty("spherical_err_mirror_deg", Deg(mirror_err));
    RecordProperty("spherical_err_peak_u", Deg(peak_u));
}

// ---------------------------------------------------------------------------
// 5. Symmetric render envelope vs asymmetric FOV
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, SameTotalFov_ShiftedEnvelopeIsADifferentRaySet) {
    const Rect full = FullRect();
    const FovTan symmetric = FovSymmetricDeg(100.0, 100.0);
    const FovTan shifted = FovFromHalfAnglesDeg(-55.0, 45.0, 50.0, -50.0);

    EXPECT_NEAR(TotalHorizontalDeg(symmetric), 100.0, 1e-12);
    EXPECT_NEAR(TotalHorizontalDeg(shifted), 100.0, 1e-12);
    EXPECT_FALSE(ResampleIsExact(symmetric, full, shifted, full));
    EXPECT_GT(MaxTangentDelta(FovOfRect(symmetric, full), FovOfRect(shifted, full)), 0.2);

    const CompareResult r = Compare(shifted, full, symmetric, full, 129);
    EXPECT_GT(r.max_deg, 11.5);
    EXPECT_LT(r.max_deg, 12.5);
    EXPECT_GT(r.worst_u, 0.45);
    EXPECT_LT(r.worst_u, 0.56);

    // Both edges are off by the same 5 deg, in the same direction: the shifted
    // envelope is smaller on the left and wider on the right, yet its edges both sit
    // 5 deg clockwise of the symmetric ones.
    EXPECT_NEAR(AzimuthDeg(RayFromSourceUv(shifted, full, 0.0, 0.5)) -
                    AzimuthDeg(RayFromXrFov(symmetric, -1.0, 0.0)),
                -5.0, 1e-9);
    EXPECT_NEAR(AzimuthDeg(RayFromSourceUv(shifted, full, 1.0, 0.5)) -
                    AzimuthDeg(RayFromXrFov(symmetric, 1.0, 0.0)),
                -5.0, 1e-9);

    std::printf("[shifted-envelope] max %.6f deg at u=%.4f, mean %.6f deg\n", r.max_deg, r.worst_u,
                r.mean_abs_deg);
    RecordProperty("shifted_envelope_max_deg", Deg(r.max_deg));
    RecordProperty("shifted_envelope_mean_deg", Deg(r.mean_abs_deg));
}

// ---------------------------------------------------------------------------
// 6. Per-eye UV offsets / source cropping
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, RectOffsetDoesNotChangeWhichRayAPixelRepresents) {
    const FovTan fov = EyeFov();
    const Rect full = FullRect();
    const Rect moved{0.05, 0.02, 1.0, 1.0};

    // A source pixel's ray is a property of the image, not of the window over it.
    for (int i = 0; i <= 32; ++i) {
        const double s = i / 32.0;
        const Ray via_full = RayFromSourceUv(fov, full, 0.05 + s * 0.95, 0.02 + s * 0.98);
        const Ray via_moved = RayFromSourceUv(fov, moved, 0.05 + s * 0.95, 0.02 + s * 0.98);
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(via_full[k], via_moved[k], kAlgebraicTol);
        }
    }

    // Shifting the window by a pure translation (same width and height) translates
    // the whole ray fan by a constant tangent offset equal to the shift times the
    // image's tangent span.
    const Rect shifted{0.05, 0.02, 1.05, 1.02};
    const double expected_du = (shifted.u0 - full.u0) * (fov.r - fov.l);
    const double expected_dv = (shifted.v0 - full.v0) * (fov.b - fov.t);
    for (int i = 0; i <= 32; ++i) {
        const double s = i / 32.0;
        const Ray a = RayFromSourceUv(fov, full, s, s);
        const Ray b = RayFromSourceUv(fov, shifted, shifted.u0 + s * RectWidth(shifted),
                                      shifted.v0 + s * RectHeight(shifted));
        EXPECT_NEAR(b[0] / -b[2] - a[0] / -a[2], expected_du, kAlgebraicTol);
        EXPECT_NEAR(b[1] / -b[2] - a[1] / -a[2], expected_dv, kAlgebraicTol);
    }
    EXPECT_NEAR(expected_du, 0.05 * 2.0 * std::tan(ToRadians(50.0)), 1e-12);
    // The rect-relative midpoint stays the rect-relative midpoint whatever the window.
}

TEST(VrProjectionMath, NaiveResampleOfCroppedSourceShiftsTheFanHorizontally) {
    // Two eyes with identical FOVs. Each guest image is cropped differently (left
    // eye keeps the right 95%, right eye keeps the left 95%) and the crop is then
    // stretched back over the full destination view. The stretch turns the crop into
    // a horizontal shift of the whole ray fan.
    const FovTan fov = EyeFov();
    const Rect full = FullRect();
    const Rect left_src{0.05, 0.0, 1.0, 1.0};
    const Rect right_src{0.0, 0.0, 0.95, 1.0};

    // Straight ahead is tangent 0, which lives at source u = 0.5.
    const double u_left = DstUForSourceTangent(fov, left_src, full, 0.0);
    const double u_right = DstUForSourceTangent(fov, right_src, full, 0.0);
    EXPECT_NEAR(u_left, 0.4736842105263158, 1e-12);
    EXPECT_NEAR(u_right, 0.5263157894736842, 1e-12);

    const double disparity_px = (u_right - u_left) * kDstWidth;
    EXPECT_NEAR(disparity_px, 159.7894736842105, 1e-9);
    EXPECT_GT(std::abs(u_left - 0.5), 0.026);
    EXPECT_GT(std::abs(u_right - 0.5), 0.026);

    // The rays those columns really represent are still straight ahead: the geometry
    // is preserved for content, only its screen position moved.
    const Uv uv_l = ResampleSourceUvForDest(fov, left_src, 2.0 * u_left - 1.0, 0.0, full);
    const Uv uv_r = ResampleSourceUvForDest(fov, right_src, 2.0 * u_right - 1.0, 0.0, full);
    EXPECT_LT(std::abs(RayFromSourceUv(fov, left_src, uv_l.u, uv_l.v)[0]), 1e-15);
    EXPECT_LT(std::abs(RayFromSourceUv(fov, right_src, uv_r.u, uv_r.v)[0]), 1e-15);

    // Not cropping at all keeps both eyes on 0.5, i.e. zero disparity.
    EXPECT_NEAR(DstUForSourceTangent(fov, full, full, 0.0), 0.5, 1e-15);

    std::printf("[stereo-crop] straight ahead -> u_left %.6f, u_right %.6f, %.3f px disparity\n",
                u_left, u_right, disparity_px);
    RecordProperty("crop_shift_left_u", Deg(u_left));
    RecordProperty("crop_shift_right_u", Deg(u_right));
    RecordProperty("crop_shift_disparity_px", Px(disparity_px));
}

// ---------------------------------------------------------------------------
// 7. Stereo
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, Stereo_PointsAtInfinityHaveZeroDisparity) {
    EyeProjection left{};
    left.fov = EyeFov();
    left.offset_x_m = -0.5 * kIpdM;
    EyeProjection right{};
    right.fov = EyeFov();
    right.offset_x_m = 0.5 * kIpdM;

    // Infinity is a direction, identical for both eyes.
    const Ray d = Normalized({0.10, 0.05, -1.0});
    const auto uv_l = UvFromRay(left.fov, FullRect(), d);
    const auto uv_r = UvFromRay(right.fov, FullRect(), d);
    ASSERT_TRUE(uv_l.has_value());
    ASSERT_TRUE(uv_r.has_value());
    EXPECT_NEAR(uv_l->u, uv_r->u, 1e-15);
    EXPECT_NEAR(uv_l->v, uv_r->v, 1e-15);
    EXPECT_NEAR((uv_r->u - uv_l->u) * kDstWidth, 0.0, 1e-12);
}

TEST(VrProjectionMath, Stereo_FiniteDepthDisparityMatchesClosedForm) {
    const FovTan fov = EyeFov();
    EyeProjection left{};
    left.fov = fov;
    left.offset_x_m = -0.5 * kIpdM;
    EyeProjection right{};
    right.fov = fov;
    right.offset_x_m = 0.5 * kIpdM;

    const double tan_half = std::tan(ToRadians(50.0));
    for (const double depth : {0.5, 1.0, 2.0, 10.0}) {
        // Point on the optical axis, `depth` metres in front of the head.
        const auto uv_l = EyeUvOfPoint(left, 0.0, 0.0, -depth);
        const auto uv_r = EyeUvOfPoint(right, 0.0, 0.0, -depth);
        ASSERT_TRUE(uv_l.has_value());
        ASSERT_TRUE(uv_r.has_value());

        // Crossed disparity: u_R - u_L = -IPD / (2 tan(h/2) depth).
        const double expected_du = -kIpdM / (2.0 * tan_half * depth);
        EXPECT_NEAR(uv_r->u - uv_l->u, expected_du, 1e-15) << "depth " << depth;
        EXPECT_NEAR((uv_r->u - uv_l->u) * kDstWidth, expected_du * kDstWidth, 1e-12);
        EXPECT_NEAR(uv_l->u + uv_r->u, 1.0, 1e-15);
        EXPECT_NEAR(uv_l->v, uv_r->v, 1e-15);

        // The same disparity expressed as angular parallax: each eye's azimuth is
        // atan(IPD / (2 depth)), so the angular separation is twice that.
        const Ray dl = EyeRayDirection(left, 2.0 * uv_l->u - 1.0, 1.0 - 2.0 * uv_l->v);
        const Ray dr = EyeRayDirection(right, 2.0 * uv_r->u - 1.0, 1.0 - 2.0 * uv_r->v);
        const double expected_deg = 2.0 * ToDegrees(std::atan(0.5 * kIpdM / depth));
        EXPECT_NEAR(std::abs(AzimuthDeg(dr) - AzimuthDeg(dl)), expected_deg, 1e-9)
            << "depth " << depth;
    }

    std::printf("[ipd] %.3f px disparity at 0.5 m, %.3f px at 1 m, %.3f px at 2 m\n",
                (-kIpdM / (2.0 * tan_half * 0.5)) * kDstWidth,
                (-kIpdM / (2.0 * tan_half * 1.0)) * kDstWidth,
                (-kIpdM / (2.0 * tan_half * 2.0)) * kDstWidth);
    RecordProperty("ipd_disparity_px_at_1m", Px((-kIpdM / (2.0 * tan_half * 1.0)) * kDstWidth));
}

TEST(VrProjectionMath, Stereo_AngularCircleBecomesAnEllipse) {
    // A cone of directions at constant angle from the view axis is exactly a circle
    // of radius tan(gamma) in tangent space, so its image extents are fixed purely by
    // the tangent spans of the two axes.
    const FovTan aniso = FovSymmetricDeg(100.0, 80.0);
    const Rect full = FullRect();
    const double gamma = ToRadians(10.0);

    double min_u = 1e9, max_u = -1e9, min_v = 1e9, max_v = -1e9;
    for (int k = 0; k < 64; ++k) {
        const double phi = 2.0 * kPi * k / 64.0;
        const Ray d{std::sin(gamma) * std::cos(phi), std::sin(gamma) * std::sin(phi),
                    -std::cos(gamma)};
        const auto uv = UvFromRay(aniso, full, d);
        ASSERT_TRUE(uv.has_value());
        min_u = std::min(min_u, uv->u);
        max_u = std::max(max_u, uv->u);
        min_v = std::min(min_v, uv->v);
        max_v = std::max(max_v, uv->v);
    }
    const double width_u = max_u - min_u;
    const double height_v = max_v - min_v;
    const FovTan f = FovOfRect(aniso, full);
    const double predicted = (f.r - f.l) / (f.t - f.b);
    EXPECT_NEAR(height_v / width_u, predicted, 1e-12);
    EXPECT_NEAR(predicted, 1.4203, 1e-4);
    EXPECT_GT(std::abs(height_v / width_u - 1.0), 0.4); // a square-UV test would say 1.0

    // A symmetric frustum centres the ellipse; an asymmetric one does not, because the
    // forward direction is not the image centre.
    EXPECT_NEAR(0.5 * (min_u + max_u), 0.5, 1e-12);
    const FovTan asym = AsymmetricFov();
    const auto centre_uv = UvFromRay(asym, full, Ray{0.0, 0.0, -1.0});
    ASSERT_TRUE(centre_uv.has_value());
    EXPECT_NEAR(centre_uv->u, -asym.l / (asym.r - asym.l), 1e-15);
    EXPECT_GT(std::abs(centre_uv->u - 0.5), 0.08);

    std::printf("[ellipse] angular circle of 10 deg -> %.6f:1 ellipse (predicted %.6f), centre "
                "u=%.6f under asymmetric fov\n",
                height_v / width_u, predicted, centre_uv->u);
    RecordProperty("angular_circle_axis_ratio", Deg(height_v / width_u));
}

TEST(VrProjectionMath, Stereo_BinocularOverlapUsesEachEyesOwnFov) {
    // Same total horizontal FOV, but the right eye's envelope is rotated 10 deg right,
    // which the runtime reports as an asymmetric per-eye fov.
    EyeProjection left{};
    left.fov = FovFromHalfAnglesDeg(-50.0, 50.0, 50.0, -50.0);
    left.offset_x_m = -0.5 * kIpdM;
    EyeProjection right{};
    right.fov = FovFromHalfAnglesDeg(-40.0, 60.0, 50.0, -50.0);
    right.offset_x_m = 0.5 * kIpdM;

    const Ray forward{0.0, 0.0, -1.0};
    const auto uv_l = UvFromRay(left.fov, FullRect(), forward);
    const auto uv_r = UvFromRay(right.fov, FullRect(), forward);
    ASSERT_TRUE(uv_l.has_value());
    ASSERT_TRUE(uv_r.has_value());
    EXPECT_NEAR(uv_l->u, 0.5, 1e-15);

    const FovTan fr = FovOfRect(right.fov, FullRect());
    EXPECT_NEAR(uv_r->u, (0.0 - fr.l) / (fr.r - fr.l), 1e-15);
    EXPECT_NEAR(uv_r->u, 0.3263518, 1e-6);

    // Angular correspondence is exact: both columns decode to straight ahead, even
    // though the two columns are nowhere near mirrored.
    EXPECT_NEAR(AngleBetweenDeg(RayFromSourceUv(left.fov, FullRect(), uv_l->u, uv_l->v), forward),
                0.0, 1e-9);
    EXPECT_NEAR(AngleBetweenDeg(RayFromSourceUv(right.fov, FullRect(), uv_r->u, uv_r->v), forward),
                0.0, 1e-9);
    EXPECT_GT(std::abs(uv_r->u - (1.0 - uv_l->u)), 0.17);

    // Mirrored UVs would be wrong by a large margin here: they claim the right eye's
    // straight-ahead column sits at 0.5, whose ray is +24 deg.
    const Ray mirrored_ray = RayFromSourceUv(right.fov, FullRect(), 1.0 - uv_l->u, 0.5);
    EXPECT_GT(AngleBetweenDeg(mirrored_ray, forward), 20.0);
    EXPECT_NEAR(AzimuthDeg(mirrored_ray), 24.0596, 1e-3);

    // Binocular overlap follows from the tangent intervals, not from the UV box.
    const FovTan fl = FovOfRect(left.fov, FullRect());
    const double overlap_deg =
        ToDegrees(std::atan(std::min(fl.r, fr.r)) - std::atan(std::max(fl.l, fr.l)));
    EXPECT_NEAR(overlap_deg, 90.0, 1e-9);

    std::printf("[overlap] forward at u=%.6f (left) / %.6f (right); mirrored-uv error %.4f deg; "
                "overlap %.3f deg\n",
                uv_l->u, uv_r->u, AngleBetweenDeg(mirrored_ray, forward), overlap_deg);
    RecordProperty("per_eye_offset_mirrored_error_deg",
                   Deg(AngleBetweenDeg(mirrored_ray, forward)));
    RecordProperty("overlap_deg", Deg(overlap_deg));
}

// ---------------------------------------------------------------------------
// 8. Grid / line / ray patterns and matched objects
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, GridOfRaysRoundTripsExactly) {
    const FovTan fov = AsymmetricFov();
    const Rect window{0.1, 0.2, 0.9, 0.8};

    for (int j = 0; j <= 16; ++j) {
        for (int i = 0; i <= 16; ++i) {
            const double u = window.u0 + RectWidth(window) * i / 16.0;
            const double v = window.v0 + RectHeight(window) * j / 16.0;
            const Ray ray = RayFromSourceUv(fov, window, u, v);
            const auto back = UvFromRay(fov, window, ray);
            ASSERT_TRUE(back.has_value());
            EXPECT_NEAR(back->u, u, 1e-13);
            EXPECT_NEAR(back->v, v, 1e-13);

            // ndc -> ray agrees with uv -> ray.
            EXPECT_NEAR(AngleBetweenDeg(RayFromXrFov(fov, 2.0 * u - 1.0, 1.0 - 2.0 * v), ray), 0.0,
                        1e-9);
        }
    }

    // A ray behind the eye has no image position.
    EXPECT_FALSE(UvFromRay(fov, window, Ray{0.0, 0.0, 1.0}).has_value());
}

TEST(VrProjectionMath, StraightLinesInAngleSpaceMapToStraightLinesInUv) {
    const FovTan fov = AsymmetricFov();
    const Rect full = FullRect();

    // (a) A line drawn in the projection plane at unit depth is linear in the
    // parameter, and UV is affine in the tangent, so UV is linear in the parameter:
    // the midpoint must map to the midpoint and three points stay collinear.
    const Uv p0{0.22, 0.30};
    const Uv p1{0.71, 0.64};
    const auto mid = UvFromRay(
        fov, full, RayFromTangent(TanX(fov, 0.5 * (p0.u + p1.u)), TanY(fov, 0.5 * (p0.v + p1.v))));
    ASSERT_TRUE(mid.has_value());
    EXPECT_NEAR(mid->u, 0.5 * (p0.u + p1.u), 1e-14);
    EXPECT_NEAR(mid->v, 0.5 * (p0.v + p1.v), 1e-14);

    // (b) A real 3D line projects to a straight image line, but its parameter is
    // rational (x/(-z)), so the parametric midpoint does not land at the chord
    // midpoint even though all three points are collinear.
    auto uv_of = [&](double t) -> Uv {
        const Ray d = Normalized({-0.3 + 0.6 * t, 0.1 - 0.2 * t, -1.0 - 0.5 * t});
        const auto uv = UvFromRay(fov, full, d);
        EXPECT_TRUE(uv.has_value());
        return uv.value_or(Uv{});
    };
    const Uv a = uv_of(0.0);
    const Uv b = uv_of(0.5);
    const Uv c = uv_of(1.0);
    const double area = (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
    EXPECT_NEAR(area, 0.0, 1e-14);
    EXPECT_GT(std::abs(b.u - 0.5 * (a.u + c.u)), 1e-3);

    // (c) The angle-interpolating variant breaks collinearity outright, which is why
    // it cannot be used for a perspective resample.
    auto uv_spherical = [&](double t) {
        const Ray d = Normalized({-0.3 + 0.6 * t, 0.1 - 0.2 * t, -1.0 - 0.5 * t});
        const double tx = d[0] / -d[2];
        const double ty = d[1] / -d[2];
        // Invert the image position from the tangent, then rebuild it the wrong way.
        const double u_t = UFromTanX(fov, tx);
        const double v_t = VFromTanY(fov, ty);
        const Ray wrong = RayFromSourceUvSpherical(fov, full, u_t, v_t);
        const auto uv = UvFromRay(fov, full, wrong);
        return uv.value_or(Uv{});
    };
    const Uv sa = uv_spherical(0.0);
    const Uv sb = uv_spherical(0.5);
    const Uv sc = uv_spherical(1.0);
    const double spherical_area = (sb.u - sa.u) * (sc.v - sa.v) - (sb.v - sa.v) * (sc.u - sa.u);
    EXPECT_GT(std::abs(spherical_area), 1e-6);

    std::printf("[lines] tangent-space line deviation %.3e, spherical deviation %.3e\n", area,
                spherical_area);
    RecordProperty("line_deviation_tangent", Deg(area));
    RecordProperty("line_deviation_spherical", Deg(spherical_area));
}

TEST(VrProjectionMath, MatchedObjectsAtSeveralDepths) {
    const FovTan fov = EyeFov();
    EyeProjection left{};
    left.fov = fov;
    left.offset_x_m = -0.5 * kIpdM;
    EyeProjection right{};
    right.fov = fov;
    right.offset_x_m = 0.5 * kIpdM;
    const double tan_half = std::tan(ToRadians(50.0));

    struct Object {
        double x, y, z;
    };
    const std::array<Object, 5> objects{{{0.0, 0.0, -0.5},
                                         {0.0, 0.0, -1.0},
                                         {0.30, 0.10, -2.0},
                                         {-0.20, -0.15, -10.0},
                                         {0.0, 0.0, -1000.0}}};
    for (const Object& o : objects) {
        const auto uv_l = EyeUvOfPoint(left, o.x, o.y, o.z);
        const auto uv_r = EyeUvOfPoint(right, o.x, o.y, o.z);
        ASSERT_TRUE(uv_l.has_value());
        ASSERT_TRUE(uv_r.has_value());

        const double depth = -o.z;
        // Closed form for parallel eyes with identical frusta: the vertical
        // coordinate is untouched, the horizontal one shifts by -IPD/(2 tan(h/2) D).
        EXPECT_NEAR(uv_r->u - uv_l->u, -kIpdM / (2.0 * tan_half * depth), 1e-15) << "z=" << o.z;
        EXPECT_NEAR(uv_r->v - uv_l->v, 0.0, 1e-15) << "z=" << o.z;

        // The same disparity as angular parallax between the two eye-origin rays.
        const Ray dl = EyeRayDirection(left, 2.0 * uv_l->u - 1.0, 1.0 - 2.0 * uv_l->v);
        const Ray dr = EyeRayDirection(right, 2.0 * uv_r->u - 1.0, 1.0 - 2.0 * uv_r->v);
        const double expected_deg = ToDegrees(std::atan((o.x + 0.5 * kIpdM) / depth)) -
                                    ToDegrees(std::atan((o.x - 0.5 * kIpdM) / depth));
        EXPECT_NEAR(std::abs(AzimuthDeg(dr) - AzimuthDeg(dl)), std::abs(expected_deg), 1e-9)
            << "z=" << o.z;
    }
}

// ---------------------------------------------------------------------------
// Worst-case numbers for the report
// ---------------------------------------------------------------------------

TEST(VrProjectionMath, WorstCaseAngularErrorSummary) {
    const Rect full = FullRect();
    const FovTan fov100 = EyeFov();
    const FovTan fov90h = FovSymmetricDeg(90.0, 100.0);
    const FovTan shifted = FovFromHalfAnglesDeg(-55.0, 45.0, 50.0, -50.0);
    const double half_gap = 0.5 * (1.0 - 8.0 / 9.0);
    const Rect pillarbox{half_gap, 0.0, 1.0 - half_gap, 1.0};

    const RectMatchPlan crop = PlanCrop(fov100, full, fov90h, full, kSrcWidth, kSrcHeight);
    ASSERT_TRUE(crop.possible);

    struct Scenario {
        const char* name;
        CompareResult result;
        const char* kept;
    };
    const std::array<Scenario, 6> scenarios{{
        {"identity, matching fov, full rect", Compare(fov100, full, fov100, full, 129), "1.000"},
        {"8:9 to 1:1, matching fov", Compare(fov100, full, fov100, full, 129), "1.000"},
        {"8:9 to 1:1, pillarboxed", Compare(fov100, full, fov100, pillarbox, 129), "0.889"},
        {"fov mismatch, dst 10 deg narrower", Compare(fov100, full, fov90h, full, 129), "1.000"},
        {"fov mismatch after minimal crop",
         Compare(fov100, crop.src_rect, fov90h, crop.dst_rect, 129), "0.839"},
        {"same total fov, shifted envelope", Compare(shifted, full, fov100, full, 129), "1.000"},
    }};

    std::printf("\n%-38s %14s %14s %10s\n", "scenario", "max err (deg)", "mean err (deg)",
                "kept area");
    for (const Scenario& s : scenarios) {
        std::printf("%-38s %14.6f %14.6f %10s\n", s.name, s.result.max_deg, s.result.mean_abs_deg,
                    s.kept);
        RecordProperty(Key("max_deg_", s.name), Deg(s.result.max_deg));
        RecordProperty(Key("mean_deg_", s.name), Deg(s.result.mean_abs_deg));
    }

    // Exact where the frusta agree, visibly not exact everywhere else.
    EXPECT_LT(scenarios[0].result.max_deg, kExactDeg);
    EXPECT_LT(scenarios[1].result.max_deg, kExactDeg);
    EXPECT_GT(scenarios[2].result.max_deg, 3.0);
    EXPECT_GT(scenarios[3].result.max_deg, 5.0);
    EXPECT_LT(scenarios[4].result.max_deg, kExactDeg);
    EXPECT_GT(scenarios[5].result.max_deg, 11.5);
}

// ---------------------------------------------------------------------------
// The guest states the frustum an eye image covers only through the reprojection
// UVs it hands over: uv = tan * scale + offset, measured in the texture that eye
// samples. Turning those back into half-angle tangents therefore needs to know
// where the eye's own picture ends inside that texture - halfway for the two
// halves of a side-by-side picture, at the end for a picture an eye has to itself.
// The numbers are the live session's (pc-vr/user/log/shad_log.txt, 2026-11-17).
// ---------------------------------------------------------------------------

struct GuestUv {
    double scale_x;
    double scale_y;
    double offset_x;
    double offset_y;
};

// What the guest passes for the left eye when its picture covers `span` of the
// texture's width (0.5 side-by-side, 1.0 a picture of its own) and all of its height.
GuestUv LeftEyeUv(const FovTan& fov, double span) {
    const double width = -fov.l + fov.r;
    const double height = fov.t - fov.b;
    return GuestUv{
        .scale_x = span / width,
        .scale_y = 1.0 / height,
        .offset_x = span * -fov.l / width,
        .offset_y = fov.t / height,
    };
}

// uv = tan * scale + offset inverted at the far edge of the eye's own picture,
// which is what Hmd::FovFromUv does once it is told where that edge is.
FovTan FovFromGuestUv(const GuestUv& uv, double nose_edge) {
    return FovTan{
        .l = -uv.offset_x / uv.scale_x,
        .r = (nose_edge - uv.offset_x) / uv.scale_x,
        .t = uv.offset_y / uv.scale_y,
        .b = -(1.0 - uv.offset_y) / uv.scale_y,
    };
}

TEST(VrProjectionMath, GuestUv_SideBySidePictureRoundTripsExactly) {
    // PlayStation VR's own field of view, the values the title is asked to draw for.
    const FovTan psvr = FovFromHalfAnglesDeg(-50.368309, 49.752363, 51.626199, -51.626199);
    const GuestUv uv = LeftEyeUv(psvr, 0.5);

    // The live session's UVs, to the digits the log prints.
    EXPECT_NEAR(uv.scale_x, 0.209312, 1e-6);
    EXPECT_NEAR(uv.scale_y, 0.395923, 1e-6);
    EXPECT_NEAR(uv.offset_x, 0.252730, 1e-6);
    EXPECT_NEAR(uv.offset_y, 0.500000, 1e-9);

    const FovTan recovered = FovFromGuestUv(uv, 0.5);
    EXPECT_LT(MaxTangentDelta(recovered, psvr), 1e-5);
    // The right edge of that picture is the middle of the side-by-side image.
    EXPECT_NEAR(uv.offset_x + recovered.r * uv.scale_x, 0.5, kAlgebraicTol);
    EXPECT_NEAR(uv.offset_x + recovered.l * uv.scale_x, 0.0, kAlgebraicTol);
}

TEST(VrProjectionMath, GuestUv_MagnitudeHeuristicBreaksOnlyBelowSeventyOneDegrees) {
    // The rule this replaces took U = 0.5 as the eye's far edge only while
    // offset_x < 0.5 and scale_x < 0.35, and scale_x = 0.5/(tan_out+tan_in) for a
    // side-by-side pair: it holds while the pair spans more than 1/0.7 tangents,
    // about 71 degrees, and doubles the nose side below that.
    struct Case {
        double total_deg;
        bool heuristic_ok;
    };
    for (const Case c : {Case{100.0, true}, Case{80.0, true}, Case{71.0, false}, Case{60.0, false},
                         Case{45.0, false}}) {
        const FovTan fov = FovSymmetricDeg(c.total_deg, 100.0);
        const GuestUv uv = LeftEyeUv(fov, 0.5);
        const bool half = uv.offset_x < 0.5 && uv.scale_x < 0.35;
        const FovTan recovered = FovFromGuestUv(uv, half ? 0.5 : 1.0);
        const FovTan exact = FovFromGuestUv(uv, 0.5);

        EXPECT_EQ(half, c.heuristic_ok) << "total " << c.total_deg;
        EXPECT_LT(MaxTangentDelta(exact, fov), 1e-12) << "total " << c.total_deg;
        if (c.heuristic_ok) {
            EXPECT_LT(MaxTangentDelta(recovered, fov), 1e-12) << "total " << c.total_deg;
        } else {
            // Not a rounding error: the nose-side tangent more than doubles.
            EXPECT_GT(recovered.r - fov.r, 0.8) << "total " << c.total_deg;
        }
    }
}

TEST(VrProjectionMath, GuestUv_PictureOfItsOwnEndsAtTheEndOfTheTexture) {
    const FovTan fov = FovFromHalfAnglesDeg(-55.0, 45.0, 40.0, -35.0);
    const GuestUv uv = LeftEyeUv(fov, 1.0);
    EXPECT_LT(MaxTangentDelta(FovFromGuestUv(uv, 1.0), fov), 1e-12);
    // Reading it as the half of a side-by-side picture would put the nose side in
    // the middle of the eye's own image, i.e. inside out or empty.
    EXPECT_LT(FovFromGuestUv(uv, 0.5).r, 0.0);
}

TEST(VrProjectionMath, GuestUv_PackingIsDecidedByTheTwoOffsetsNotByMagnitudes) {
    const FovTan fov = FovFromHalfAnglesDeg(-50.0, 50.0, 50.0, -50.0);
    const GuestUv left = LeftEyeUv(fov, 0.5);
    // Right eye of a side-by-side picture: same span, offset mirrored about the middle.
    const double right_sbs = 1.0 - left.offset_x;
    // Both eyes sampling the same whole picture: the same offset twice.
    const double right_same = left.offset_x;
    const auto halves = [&](double right_offset) {
        return std::abs(right_offset - (1.0 - left.offset_x)) <
               std::abs(right_offset - left.offset_x);
    };
    EXPECT_TRUE(halves(right_sbs));
    EXPECT_FALSE(halves(right_same));
    // The two readings are half a texture apart, far outside any measurement noise.
    EXPECT_NEAR(std::abs(right_sbs - right_same), 0.5, kAlgebraicTol);
}

TEST(VrProjectionMath, LiveConfiguration_TodayAndAtTheRuntimesRecommendation) {
    // PlayStation VR's field of view, asymmetric horizontally and symmetric vertically:
    // 50.368 / 49.752 / 51.626 / 51.626 degrees (out, in, up, down).
    const FovTan psvr = FovFromHalfAnglesDeg(-50.368309, 49.752363, 51.626199, -51.626199);
    // What the runtime recommends to an eye on this machine, from the same session's log.
    const FovTan headset = FovFromHalfAnglesDeg(-59.0, 50.7, 49.8, -59.9);
    const Rect full = FullRect();

    // A full-rect resample is exact whenever the rect is shown with the frustum the guest
    // drew for. The rect's pixel count is not a parameter of that geometry at all, so today's
    // 2880x3248 an eye (the source height rounded up to a multiple of 16) and the runtime's
    // square 3036x3036 describe the same rays: exactly one assertion covers both.
    EXPECT_LT(Compare(psvr, full, psvr, full, 129).max_deg, kExactDeg);

    // Declaring the headset's own frustum instead - the mistake of taking the runtime's
    // recommended size for its recommended field of view - is a real distortion, largest
    // where the two frusta differ most (the top and bottom edges here).
    const CompareResult mismatch = Compare(psvr, full, headset, full, 129);
    RecordProperty("headset_fov_max_deg", Deg(mismatch.max_deg));
    EXPECT_GT(mismatch.max_deg, 5.0);

    // What the 1:1 recommendation does cost is sampling density, and only that: 8:9 source
    // pixels spread over a square rect gain 3036/2880 horizontally and lose 3036/3240
    // vertically. Their ratio is exactly the source aspect, 1.125.
    const double horizontal = static_cast<double>(kDstWidth) / kSrcWidth;
    const double vertical = static_cast<double>(kDstWidth) / kSrcHeight;
    EXPECT_NEAR(horizontal / vertical, static_cast<double>(kSrcHeight) / kSrcWidth, kAlgebraicTol);
}

// The layout decision the emulator applies to a frame's UVs: the two eyes' offsets
// say whether they own half of one shared picture or a picture each. Offsets that do
// not discriminate, or that contradict the textures, are flagged instead of assumed,
// because reading the layout wrongly moves the nose side of the frustum by degrees.
struct LayoutInput {
    bool shared_image;
    double left_offset;
    double right_offset;
};

struct LayoutDecision {
    bool halves;
    bool ambiguous;
};

LayoutDecision DecideLayout(const LayoutInput& in) {
    constexpr double kClose = 0.02; // 2 % of a texture's width
    const double mirrored = std::abs(in.right_offset - (1.0 - in.left_offset));
    const double same = std::abs(in.right_offset - in.left_offset);
    const bool offsets_say_halves = mirrored < same;
    const bool offsets_fit_neither = same >= kClose && mirrored >= kClose;
    return {
        .halves = in.shared_image && offsets_say_halves,
        .ambiguous = offsets_fit_neither || (!in.shared_image && offsets_say_halves),
    };
}

TEST(VrProjectionMath, GuestUv_LayoutDecisionTable) {
    struct Case {
        const char* name;
        LayoutInput input;
        bool halves;
        bool ambiguous;
    };
    const Case cases[]{
        {"live Deracine, one image, mirrored offsets", {true, 0.252730, 0.747270}, true, false},
        {"one image, both eyes on the whole picture", {true, 0.506, 0.506}, false, false},
        {"a texture each, whole-texture UVs", {false, 0.506, 0.506}, false, false},
        {"a texture each, yet mirrored offsets", {false, 0.2527, 0.7473}, false, true},
        {"one image, offsets fitting neither reading", {true, 0.30, 0.55}, true, true},
        {"one image, both eyes on its middle", {true, 0.5, 0.5}, false, false},
    };
    for (const Case& c : cases) {
        const LayoutDecision d = DecideLayout(c.input);
        EXPECT_EQ(d.halves, c.halves) << c.name;
        EXPECT_EQ(d.ambiguous, c.ambiguous) << c.name;
    }
}

TEST(VrProjectionMath, GuestUv_ReadingTheLiveLayoutAsWholePicturesIsASixFigureMiss) {
    const FovTan psvr = FovFromHalfAnglesDeg(-50.368309, 49.752363, 51.626199, -51.626199);
    const GuestUv uv = LeftEyeUv(psvr, 0.5);
    const FovTan halves = FovFromGuestUv(uv, 0.5);
    const FovTan whole = FovFromGuestUv(uv, 1.0);
    EXPECT_NEAR(ToDegrees(std::atan(halves.r)), 49.752363, 1e-3);
    EXPECT_NEAR(ToDegrees(std::atan(whole.r)), 74.34, 0.05);
    // 24.6 degrees of nose-side field of view, and the horizontal scale with it.
    EXPECT_GT(ToDegrees(std::atan(whole.r)) - ToDegrees(std::atan(halves.r)), 24.0);
}

} // namespace
