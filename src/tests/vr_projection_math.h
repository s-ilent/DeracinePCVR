// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Projection geometry for a guest eye image that is composited through an OpenXR
// projection layer (XrCompositionLayerProjection + XrSwapchainSubImage).
//
// The whole point of this header is that a "resample source rectangle into the
// destination view" step carries no FOV information at all. Whether it is
// geometrically correct is decided entirely by whether the frustum covered by the
// source rectangle equals the frustum claimed by the destination view. Nothing in
// here depends on pixel aspect ratio, image size, or the emulator.
//
// Conventions (every function in this file follows them):
//
//  * View space is OpenXR view space: +X right, +Y up, -Z forward. A ray direction
//    that hits the projection plane at 1 unit of depth is (tx, ty, -1), where tx/ty
//    are the tangents of the horizontal/vertical angles.
//  * FovTan holds the *tangent of the four half-angles* of a frustum, in the same
//    left/right/up/down order as XrFovf and with the same signs:
//        l = tan(angleLeft)  < 0
//        r = tan(angleRight) > 0
//        t = tan(angleUp)    > 0
//        b = tan(angleDown)  < 0
//    Storing tangents instead of angles is what makes the entire mapping affine,
//    and therefore exactly separable per axis.
//  * Rect is a UV window over an image whose origin is the image's top-left corner
//    and whose v axis grows downwards (Vulkan/OpenGL texture convention).
//  * A FovTan is always the frustum covered by a *whole* [0,1]^2 UV square. A Rect
//    selects a sub-frustum of it, and because tangents interpolate linearly with UV
//    that sub-frustum is simply FovOfRect(full_fov, rect).
//
// Why tangents and not angles: a perspective projection divides by depth, so screen
// position is an affine function of the tangent of the viewing angle, not of the
// angle itself. Interpolating the half-angle linearly across the image (a
// "spherical" resample) bends straight world lines into curves and is only correct
// in the infinitesimal limit. See RayFromSourceUvSpherical() for the wrong version,
// it is kept because several tests measure how wrong it is.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace VrProj {

inline constexpr double kPi = 3.14159265358979323846;

inline constexpr double ToRadians(double deg) noexcept {
    return deg * (kPi / 180.0);
}
inline constexpr double ToDegrees(double rad) noexcept {
    return rad * (180.0 / kPi);
}

// Tangent of the four half-angle bounds, XrFovf sign convention. Invariants that
// every helper below relies on: l < r and b < t.
struct FovTan {
    double l = -1.0;
    double r = 1.0;
    double t = 1.0;
    double b = -1.0;
};

// UV window, v downwards from the top-left of the image.
struct Rect {
    double u0 = 0.0;
    double v0 = 0.0;
    double u1 = 1.0;
    double v1 = 1.0;
};

using Ray = std::array<double, 3>;

struct Uv {
    double u = 0.0;
    double v = 0.0;
};

// ---------------------------------------------------------------------------
// Frustum construction / inspection
// ---------------------------------------------------------------------------

// Frustum from the four half-angles in degrees, signed like XrFovf.
inline FovTan FovFromHalfAnglesDeg(double left_deg, double right_deg, double up_deg,
                                   double down_deg) {
    return {std::tan(ToRadians(left_deg)), std::tan(ToRadians(right_deg)),
            std::tan(ToRadians(up_deg)), std::tan(ToRadians(down_deg))};
}

inline FovTan FovSymmetricDeg(double horizontal_total_deg, double vertical_total_deg) {
    return FovFromHalfAnglesDeg(-0.5 * horizontal_total_deg, 0.5 * horizontal_total_deg,
                                0.5 * vertical_total_deg, -0.5 * vertical_total_deg);
}

inline double TotalHorizontalDeg(const FovTan& f) noexcept {
    return ToDegrees(std::atan(f.r) - std::atan(f.l));
}
inline double TotalVerticalDeg(const FovTan& f) noexcept {
    return ToDegrees(std::atan(f.t) - std::atan(f.b));
}

