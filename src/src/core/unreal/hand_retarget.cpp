// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/hand_retarget.h"

#include <algorithm>
#include <cmath>

namespace Core::Unreal {
namespace {

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
constexpr float kRadiansToDegrees = 180.0f / 3.14159265358979323846f;

struct Mat3 {
    float m[3][3]{};
};

Mat3 QuatToMatrix(const Quat& q) {
    const auto n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const float x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n;
    Mat3 out;
    out.m[0][0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[0][1] = 2.0f * (x * y - z * w);
    out.m[0][2] = 2.0f * (x * z + y * w);
    out.m[1][0] = 2.0f * (x * y + z * w);
    out.m[1][1] = 1.0f - 2.0f * (x * x + z * z);
    out.m[1][2] = 2.0f * (y * z - x * w);
    out.m[2][0] = 2.0f * (x * z - y * w);
    out.m[2][1] = 2.0f * (y * z + x * w);
    out.m[2][2] = 1.0f - 2.0f * (x * x + y * y);
    return out;
}

Quat MatrixToQuat(const Mat3& m) {
    Quat q;
    const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m.m[2][1] - m.m[1][2]) / s;
        q.y = (m.m[0][2] - m.m[2][0]) / s;
        q.z = (m.m[1][0] - m.m[0][1]) / s;
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
        q.w = (m.m[2][1] - m.m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m.m[0][1] + m.m[1][0]) / s;
        q.z = (m.m[0][2] + m.m[2][0]) / s;
    } else if (m.m[1][1] > m.m[2][2]) {
        const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
        q.w = (m.m[0][2] - m.m[2][0]) / s;
        q.x = (m.m[0][1] + m.m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m.m[1][2] + m.m[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
        q.w = (m.m[1][0] - m.m[0][1]) / s;
        q.x = (m.m[0][2] + m.m[2][0]) / s;
        q.y = (m.m[1][2] + m.m[2][1]) / s;
        q.z = 0.25f * s;
    }
    return QuatNormalize(q);
}

Mat3 Multiply(const Mat3& a, const Mat3& b) {
    Mat3 out;
    for (auto i = 0; i < 3; i++) {
        for (auto j = 0; j < 3; j++) {
            out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return out;
}

Mat3 Transpose(const Mat3& a) {
    Mat3 out;
    for (auto i = 0; i < 3; i++) {
        for (auto j = 0; j < 3; j++) {
            out.m[i][j] = a.m[j][i];
        }
    }
    return out;
}

Mat3 Basis() {
    Mat3 b;
    for (auto i = 0; i < 3; i++) {
        for (auto j = 0; j < 3; j++) {
            b.m[i][j] = kXrToUnrealBasis[i][j];
        }
    }
    return b;
}

} // Anonymous namespace

Quat QuatMultiply(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat QuatInverse(const Quat& q) {
    return Quat{-q.x, -q.y, -q.z, q.w};
}

Quat QuatNormalize(const Quat& q) {
    const auto n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 1e-9f) {
        return Quat{};
    }
    return Quat{q.x / n, q.y / n, q.z / n, q.w / n};
}

float QuatAngleRadians(const Quat& a, const Quat& b) {
    const auto dot = std::clamp(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w), 0.0f, 1.0f);
    return 2.0f * std::acos(dot);
}

Quat QuatSlerp(const Quat& a, const Quat& b, float t) {
    auto cos_half = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    auto end = b;
    if (cos_half < 0.0f) {
        cos_half = -cos_half;
        end = Quat{-b.x, -b.y, -b.z, -b.w};
    }
    if (cos_half > 0.9995f) {
        return QuatNormalize(Quat{a.x + (end.x - a.x) * t, a.y + (end.y - a.y) * t,
                                  a.z + (end.z - a.z) * t, a.w + (end.w - a.w) * t});
    }
    const auto half = std::acos(std::clamp(cos_half, -1.0f, 1.0f));
    const auto scale = std::sin(half);
    const auto wa = std::sin((1.0f - t) * half) / scale;
    const auto wb = std::sin(t * half) / scale;
    return QuatNormalize(Quat{a.x * wa + end.x * wb, a.y * wa + end.y * wb, a.z * wa + end.z * wb,
                              a.w * wa + end.w * wb});
}

Quat QuatFromAxisAngle(const Vec3& axis, float radians) {
    const auto length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    if (length <= 1e-9f) {
        return Quat{};
    }
    const auto half = radians * 0.5f;
    const auto s = std::sin(half) / length;
    return Quat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

Vec3 QuatRotate(const Quat& q, const Vec3& v) {
    const auto m = QuatToMatrix(q);
    return Vec3{m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
                m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
                m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z};
}

Quat HandRetargeter::ConvertOrientation(const Quat& xr) {
    // M * R * M^T stays a proper rotation even though M is a reflection, which is why the
    // right-handed to left-handed conversion goes through a matrix.
    const auto basis = Basis();
    const auto rotation = QuatToMatrix(xr);
    return MatrixToQuat(Multiply(Multiply(basis, rotation), Transpose(basis)));
}

HandRetargeter::HandRetargeter(HandRig rig_, OpenXrMapping mapping_, RetargetOptions options_)
    : rig{std::move(rig_)}, mapping{mapping_}, options{options_} {
    joint_offsets.assign(rig.joints.size(), Quat{});
}

void HandRetargeter::Calibrate(std::span<const Quat> xr_rest) {
    joint_offsets.assign(rig.joints.size(), Quat{});
    for (auto joint_index = 0u; joint_index < rig.joints.size(); joint_index++) {
        u32 xr_index = kUnmappedJoint;
        for (auto i = 0u; i < OpenXrHandJointCount; i++) {
            if (mapping.game_joint[i] == joint_index) {
                xr_index = i;
                break;
            }
        }
        if (xr_index == kUnmappedJoint || xr_index >= xr_rest.size()) {
            continue;
        }
        const auto converted = ConvertOrientation(xr_rest[xr_index]);
        joint_offsets[joint_index] =
            QuatNormalize(QuatMultiply(rig.joints[joint_index].reference_world,
                                       QuatInverse(converted)));
    }
    const auto hand_xr = mapping.hand_frame_joints[0];
    if (hand_xr < xr_rest.size()) {
        hand_offset = QuatInverse(ConvertOrientation(xr_rest[hand_xr]));
    }
    is_calibrated = true;
}

Quat HandRetargeter::ClampToReference(const Quat& target_local, const Quat& reference_local,
                                      float max_bend_degrees) const {
    const auto angle = QuatAngleRadians(reference_local, target_local) * kRadiansToDegrees;
    if (angle <= max_bend_degrees || angle <= 1e-4f) {
        return target_local;
    }
    return QuatSlerp(reference_local, target_local, max_bend_degrees / angle);
}

void HandRetargeter::Retarget(std::span<const Quat> xr_live, const Quat& hand_world,
                              std::span<Quat> out_local) const {
    std::vector<Quat> world(rig.joints.size(), Quat{});
    std::vector<bool> has_world(rig.joints.size(), false);
    for (auto index = 0u; index < rig.joints.size(); index++) {
        const auto& joint = rig.joints[index];
        Quat local = joint.reference_local;
        u32 xr_index = kUnmappedJoint;
        for (auto i = 0u; i < OpenXrHandJointCount; i++) {
            if (mapping.game_joint[i] == index) {
                xr_index = i;
                break;
            }
        }
        if (is_calibrated && xr_index != kUnmappedJoint && xr_index < xr_live.size()) {
            const auto target_world = QuatMultiply(ConvertOrientation(xr_live[xr_index]),
                                                   joint_offsets[index]);
            const Quat parent_world = joint.parent == kHandJoint
                                          ? hand_world
                                          : (joint.parent < world.size() && has_world[joint.parent]
                                                 ? world[joint.parent]
                                                 : hand_world);
            local = QuatNormalize(QuatMultiply(QuatInverse(parent_world), target_world));
            if (options.clamp_bends) {
                local = ClampToReference(local, joint.reference_local, joint.max_bend_degrees);
            }
            world[index] = target_world;
            has_world[index] = true;
        } else if (joint.parent != kHandJoint && joint.parent < world.size() &&
                   has_world[joint.parent]) {
            world[index] = QuatMultiply(world[joint.parent], local);
            has_world[index] = true;
        } else {
            world[index] = QuatMultiply(hand_world, local);
            has_world[index] = true;
        }
        if (index < out_local.size()) {
            out_local[index] = local;
        }
    }
}

u32 HandRetargeter::Blend(std::span<const Quat> animated_local, std::span<const Quat> target_local,
                          float weight, std::span<Quat> out_local) const {
    const auto clamped = std::clamp(weight, 0.0f, 1.0f);
    if (clamped <= options.minimum_weight) {
        for (auto i = 0u; i < out_local.size() && i < animated_local.size(); i++) {
            out_local[i] = animated_local[i];
        }
        return 0;
    }
    u32 blended = 0;
    for (auto i = 0u; i < out_local.size() && i < animated_local.size() && i < target_local.size();
         i++) {
        out_local[i] = QuatSlerp(animated_local[i], target_local[i], clamped);
        ++blended;
    }
    return blended;
}

float HandRetargeter::InteractionWeight(float desired_weight, bool grab_active) {
    if (grab_active) {
        return 0.0f;
    }
    return std::clamp(desired_weight, 0.0f, 1.0f);
}

} // namespace Core::Unreal
