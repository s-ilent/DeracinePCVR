// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/transform_validation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Core::Unreal {
namespace {

constexpr float kUnitQuaternionTolerance = 0.01f;
constexpr float kMinPlausibleScale = 0.01f;
constexpr float kMaxPlausibleScale = 100.0f;
constexpr float kDegrees = 3.14159265358979323846f / 180.0f;

constexpr std::array<TransformLayout, 3> Layouts{{
    {0x30, 0x00, 0x10, 0x1C},
    {0x30, 0x00, 0x20, 0x2C},
    {0x28, 0x00, 0x10, 0x1C},
}};

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Plausible(const Vec3& v) {
    return Finite(v) && v.x >= kMinPlausibleScale && v.x <= kMaxPlausibleScale &&
           v.y >= kMinPlausibleScale && v.y <= kMaxPlausibleScale && v.z >= kMinPlausibleScale &&
           v.z <= kMaxPlausibleScale;
}

} // Anonymous namespace

std::span<const TransformLayout> CandidateTransformLayouts() {
    return Layouts;
}

bool ReadTransform(const GuestMemory& memory, VAddr array, u32 index, const TransformLayout& layout,
                   Transform& out) {
    const VAddr element = array + static_cast<u64>(index) * layout.stride;
    std::array<float, 4> rotation{};
    std::array<float, 3> translation{};
    std::array<float, 3> scale{};
    if (!memory.Read(element + layout.rotation, rotation.data(), sizeof(rotation)) ||
        !memory.Read(element + layout.translation, translation.data(), sizeof(translation)) ||
        !memory.Read(element + layout.scale, scale.data(), sizeof(scale))) {
        return false;
    }
    out.rotation = Quat{rotation[0], rotation[1], rotation[2], rotation[3]};
    out.translation = Vec3{translation[0], translation[1], translation[2]};
    out.scale = Vec3{scale[0], scale[1], scale[2]};
    return true;
}

TransformArrayStats ValidateTransformArray(const GuestMemory& memory, VAddr array, u32 count,
                                           const TransformLayout& layout) {
    TransformArrayStats stats;
    stats.requested = count;
    if (count == 0 || array == 0) {
        return stats;
    }
    stats.min_scale = kMaxPlausibleScale;
    stats.max_scale = kMinPlausibleScale;
    for (auto index = 0u; index < count; index++) {
        Transform transform;
        if (!ReadTransform(memory, array, index, layout, transform)) {
            break;
        }
        stats.read++;
        const auto length = std::sqrt(transform.rotation.x * transform.rotation.x +
                                      transform.rotation.y * transform.rotation.y +
                                      transform.rotation.z * transform.rotation.z +
                                      transform.rotation.w * transform.rotation.w);
        const bool unit = std::isfinite(length) && std::abs(length - 1.0f) <= kUnitQuaternionTolerance;
        const bool finite = Finite(transform.translation) && Finite(transform.scale) &&
                            std::isfinite(transform.rotation.x) && std::isfinite(transform.rotation.y) &&
                            std::isfinite(transform.rotation.z) && std::isfinite(transform.rotation.w);
        const bool scale_ok = Plausible(transform.scale);
        if (unit) {
            stats.unit_rotations++;
        }
        if (finite) {
            stats.finite++;
        }
        if (scale_ok) {
            stats.plausible_scales++;
            const auto largest = std::max({transform.scale.x, transform.scale.y, transform.scale.z});
            const auto smallest = std::min({transform.scale.x, transform.scale.y, transform.scale.z});
            stats.max_scale = std::max(stats.max_scale, largest);
            stats.min_scale = std::min(stats.min_scale, smallest);
        }
    }
    if (stats.plausible_scales == 0) {
        stats.min_scale = 0.0f;
        stats.max_scale = 0.0f;
    }
    return stats;
}

std::vector<u32> ValidateFingerChainIndices(const HandRig& rig,
                                            std::span<const std::string> bone_names) {
    std::vector<u32> failures;
    for (auto index = 0u; index < rig.joints.size(); index++) {
        const auto& joint = rig.joints[index];
        const bool bone_in_range = joint.bone_index < bone_names.size();
        const bool parent_ok = joint.parent == kHandJoint ||
                               (joint.parent < rig.joints.size() && joint.parent < index);
        if (!bone_in_range || !parent_ok) {
            failures.push_back(index);
            continue;
        }
        // A child must not be the same bone as its parent.
        if (joint.parent != kHandJoint && rig.joints[joint.parent].bone_index == joint.bone_index) {
            failures.push_back(index);
        }
    }
    if (rig.hand_bone_index >= bone_names.size() && !bone_names.empty()) {
        failures.push_back(kHandJoint);
    }
    return failures;
}

ReferencePoseCheck CompareReferencePose(const HandRig& rig, std::span<const Transform> pose,
                                        float tolerance_degrees) {
    ReferencePoseCheck check;
    for (auto index = 0u; index < rig.joints.size(); index++) {
        const auto& joint = rig.joints[index];
        if (joint.bone_index >= pose.size()) {
            check.first_bad_joint = index;
            check.mismatched++;
            continue;
        }
        const auto& rotation = pose[joint.bone_index].rotation;
        const auto length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y +
                                      rotation.z * rotation.z + rotation.w * rotation.w);
        if (!std::isfinite(length) || std::abs(length - 1.0f) > kUnitQuaternionTolerance) {
            check.first_bad_joint = index;
            check.mismatched++;
            continue;
        }
        const auto degrees = QuatAngleRadians(joint.reference_local, rotation) / kDegrees;
        check.checked++;
        check.worst_degrees = std::max(check.worst_degrees, degrees);
        if (degrees > tolerance_degrees) {
            if (check.mismatched == 0) {
                check.first_bad_joint = index;
            }
            check.mismatched++;
        }
    }
    return check;
}

} // namespace Core::Unreal