inline Rect FullRect() noexcept {
    return {0.0, 0.0, 1.0, 1.0};
}
inline bool IsValidRect(const Rect& r) noexcept {
    return r.u1 > r.u0 && r.v1 > r.v0;
}

inline double RectWidth(const Rect& r) noexcept {
    return r.u1 - r.u0;
}
inline double RectHeight(const Rect& r) noexcept {
    return r.v1 - r.v0;
}

// Tangent of the horizontal angle at horizontal image coordinate u. Affine in u,
// which is exactly the perspective relationship between screen x and tangent.
inline double TanX(const FovTan& f, double u) noexcept {
    return f.l + u * (f.r - f.l);
}
// Tangent of the vertical angle at vertical image coordinate v (v down). Affine in
// v, running from t at v = 0 down to b at v = 1.
inline double TanY(const FovTan& f, double v) noexcept {
    return f.t + v * (f.b - f.t);
}
inline double UFromTanX(const FovTan& f, double tx) noexcept {
    return (tx - f.l) / (f.r - f.l);
}
inline double VFromTanY(const FovTan& f, double ty) noexcept {
    return (f.t - ty) / (f.t - f.b);
}

// The frustum covered by a UV window of an image whose full extent covers `full`.
// Exact because tangents are affine in UV, so a window's bounds are just the
// interpolated tangents at its edges.
inline FovTan FovOfRect(const FovTan& full, const Rect& rect) noexcept {
    return {TanX(full, rect.u0), TanX(full, rect.u1), TanY(full, rect.v0), TanY(full, rect.v1)};
}

// Horizontal image coordinate of the frustum's angular bisector: the direction that
// sits halfway between the left and right edge *angles*. For a symmetric frustum
// this is 0.5; for an asymmetric one it is not, because u is affine in the tangent,
// not in the angle. A renderer that treats u = 0.5 as "straight ahead" is off by
// |BisectorU - 0.5| of the image width.
inline double BisectorU(const FovTan& f) noexcept {
    const double centre_angle = 0.5 * (std::atan(f.l) + std::atan(f.r));
    return UFromTanX(f, std::tan(centre_angle));
}

// Horizontal image coordinate of the view-space forward direction (tangent 0).
// Only 0.5 when the frustum is centred on the optical axis.
inline double ForwardU(const FovTan& f) noexcept {
    return UFromTanX(f, 0.0);
}

inline bool ExactMatch(const FovTan& a, const FovTan& b, double tol_tan = 1e-9) noexcept {
    return std::abs(a.l - b.l) <= tol_tan && std::abs(a.r - b.r) <= tol_tan &&
           std::abs(a.t - b.t) <= tol_tan && std::abs(a.b - b.b) <= tol_tan;
}

// Largest absolute difference over the four edge tangents; a scalar "how far from
// geometrically identical are these two frusta".
inline double MaxTangentDelta(const FovTan& a, const FovTan& b) noexcept {
    return std::max(
        {std::abs(a.l - b.l), std::abs(a.r - b.r), std::abs(a.t - b.t), std::abs(a.b - b.b)});
}

// ---------------------------------------------------------------------------
// Ray <-> coordinate mapping
// ---------------------------------------------------------------------------

inline double Dot(const Ray& a, const Ray& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline Ray Normalized(const Ray& v) noexcept {
    const double n = std::sqrt(Dot(v, v));
    return {v[0] / n, v[1] / n, v[2] / n};
}

// Ray through projection-plane point (tx, ty, -1).
inline Ray RayFromTangent(double tx, double ty) noexcept {
    return Normalized({tx, ty, -1.0});
}

// Angle between two rays, in degrees. Uses atan2 of the cross/dot pair rather than
// acos(dot) so that it keeps full relative precision for the tiny angles that the
// exactness tests assert on.
inline double AngleBetweenDeg(const Ray& a, const Ray& b) noexcept {
    const double cross[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                             a[0] * b[1] - a[1] * b[0]};
    const double cn = std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    return ToDegrees(std::atan2(cn, Dot(a, b)));
}

// Azimuth of a ray: 0 straight ahead, positive to the right.
inline double AzimuthDeg(const Ray& d) noexcept {
    return ToDegrees(std::atan2(d[0], -d[2]));
}
// Elevation of a ray: 0 straight ahead, positive upwards.
inline double ElevationDeg(const Ray& d) noexcept {
    return ToDegrees(std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2])));
}

