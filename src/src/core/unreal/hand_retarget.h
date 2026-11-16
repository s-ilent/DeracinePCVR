// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <vector>

#include "common/types.h"

namespace Core::Unreal {

// Minimal quaternion/vector types: the retargeter is pure math so it can be unit-tested without a
// guest process. Quaternions are Unreal's (x, y, z, w) layout.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

constexpr u32 OpenXrHandJointCount = 26;

// OpenXR hand joint order (XR_HAND_JOINT_*); the indices the retargeter's maps refer to.
enum class OpenXrJoint : u32 {
    Palm = 0,
    Wrist = 1,
    ThumbMetacarpal = 2,
    ThumbProximal = 3,
    ThumbDistal = 4,
    ThumbTip = 5,
    IndexMetacarpal = 6,
    IndexProximal = 7,
    IndexIntermediate = 8,
    IndexDistal = 9,
    IndexTip = 10,
    MiddleMetacarpal = 11,
    MiddleProximal = 12,
    MiddleIntermediate = 13,
    MiddleDistal = 14,
    MiddleTip = 15,
    RingMetacarpal = 16,
    RingProximal = 17,
    RingIntermediate = 18,
    RingDistal = 19,
    RingTip = 20,
    LittleMetacarpal = 21,
    LittleProximal = 22,
    LittleIntermediate = 23,
    LittleDistal = 24,
    LittleTip = 25,
};

enum class HandSide : u32 {
    Left = 0,
    Right = 1,
};

// A game bone driven by the retargeter, with the data its local transform needs.
struct HandJoint {
    u32 bone_index = 0;             // index in the skeleton's reference bone array
    u32 parent = 0;                 // index into HandRig::joints, or HandParentHand for a digit root
    Quat reference_local;           // reference-pose local rotation
    Quat reference_world;           // reference-pose world rotation (composes up to here)
    float max_bend_degrees = 90.0f; // clamp of the local rotation away from the reference pose
};

// One hand's 15 retargetable finger joints, ordered so parents come before children.
struct HandRig {
    HandSide side = HandSide::Right;
    u32 hand_bone_index = 0;        // skeleton bone index of <side>-Hand
    Quat hand_reference_local;      // reference-pose local rotation of the hand bone
    std::vector<HandJoint> joints;  // 15 joints
};

// Which game joint each OpenXR joint drives. kUnmapped means no bone (metacarpals, tips), and
// kHand marks joints that only contribute to the hand's frame.
constexpr u32 kUnmappedJoint = 0xFFFF'FFFEu;
constexpr u32 kHandJoint = 0xFFFF'FFFFu;

struct OpenXrMapping {
    // Index into HandRig::joints, kHandJoint, or kUnmappedJoint, per OpenXR joint.
    std::array<u32, OpenXrHandJointCount> game_joint{};
    // OpenXR joints used to build the hand frame (palm, wrist).
    std::array<u32, 2> hand_frame_joints{static_cast<u32>(OpenXrJoint::Palm),
                                         static_cast<u32>(OpenXrJoint::Wrist)};
};

struct RetargetOptions {
    // Local rotations closer to the reference pose than the clamp are impossible to check cheaply,
    // so the clamp is applied to the angle between the target local and the reference local.
    bool clamp_bends = true;
    // Below this weight the retargeted pose is ignored entirely.
    float minimum_weight = 0.02f;
};

// Converts live OpenXR joint orientations into the game's local bone rotations.
//
// Coordinate handling: OpenXR is right-handed Y-up (-Z forward), Unreal is left-handed Z-up
// (+X forward). The mapping between them is a reflection, which no quaternion can express, so
// orientations are converted through 3x3 matrices (M * R * M^T) and turned back into quaternions.
// Any remaining per-joint convention difference is absorbed by a calibrated offset
// `C = reference_world * inverse(Convert(xr_rest))`, measured once from a rest capture, so the
// left and right rigs need no mirror-specific code.
class HandRetargeter {
public:
    HandRetargeter() = default;
    HandRetargeter(HandRig rig, OpenXrMapping mapping, RetargetOptions options = {});

    // Records the per-joint offsets from a rest capture (26 orientations, XR space). Until this is
    // called the retargeter returns the rig's reference pose.
    void Calibrate(std::span<const Quat> xr_rest);
    bool calibrated() const {
        return is_calibrated;
    }

    const HandRig& Rig() const {
        return rig;
    }

    // XR joint orientation -> Unreal orientation, with the handedness conversion applied.
    static Quat ConvertOrientation(const Quat& xr);

    // Target local rotations for every rig joint. `hand_world` is the game hand bone's current
    // world rotation (the animation keeps owning the wrist); it anchors the digit roots.
    void Retarget(std::span<const Quat> xr_live, const Quat& hand_world,
                  std::span<Quat> out_local) const;

    // Blends between the animation's evaluated local rotations and the retargeted ones:
    // weight 0 keeps the game pose, 1 replaces it. Returns the number of joints actually blended.
    u32 Blend(std::span<const Quat> animated_local, std::span<const Quat> target_local, float weight,
              std::span<Quat> out_local) const;

    // Game-thread preference: an override must yield while the game is grabbing. A weight of 0
    // leaves every rotation untouched.
    static float InteractionWeight(float desired_weight, bool grab_active);

private:
    Quat ClampToReference(const Quat& target_local, const Quat& reference_local,
                          float max_bend_degrees) const;

    HandRig rig;
    OpenXrMapping mapping;
    RetargetOptions options;
    bool is_calibrated = false;
    std::vector<Quat> joint_offsets; // C per rig joint
    Quat hand_offset;                // C for the hand frame
};

// -- quaternion helpers (exposed for the tests and for the live writer) --------------------------

Quat QuatMultiply(const Quat& a, const Quat& b);
Quat QuatInverse(const Quat& q);
Quat QuatNormalize(const Quat& q);
float QuatAngleRadians(const Quat& a, const Quat& b);
Quat QuatSlerp(const Quat& a, const Quat& b, float t);
Quat QuatFromAxisAngle(const Vec3& axis, float radians);
Vec3 QuatRotate(const Quat& q, const Vec3& v);

// Basis change between OpenXR (right-handed, Y up) and Unreal (left-handed, Z up):
// UE = (-XR.z, XR.x, XR.y), i.e. forward = -XR.back, right = XR.right, up = XR.up.
constexpr float kXrToUnrealBasis[3][3] = {
    {0.0f, 0.0f, -1.0f},
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
};

} // namespace Core::Unreal
