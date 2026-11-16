// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <vector>

#include "core/unreal/hand_retarget.h"

namespace Core::Unreal {
namespace {

constexpr float kDeg = 3.14159265358979323846f / 180.0f;
constexpr float kEpsDeg = 0.25f;

float DegreesBetween(const Quat& a, const Quat& b) {
    return QuatAngleRadians(a, b) / kDeg;
}

Quat AxisAngle(float x, float y, float z, float degrees) {
    return QuatFromAxisAngle(Vec3{x, y, z}, degrees * kDeg);
}

// 15 joints: five 3-bone digits, each rooted at the hand. Reference locals are non-identity so the
// clamp and the calibration paths are actually exercised.
HandRig BuildRig(HandSide side) {
    HandRig rig;
    rig.side = side;
    rig.hand_bone_index = 6;
    rig.hand_reference_local = Quat{};
    const float mirror = side == HandSide::Right ? 1.0f : -1.0f;
    Quat hand_world{};
    for (auto digit = 0u; digit < 5; digit++) {
        for (auto joint = 0u; joint < 3; joint++) {
            HandJoint entry;
            entry.bone_index = 10 + digit * 3 + joint;
            entry.parent = joint == 0 ? kHandJoint : digit * 3 + joint - 1;
            // A few degrees of authored curl on every bone, mirrored on the left.
            entry.reference_local =
                AxisAngle(1.0f, 0.0f, 0.0f, mirror * (4.0f + 2.0f * digit + joint));
            const Quat parent_world =
                joint == 0 ? hand_world : rig.joints[entry.parent].reference_world;
            entry.reference_world = QuatNormalize(QuatMultiply(parent_world, entry.reference_local));
            entry.max_bend_degrees = 90.0f;
            rig.joints.push_back(entry);
        }
    }
    return rig;
}

OpenXrMapping BuildMapping() {
    OpenXrMapping mapping{};
    for (auto& entry : mapping.game_joint) {
        entry = kUnmappedJoint;
    }
    auto set = [&mapping](OpenXrJoint joint, u32 game_joint) {
        mapping.game_joint[static_cast<u32>(joint)] = game_joint;
    };
    set(OpenXrJoint::Palm, kHandJoint);
    set(OpenXrJoint::Wrist, kHandJoint);
    // Thumb: metacarpal + two phalanges, then tip unmapped.
    set(OpenXrJoint::ThumbMetacarpal, 0);
    set(OpenXrJoint::ThumbProximal, 1);
    set(OpenXrJoint::ThumbDistal, 2);
    // INDEX/MIDDLE/RING/LITTLE: metacarpal unused, three phalanges mapped.
    const std::array<OpenXrJoint, 4> metacarpals{OpenXrJoint::IndexMetacarpal, OpenXrJoint::MiddleMetacarpal,
                                                 OpenXrJoint::RingMetacarpal, OpenXrJoint::LittleMetacarpal};
    const std::array<std::array<OpenXrJoint, 3>, 4> phalanges{{
        {OpenXrJoint::IndexProximal, OpenXrJoint::IndexIntermediate, OpenXrJoint::IndexDistal},
        {OpenXrJoint::MiddleProximal, OpenXrJoint::MiddleIntermediate, OpenXrJoint::MiddleDistal},
        {OpenXrJoint::RingProximal, OpenXrJoint::RingIntermediate, OpenXrJoint::RingDistal},
        {OpenXrJoint::LittleProximal, OpenXrJoint::LittleIntermediate, OpenXrJoint::LittleDistal},
    }};
    for (auto digit = 0u; digit < 4; digit++) {
        set(metacarpals[digit], kUnmappedJoint);
        for (auto joint = 0u; joint < 3; joint++) {
            set(phalanges[digit][joint], (digit + 1) * 3 + joint);
        }
    }
    return mapping;
}

std::vector<Quat> IdentityJoints() {
    return std::vector<Quat>(OpenXrHandJointCount, Quat{});
}

class HandRetargetTest : public ::testing::Test {
protected:
    HandRetargetTest() {
        rig = BuildRig(HandSide::Right);
        mapping = BuildMapping();
        retargeter = HandRetargeter(rig, mapping);
        rest = IdentityJoints();
        retargeter.Calibrate(rest);
    }