// Fraction of `a..b` that `x` occupies.
inline double Fraction(double a, double b, double x) noexcept {
    return (x - a) / (b - a);
}

// Ray represented by source-image UV (u, v) — absolute image UV, v downwards.
//
// Geometry: the source image is a rectilinear projection of the frustum `full`.
// Screen coordinate is affine in the tangent, so for u inside [rect.u0, rect.u1]
// the horizontal tangent is exactly lerp(tan of the left edge angle, tan of the
// right edge angle). Because TanX is globally affine, this is identical to
// TanX(full, u): cropping a window out of an image does not change which ray any
// given source pixel represents, it only changes which pixels get read.
//
// The rect edges are used directly rather than the full extent so that the
// interpolation statement stays local and remains true for any window.
inline Ray RayFromSourceUv(const FovTan& full, const Rect& rect, double u, double v) noexcept {
    const FovTan f = FovOfRect(full, rect);
    return RayFromTangent(TanX(f, Fraction(rect.u0, rect.u1, u)),
                          TanY(f, Fraction(rect.v0, rect.v1, v)));
}

// Deliberately wrong variant, kept so the error can be measured: interpolate the
// half-angle linearly across the image instead of the tangent. This maps every
// straight world line to a curve (a degenerate projection), so it can only agree
// with RayFromSourceUv in the limit of small angles.
inline Ray RayFromSourceUvSpherical(const FovTan& full, const Rect& rect, double u,
                                    double v) noexcept {
    const FovTan f = FovOfRect(full, rect);
    const double s = Fraction(rect.u0, rect.u1, u);
    const double w = Fraction(rect.v0, rect.v1, v);
    const double ax = std::lerp(std::atan(f.l), std::atan(f.r), s);
    const double ay = std::lerp(std::atan(f.t), std::atan(f.b), w);
    return RayFromTangent(std::tan(ax), std::tan(ay));
}

// Inverse of RayFromSourceUv. Returns nullopt for rays that cannot hit the
// projection plane (dz >= 0), which are behind the eye and have no image position.
inline std::optional<Uv> UvFromRay(const FovTan& full, const Rect& rect, const Ray& dir) noexcept {
    if (dir[2] > -1e-12) {
        return std::nullopt;
    }
    const FovTan f = FovOfRect(full, rect);
    const double tx = dir[0] / -dir[2];
    const double ty = dir[1] / -dir[2];
    return Uv{rect.u0 + UFromTanX(f, tx) * RectWidth(rect),
              rect.v0 + VFromTanY(f, ty) * RectHeight(rect)};
}

// Ray for an OpenXR projection view: ndc_x/ndc_y in [-1, 1] are the rasteriser's
// normalised device coordinates of the XrCompositionLayerProjectionView. The
// standard XrFovf interpretation maps that NDC box affinely onto the asymmetric
// frustum's tangent quad:
//     tx = lerp(tanLeft,  tanRight, (ndc_x + 1) / 2)
//     ty = lerp(tanBottom, tanTop,  (ndc_y + 1) / 2)
// and the ray is (tx, ty, -1) normalised. This is the inverse of the projection
// matrix an OpenXR runtime builds from XrFovf (x_ndc = (2x)/(r-l) - (r+l)/(r-l)
// in tangent space).
inline Ray RayFromXrFov(const FovTan& fov, double ndc_x, double ndc_y) noexcept {
    return RayFromTangent(std::lerp(fov.l, fov.r, 0.5 * (ndc_x + 1.0)),
                          std::lerp(fov.b, fov.t, 0.5 * (ndc_y + 1.0)));
}

// ---------------------------------------------------------------------------
// The step under test: a full-rectangle bilinear resample
// ---------------------------------------------------------------------------

// Source UV that a full-rectangle resample reads for destination NDC position
// (ndc_x, ndc_y), when it stretches `rect_src` of the source across `dst_rect` of
// the destination view.
//
// This is a pure affine map in UV. It contains no FOV term, and that is precisely
// the hazard: it will happily squash an arbitrary source frustum into an arbitrary
// destination frustum. fov_src is accepted only so that call sites are forced to
// have the source FOV at hand and cannot pretend it is irrelevant; the returned
// coordinate does not depend on it.
inline Uv ResampleSourceUvForDest([[maybe_unused]] const FovTan& fov_src, const Rect& rect_src,
                                  double ndc_x, double ndc_y, const Rect& dst_rect) noexcept {
    const double u_view = 0.5 * (ndc_x + 1.0);
    const double v_view = 0.5 * (1.0 - ndc_y);
    const double s = Fraction(dst_rect.u0, dst_rect.u1, u_view);
    const double w = Fraction(dst_rect.v0, dst_rect.v1, v_view);
    return Uv{rect_src.u0 + s * RectWidth(rect_src), rect_src.v0 + w * RectHeight(rect_src)};
}

// The affine resample above is geometrically exact over its whole domain if and only
// if the frustum the source rectangle covers equals the frustum the destination
// rectangle claims. Both sides interpolate the tangent linearly in their own UV, so
// equal frustum bounds force the composed map to reproduce the destination ray at
// every sample.
inline bool ResampleIsExact(const FovTan& fov_src, const Rect& rect_src, const FovTan& fov_dst,
                            const Rect& dst_rect, double tol_tan = 1e-9) noexcept {
    return ExactMatch(FovOfRect(fov_src, rect_src), FovOfRect(fov_dst, dst_rect), tol_tan);
}

// ---------------------------------------------------------------------------
// Error measurement
// ---------------------------------------------------------------------------

struct CompareResult {
    int samples = 0;
    // Angle in degrees between the ray the destination pixel claims (from fov_dst)
    // and the ray the sampled source pixel actually represents (from fov_src).
    double max_deg = 0.0;
    double mean_abs_deg = 0.0;
    double rms_deg = 0.0;
    // Where the maximum occurs, in destination NDC and in destination-view UV.
    double worst_ndc_x = 0.0;
    double worst_ndc_y = 0.0;
    double worst_u = 0.0;
    double worst_v = 0.0;
    // Signed components at the worst sample (azimuth/elevation, source minus dest);
    // positive horizontal means the source pixel points further right than claimed.
    double worst_signed_h_deg = 0.0;
    double worst_signed_v_deg = 0.0;
    // Asymmetry diagnostics: worst |error| restricted to each half of dst_rect.
    double max_left_deg = 0.0;
    double max_right_deg = 0.0;
    double max_top_deg = 0.0;
    double max_bottom_deg = 0.0;
    // Destination-view UV where the signed horizontal (vertical) error changes sign
    // along the row (column) through the centre, if it does.
    std::optional<double> zero_cross_u;
    std::optional<double> zero_cross_v;
};