    HandRig rig;
    OpenXrMapping mapping;
    HandRetargeter retargeter;
    std::vector<Quat> rest;
};

TEST(HandRetargetMath, BasisConversionPreservesRotationAngle) {
    // A reflection-based basis change must still produce a rotation of the same magnitude.
    const auto quarter = AxisAngle(0.0f, 1.0f, 0.0f, 90.0f);
    const auto converted = HandRetargeter::ConvertOrientation(quarter);
    EXPECT_NEAR(DegreesBetween(converted, Quat{}), 90.0f, 0.01f);
    EXPECT_NEAR(QuatNormalize(converted).w * QuatNormalize(converted).w +
                    QuatNormalize(converted).x * QuatNormalize(converted).x +
                    QuatNormalize(converted).y * QuatNormalize(converted).y +
                    QuatNormalize(converted).z * QuatNormalize(converted).z,
                1.0f, 1e-4f);
    // Identity stays identity.
    const auto identity = HandRetargeter::ConvertOrientation(Quat{});
    EXPECT_NEAR(DegreesBetween(identity, Quat{}), 0.0f, 1e-4f);
}

TEST_F(HandRetargetTest, RestCaptureReproducesTheReferencePose) {
    std::vector<Quat> out(rig.joints.size());
    retargeter.Retarget(rest, Quat{}, out);

    for (auto i = 0u; i < rig.joints.size(); i++) {
        EXPECT_NEAR(DegreesBetween(out[i], rig.joints[i].reference_local), 0.0f, kEpsDeg)
            << "joint " << i;
    }
}

TEST_F(HandRetargetTest, IsolatedIndexBendMovesOnlyThatDigit) {
    auto live = IdentityJoints();
    live[static_cast<u32>(OpenXrJoint::IndexProximal)] = AxisAngle(1.0f, 0.0f, 0.0f, 55.0f);
    live[static_cast<u32>(OpenXrJoint::IndexIntermediate)] = AxisAngle(1.0f, 0.0f, 0.0f, 45.0f);
    live[static_cast<u32>(OpenXrJoint::IndexDistal)] = AxisAngle(1.0f, 0.0f, 0.0f, 35.0f);

    std::vector<Quat> out(rig.joints.size());
    retargeter.Retarget(live, Quat{}, out);

    // Index is the second digit (joints 3..5).
    EXPECT_GT(DegreesBetween(out[3], rig.joints[3].reference_local), 20.0f);
    // Local rotations are relative to the parent, so a chain bent by equal world rotations keeps
    // its joints nearly aligned: the distal joint only shows the difference between its own
    // world rotation and its parent's.
    EXPECT_GT(DegreesBetween(out[3], rig.joints[3].reference_local), 20.0f);
    EXPECT_GT(DegreesBetween(out[5], rig.joints[5].reference_local), 5.0f);
    for (auto i : {0u, 1u, 2u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u}) {
        EXPECT_LT(DegreesBetween(out[i], rig.joints[i].reference_local), 0.01f)
            << "digit " << i / 3 << " moved";
    }
}

TEST_F(HandRetargetTest, ClosedFistBendsEveryDigit) {
    auto live = IdentityJoints();
    const std::array<std::array<OpenXrJoint, 3>, 5> chains{{
        {OpenXrJoint::ThumbProximal, OpenXrJoint::ThumbDistal, OpenXrJoint::ThumbTip},
        {OpenXrJoint::IndexProximal, OpenXrJoint::IndexIntermediate, OpenXrJoint::IndexDistal},
        {OpenXrJoint::MiddleProximal, OpenXrJoint::MiddleIntermediate, OpenXrJoint::MiddleDistal},
        {OpenXrJoint::RingProximal, OpenXrJoint::RingIntermediate, OpenXrJoint::RingDistal},
        {OpenXrJoint::LittleProximal, OpenXrJoint::LittleIntermediate, OpenXrJoint::LittleDistal},
    }};
    for (const auto& chain : chains) {
        for (auto joint = 0u; joint < 3; joint++) {
            live[static_cast<u32>(chain[joint])] = AxisAngle(0.0f, 1.0f, 0.0f, 70.0f);
        }
    }

    std::vector<Quat> out(rig.joints.size());
    retargeter.Retarget(live, Quat{}, out);
    for (auto digit = 0u; digit < 5; digit++) {
        double total = 0.0;
        for (auto joint = 0u; joint < 3; joint++) {
            const auto index = digit * 3 + joint;
            total += DegreesBetween(out[index], rig.joints[index].reference_local);
        }
        EXPECT_GT(total, 40.0) << "digit " << digit << " did not close";
    }
}

TEST_F(HandRetargetTest, ClampLimitsAnImpossibleBend) {
    auto live = IdentityJoints();
    live[static_cast<u32>(OpenXrJoint::MiddleProximal)] = AxisAngle(1.0f, 0.0f, 0.0f, 170.0f);
    std::vector<Quat> out(rig.joints.size());
    retargeter.Retarget(live, Quat{}, out);
    // Joints 9..11 are the middle digit.
    EXPECT_LE(DegreesBetween(out[9], rig.joints[9].reference_local), 90.0f + kEpsDeg);
}

TEST_F(HandRetargetTest, BlendInterpolatesBetweenAnimationAndTracking) {
    auto live = IdentityJoints();
    live[static_cast<u32>(OpenXrJoint::RingDistal)] = AxisAngle(1.0f, 0.0f, 0.0f, 80.0f);
    std::vector<Quat> target(rig.joints.size());
    retargeter.Retarget(live, Quat{}, target);

    std::vector<Quat> animated(rig.joints.size());
    for (auto i = 0u; i < animated.size(); i++) {
        animated[i] = AxisAngle(1.0f, 0.0f, 0.0f, 10.0f);
    }
    std::vector<Quat> out(rig.joints.size());

    EXPECT_EQ(retargeter.Blend(animated, target, 0.0f, out), 0u);
    for (auto i = 0u; i < out.size(); i++) {
        EXPECT_NEAR(DegreesBetween(out[i], animated[i]), 0.0f, 1e-4f);
    }

    const auto full = DegreesBetween(animated[14], target[14]);
    EXPECT_GT(retargeter.Blend(animated, target, 1.0f, out), 0u);
    EXPECT_NEAR(DegreesBetween(out[14], target[14]), 0.0f, kEpsDeg);

    retargeter.Blend(animated, target, 0.5f, out);
    EXPECT_NEAR(DegreesBetween(out[14], animated[14]), full * 0.5f, 1.0f);
}

TEST_F(HandRetargetTest, LeftAndRightRigsProduceMirroredMagnitudes) {
    auto left = HandRetargeter(BuildRig(HandSide::Left), mapping);
    left.Calibrate(rest);
    auto live = IdentityJoints();
    live[static_cast<u32>(OpenXrJoint::LittleProximal)] = AxisAngle(1.0f, 0.0f, 0.0f, 50.0f);

    std::vector<Quat> right_out(rig.joints.size());
    std::vector<Quat> left_out(rig.joints.size());
    retargeter.Retarget(live, Quat{}, right_out);
    left.Retarget(live, Quat{}, left_out);

    for (auto i = 0u; i < rig.joints.size(); i++) {
        const auto right_angle = DegreesBetween(right_out[i], rig.joints[i].reference_local);
        const auto left_angle =
            DegreesBetween(left_out[i], left.Rig().joints[i].reference_local);
        EXPECT_NEAR(right_angle, left_angle, 0.5f) << "joint " << i;
    }
}

TEST(HandRetargetInteraction, GrabYieldsTheOverride) {
    EXPECT_FLOAT_EQ(HandRetargeter::InteractionWeight(1.0f, true), 0.0f);
    EXPECT_FLOAT_EQ(HandRetargeter::InteractionWeight(1.0f, false), 1.0f);
    EXPECT_FLOAT_EQ(HandRetargeter::InteractionWeight(0.25f, false), 0.25f);
    EXPECT_FLOAT_EQ(HandRetargeter::InteractionWeight(2.0f, false), 1.0f);
}

} // Anonymous namespace
} // namespace Core::Unreal