// Compare an affine full-rect resample of (fov_src, rect_src) against what the
// destination frustum claims, over an N x N grid of destination NDC samples
// (N >= 2 samples per axis, endpoints included).
inline CompareResult Compare(const FovTan& fov_src, const Rect& rect_src, const FovTan& fov_dst,
                             const Rect& dst_rect, int n) noexcept {
    CompareResult out{};
    if (n < 2) {
        return out;
    }

    const double step = 1.0 / static_cast<double>(n - 1);
    double sum_abs = 0.0;
    double sum_sq = 0.0;

    // Rows/columns for the zero-crossing scan: the grid line closest to the centre.
    const int mid_index = (n - 1) / 2;
    double prev_h = 0.0;
    double prev_v = 0.0;
    bool have_prev = false;

    for (int j = 0; j < n; ++j) {
        const double ndc_y = -1.0 + 2.0 * (j * step);
        for (int i = 0; i < n; ++i) {
            const double ndc_x = -1.0 + 2.0 * (i * step);

            const Ray claimed = RayFromXrFov(fov_dst, ndc_x, ndc_y);
            const Uv src = ResampleSourceUvForDest(fov_src, rect_src, ndc_x, ndc_y, dst_rect);
            const Ray actual = RayFromSourceUv(fov_src, rect_src, src.u, src.v);

            const double err = AngleBetweenDeg(claimed, actual);
            const double signed_h = AzimuthDeg(actual) - AzimuthDeg(claimed);
            const double signed_v = ElevationDeg(actual) - ElevationDeg(claimed);

            if (err > out.max_deg) {
                out.max_deg = err;
                out.worst_ndc_x = ndc_x;
                out.worst_ndc_y = ndc_y;
                out.worst_u = 0.5 * (ndc_x + 1.0);
                out.worst_v = 0.5 * (1.0 - ndc_y);
                out.worst_signed_h_deg = signed_h;
                out.worst_signed_v_deg = signed_v;
            }
            sum_abs += std::abs(err);
            sum_sq += err * err;
            ++out.samples;

            if (ndc_x < 0.0) {
                out.max_left_deg = std::max(out.max_left_deg, err);
            } else {
                out.max_right_deg = std::max(out.max_right_deg, err);
            }
            if (ndc_y > 0.0) {
                out.max_top_deg = std::max(out.max_top_deg, err);
            } else {
                out.max_bottom_deg = std::max(out.max_bottom_deg, err);
            }

            if (j == mid_index) {
                if (have_prev && !out.zero_cross_u.has_value() && prev_h != signed_h &&
                    ((prev_h < 0.0) != (signed_h < 0.0))) {
                    const double t = prev_h / (prev_h - signed_h);
                    out.zero_cross_u = 0.5 * (ndc_x + 1.0) - t * step;
                }
                prev_h = signed_h;
                have_prev = true;
            }
            if (i == mid_index) {
                const double prev_ndc_y = -1.0 + 2.0 * ((j - 1) * step);
                if (j > 0 && !out.zero_cross_v.has_value() && prev_v != signed_v &&
                    ((prev_v < 0.0) != (signed_v < 0.0))) {
                    const double t = prev_v / (prev_v - signed_v);
                    out.zero_cross_v = 0.5 * (1.0 - std::lerp(prev_ndc_y, ndc_y, t));
                }
                prev_v = signed_v;
            }
        }
    }

    out.mean_abs_deg = sum_abs / static_cast<double>(out.samples);
    out.rms_deg = std::sqrt(sum_sq / static_cast<double>(out.samples));
    return out;
}

// ---------------------------------------------------------------------------
// Minimal crop / pad plan
// ---------------------------------------------------------------------------

struct RectMatchPlan {
    // False when the source and destination frusta do not overlap on either axis:
    // then no crop, no pad and no resample can make the mapping exact.
    bool possible = false;
    // Frustum shared by the two rectangles: the largest region both can present.
    FovTan shared{};
    // Windows that both cover exactly `shared`. Feeding these to Compare() yields
    // zero error, by construction.
    Rect src_rect{};
    Rect dst_rect{};
    // How far each source edge has to move, in source pixels, to reach `src_rect`.
    // Always >= 0: the shared frustum is by definition inside the source window, so
    // the source is only ever cropped. Padding, when it is needed, shows up on the
    // other side instead, as a `dst_rect` smaller than the destination window (bars).
    // Order: left, right, top, bottom.
    std::array<double, 4> crop_px{0.0, 0.0, 0.0, 0.0};
    // Fraction of rect_src's area still used after the crop.
    double kept_area_frac = 0.0;
};

// Work out the minimal source crop and the matching destination placement that make
// an affine resample geometrically exact. This is the practical answer to "8:9 source
// into a 1:1 destination view with differing FOVs": crop the source down to the
// shared tangent intervals, or inset the destination (bars) where the source is the
// narrower side, and the resample becomes exact.
//
// Both rectangles present the tangents of their own edges linearly, so the shared
// region is simply the intersection of the two tangent intervals on each axis. Each
// axis is independent because the mapping is separable.
inline RectMatchPlan PlanCrop(const FovTan& fov_src, const Rect& rect_src, const FovTan& fov_dst,
                              const Rect& rect_dst, int src_width_px, int src_height_px) noexcept {
    RectMatchPlan plan{};
    if (!IsValidRect(rect_src) || !IsValidRect(rect_dst) || src_width_px <= 0 ||
        src_height_px <= 0) {
        return plan;
    }

    const FovTan fs = FovOfRect(fov_src, rect_src);
    const FovTan fd = FovOfRect(fov_dst, rect_dst);

    // Tangent intervals: [l, r] horizontally, [b, t] vertically (b < t).
    const double left = std::max(fs.l, fd.l);
    const double right = std::min(fs.r, fd.r);
    const double bottom = std::max(fs.b, fd.b);
    const double top = std::min(fs.t, fd.t);
    if (left >= right || bottom >= top) {
        return plan;
    }

    plan.possible = true;
    plan.shared = FovTan{left, right, top, bottom};

    // Tangent -> rectangle-relative UV -> absolute UV, for both sides.
    const double su = Fraction(fs.l, fs.r, left);
    const double eu = Fraction(fs.l, fs.r, right);
    const double sv = Fraction(fs.t, fs.b, top);
    const double ev = Fraction(fs.t, fs.b, bottom);
    plan.src_rect =
        Rect{rect_src.u0 + su * RectWidth(rect_src), rect_src.v0 + sv * RectHeight(rect_src),
             rect_src.u0 + eu * RectWidth(rect_src), rect_src.v0 + ev * RectHeight(rect_src)};

    const double du = Fraction(fd.l, fd.r, left);
    const double eu_d = Fraction(fd.l, fd.r, right);
    const double dv = Fraction(fd.t, fd.b, top);
    const double ev_d = Fraction(fd.t, fd.b, bottom);
    plan.dst_rect =
        Rect{rect_dst.u0 + du * RectWidth(rect_dst), rect_dst.v0 + dv * RectHeight(rect_dst),
             rect_dst.u0 + eu_d * RectWidth(rect_dst), rect_dst.v0 + ev_d * RectHeight(rect_dst)};

    const double w = static_cast<double>(src_width_px);
    const double h = static_cast<double>(src_height_px);
    plan.crop_px = {(plan.src_rect.u0 - rect_src.u0) * w, (rect_src.u1 - plan.src_rect.u1) * w,
                    (plan.src_rect.v0 - rect_src.v0) * h, (rect_src.v1 - plan.src_rect.v1) * h};

    const double kept = RectWidth(plan.src_rect) * RectHeight(plan.src_rect);
    const double whole = RectWidth(rect_src) * RectHeight(rect_src);
    plan.kept_area_frac = kept / whole;
    return plan;
}

// ---------------------------------------------------------------------------
// Stereo
// ---------------------------------------------------------------------------

// One eye of a stereo pair. offset_x_m is the eye's position in head space along
// +X (so -IPD/2 for the left eye and +IPD/2 for the right); the eye's own fov
// already encodes any toe-in, so no extra rotation is modelled.
struct EyeProjection {
    FovTan fov{};
    double offset_x_m = 0.0;
};

inline Ray EyeRayDirection(const EyeProjection& eye, double ndc_x, double ndc_y) noexcept {
    return RayFromXrFov(eye.fov, ndc_x, ndc_y);
}

// Image UV at which a head-space point appears in this eye, or nullopt if the point
// is not in front of it.
inline std::optional<Uv> EyeUvOfPoint(const EyeProjection& eye, double x, double y,
                                      double z) noexcept {
    return UvFromRay(eye.fov, FullRect(), Ray{x - eye.offset_x_m, y, z});
}

} // namespace VrProj
