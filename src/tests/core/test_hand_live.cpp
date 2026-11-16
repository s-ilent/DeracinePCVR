// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/unreal/component_probe.h"
#include "core/unreal/hand_override.h"
#include "core/unreal/transform_validation.h"

namespace Core::Unreal {
namespace {

constexpr float kDeg = 3.14159265358979323846f / 180.0f;
constexpr VAddr kBase = 0x800000000;

// A small writable memory image: one mapped region, readable through GuestMemory.
class MemoryImage final : public GuestMemory {
public:
    explicit MemoryImage(u64 size) : bytes(size, 0) {}

    bool Read(VAddr address, void* out, u64 size) const override {
        if (address < kBase || address + size > kBase + bytes.size()) {
            return false;
        }
        std::memcpy(out, bytes.data() + (address - kBase), size);
        return true;
    }

    void Store(VAddr address, const void* data, u64 size) {
        std::memcpy(bytes.data() + (address - kBase), data, size);
    }

    template <typename T>
    void Store(VAddr address, T value) {
        Store(address, &value, sizeof(T));
    }

    u8* At(VAddr address) {
        return bytes.data() + (address - kBase);
    }

private:
    std::vector<u8> bytes;
};

void StoreTransform(MemoryImage& memory, VAddr element, const Quat& rotation, const Vec3& translation,
                    const Vec3& scale) {
    memory.Store(element + 0x00, rotation);
    memory.Store(element + 0x10, translation);
    memory.Store(element + 0x1C, scale);
}

void StoreTransformLayoutB(MemoryImage& memory, VAddr element, const Quat& rotation,
                           const Vec3& translation, const Vec3& scale) {
    memory.Store(element + 0x00, rotation);
    memory.Store(element + 0x20, translation);
    memory.Store(element + 0x2C, scale);
}

HandRig MakeSmallRig() {
    HandRig rig;
    rig.side = HandSide::Right;
    rig.hand_bone_index = 6;
    for (auto digit = 0u; digit < 5; digit++) {
        for (auto joint = 0u; joint < 3; joint++) {
            HandJoint entry;
            entry.bone_index = 7 + digit * 3 + joint;
            entry.parent = joint == 0 ? kHandJoint : digit * 3 + joint - 1;
            entry.reference_local = QuatFromAxisAngle(Vec3{1, 0, 0}, 5.0f * kDeg);
            entry.reference_world = entry.reference_local;
            rig.joints.push_back(entry);
        }
    }
    return rig;
}

std::vector<Quat> UniformPose(const Quat& value) {
    return std::vector<Quat>(15, value);
}

// -- transform validation ----------------------------------------------------------------------

TEST(TransformValidation, AcceptsAUnitQuaternionArray) {
    MemoryImage memory(0x10000);
    const VAddr array = kBase + 0x1000;
    for (auto i = 0u; i < 79; i++) {
        StoreTransform(memory, array + i * 0x30, Quat{}, Vec3{1.0f * i, 0.0f, 0.0f}, Vec3{1, 1, 1});
    }
    const auto stats = ValidateTransformArray(memory, array, 79, TransformLayout{});
    EXPECT_TRUE(stats.valid());
    EXPECT_EQ(stats.read, 79u);
    EXPECT_NEAR(stats.min_scale, 1.0f, 1e-4f);
}

TEST(TransformValidation, RejectsNonUnitQuaternions) {
    MemoryImage memory(0x10000);
    const VAddr array = kBase + 0x1000;
    for (auto i = 0u; i < 8; i++) {
        StoreTransform(memory, array + i * 0x30, i == 3 ? Quat{1, 1, 1, 1} : Quat{}, Vec3{},
                       Vec3{1, 1, 1});
    }
    const auto stats = ValidateTransformArray(memory, array, 8, TransformLayout{});
    EXPECT_FALSE(stats.valid());
    EXPECT_EQ(stats.unit_rotations, 7u);
}

TEST(TransformValidation, RejectsNonFiniteTranslationsAndBadScales) {
    MemoryImage memory(0x10000);
    const VAddr array = kBase + 0x1000;
    StoreTransform(memory, array, Quat{}, Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0},
                   Vec3{1, 1, 1});
    StoreTransform(memory, array + 0x30, Quat{}, Vec3{}, Vec3{0, 0, 0});
    const auto stats = ValidateTransformArray(memory, array, 2, TransformLayout{});
    EXPECT_FALSE(stats.valid());
    EXPECT_EQ(stats.finite, 1u);
    EXPECT_EQ(stats.plausible_scales, 1u);
}

TEST(TransformValidation, ReadsTheAlternativeVectorOffsets) {
    MemoryImage memory(0x10000);
    const VAddr array = kBase + 0x1000;
    StoreTransformLayoutB(memory, array, Quat{}, Vec3{7, 8, 9}, Vec3{1, 1, 1});
    const auto layout = TransformLayout{0x30, 0x00, 0x20, 0x2C};
    Transform transform;
    ASSERT_TRUE(ReadTransform(memory, array, 0, layout, transform));
    EXPECT_FLOAT_EQ(transform.translation.x, 7.0f);
    EXPECT_FLOAT_EQ(transform.translation.z, 9.0f);
    // The default layout reads different bytes, which is why layouts are scored rather than assumed.
    Transform other;
    ASSERT_TRUE(ReadTransform(memory, array, 0, TransformLayout{}, other));
    EXPECT_NE(other.translation.x, 7.0f);
}

TEST(TransformValidation, FingerChainIndicesMustBeOrderedAndInRange) {
    auto rig = MakeSmallRig();
    std::vector<std::string> bones(64, "bone");
    EXPECT_TRUE(ValidateFingerChainIndices(rig, bones).empty());

    rig.joints[4].bone_index = 1000; // outside the bone list
    auto failures = ValidateFingerChainIndices(rig, bones);
    ASSERT_EQ(failures.size(), 1u);
    EXPECT_EQ(failures[0], 4u);

    auto broken = MakeSmallRig();
    broken.joints[1].parent = 5; // child before parent
    EXPECT_FALSE(ValidateFingerChainIndices(broken, bones).empty());
}

TEST(TransformValidation, ReferencePoseMustMatchTheRig) {
    const auto rig = MakeSmallRig();
    std::vector<Transform> pose(64);
    for (auto i = 0u; i < rig.joints.size(); i++) {
        pose[rig.joints[i].bone_index] = Transform{rig.joints[i].reference_local, {}, {1, 1, 1}};
    }
    const auto check = CompareReferencePose(rig, pose, 1.0f);
    EXPECT_TRUE(check.ok());
    EXPECT_EQ(check.checked, 15u);

    pose[rig.joints[7].bone_index].rotation =
        QuatFromAxisAngle(Vec3{0, 1, 0}, 20.0f * kDeg);
    const auto mismatched = CompareReferencePose(rig, pose, 1.0f);
    EXPECT_FALSE(mismatched.ok());
    EXPECT_EQ(mismatched.mismatched, 1u);
    EXPECT_EQ(mismatched.first_bad_joint, 7u);
    EXPECT_GT(mismatched.worst_degrees, 10.0f);
}

// -- component probe ---------------------------------------------------------------------------

TEST(ComponentProbe, FindsMeshSkeletonAnimInstanceAndTransformArray) {
    MemoryImage memory(0x20000);
    const VAddr component = kBase + 0x1000;
    const VAddr mesh = kBase + 0x2000;
    const VAddr skeleton = kBase + 0x2100;
    const VAddr anim = kBase + 0x2200;
    const VAddr pose_array = kBase + 0x4000;
    const VAddr bogus_array = kBase + 0x9000;

    memory.Store<VAddr>(component + 0x30, mesh);
    memory.Store<VAddr>(component + 0x38, skeleton);
    memory.Store<VAddr>(component + 0x40, anim);
    memory.Store<VAddr>(component + 0x200, pose_array);
    memory.Store<s32>(component + 0x208, 79);
    memory.Store<s32>(component + 0x20C, 79);
    memory.Store<VAddr>(component + 0x250, bogus_array);
    memory.Store<s32>(component + 0x258, 79);
    memory.Store<s32>(component + 0x25C, 79);

    for (auto i = 0u; i < 79; i++) {
        StoreTransform(memory, pose_array + i * 0x30, Quat{}, Vec3{}, Vec3{1, 1, 1});
    }
    std::memset(memory.At(bogus_array), 0xCD, 79 * 0x30);

    std::map<VAddr, ObjectIdentity> names{
        {mesh, {"c0000", "SkeletalMesh"}},
        {skeleton, {"c0000_Skeleton", "Skeleton"}},
        {anim, {"c0000_AnimBP_C", "AnimInstance"}},
    };
    const ObjectNamer namer = [&names](VAddr address, ObjectIdentity& out) {
        const auto found = names.find(address);
        if (found == names.end()) {
            return false;
        }
        out = found->second;
        return true;
    };

    const auto probe = ProbeSkeletalMeshComponent(memory, component, ExpectedBoneCounts{79, 105, 79},
                                                  namer);
    EXPECT_EQ(probe.mesh.name, "c0000");
    EXPECT_EQ(probe.mesh.class_name, "SkeletalMesh");
    EXPECT_EQ(probe.skeleton.name, "c0000_Skeleton");
    EXPECT_EQ(probe.anim_instance.name, "c0000_AnimBP_C");
    ASSERT_EQ(probe.candidates.size(), 1u);
    EXPECT_EQ(probe.candidates[0].count, 79u);
    EXPECT_EQ(probe.candidates[0].data, pose_array);
    EXPECT_EQ(probe.candidates[0].layout.stride, 0x30u);
    EXPECT_TRUE(probe.candidates[0].stats.valid());
}

TEST(ComponentProbe, RejectsArraysWithAnUnexpectedLength) {
    MemoryImage memory(0x20000);
    const VAddr component = kBase + 0x1000;
    const VAddr pose_array = kBase + 0x4000;
    memory.Store<VAddr>(component + 0x200, pose_array);
    memory.Store<s32>(component + 0x208, 12);
    memory.Store<s32>(component + 0x20C, 12);
    for (auto i = 0u; i < 12; i++) {
        StoreTransform(memory, pose_array + i * 0x30, Quat{}, Vec3{}, Vec3{1, 1, 1});
    }
    const auto probe = ProbeSkeletalMeshComponent(memory, component, ExpectedBoneCounts{79, 105, 79},
                                                  {});
    EXPECT_TRUE(probe.candidates.empty());
    EXPECT_EQ(probe.error, "no skeletal mesh reference resolved");
}

// -- override session --------------------------------------------------------------------------

TEST(HandOverrideSession, RefusesUnsafePhases) {
    HandOverrideSession session;
    session.SetEnabled(true);
    auto animated = UniformPose(Quat{});
    auto retargeted = UniformPose(QuatFromAxisAngle(Vec3{1, 0, 0}, 60.0f * kDeg));
    auto out = UniformPose(Quat{});

    for (const auto phase : {AnimationPhase::Unknown, AnimationPhase::BeforeEvaluation,
                             AnimationPhase::EvaluationRunning, AnimationPhase::SkinningHandoff}) {
        EXPECT_EQ(session.Apply(animated, retargeted, 1.0f, OverrideFrame{phase, 7}, out), 0u)
            << "phase " << static_cast<u32>(phase);
        EXPECT_FALSE(session.HasSavedPose());
    }
    // The last refusal in the loop is the skinning handoff; both refusals name their reason.
    EXPECT_TRUE(session.LastRefusal().find("skinning") != std::string::npos ||
                session.LastRefusal().find("evaluation") != std::string::npos);
}

TEST(HandOverrideSession, AppliesOnlyAfterEvaluationAndKeepsTheGamePose) {
    HandOverrideSession session;
    session.SetEnabled(true);
    auto animated = UniformPose(Quat{});
    auto retargeted = UniformPose(QuatFromAxisAngle(Vec3{1, 0, 0}, 60.0f * kDeg));
    auto out = UniformPose(Quat{});

    ASSERT_EQ(session.Apply(animated, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 10},
                            out),
              15u);
    EXPECT_TRUE(session.HasSavedPose());
    EXPECT_GT(QuatAngleRadians(out[3], animated[3]), 0.5f);

    // A second write in the same frame must keep the original animation values, not the blend.
    ASSERT_EQ(session.Apply(animated, retargeted, 0.5f,
                            OverrideFrame{AnimationPhase::AfterEvaluation, 10}, out),
              15u);
    ASSERT_EQ(session.Restore(out), 15u);
    for (auto i = 0u; i < out.size(); i++) {
        EXPECT_EQ(out[i].w, animated[i].w);
        EXPECT_EQ(out[i].x, animated[i].x);
    }
    EXPECT_FALSE(session.HasSavedPose());
}

TEST(HandOverrideSession, WeightZeroAndDisableRestoreTheAnimation) {
    HandOverrideSession session;
    session.SetEnabled(true);
    auto animated = UniformPose(Quat{});
    auto retargeted = UniformPose(QuatFromAxisAngle(Vec3{1, 0, 0}, 60.0f * kDeg));
    auto out = UniformPose(Quat{});

    ASSERT_EQ(session.Apply(animated, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 4},
                            out),
              15u);
    EXPECT_EQ(session.Apply(animated, retargeted, 0.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 5},
                            out),
              0u);
    EXPECT_FALSE(session.HasSavedPose());

    // Write again, then disable: the next frame puts the game's pose back.
    ASSERT_EQ(session.Apply(animated, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 6},
                            out),
              15u);
    session.SetEnabled(false);
    // A disabled session refuses to write but still performs its owed restore into `out`.
    EXPECT_EQ(session.Apply(animated, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 7},
                            out),
              0u);
    EXPECT_FALSE(session.HasSavedPose());
    for (const auto& rotation : out) {
        EXPECT_NEAR(QuatAngleRadians(rotation, Quat{}), 0.0f, 1e-5f);
    }
}

TEST(HandOverrideSession, NewFrameSavesTheNewAnimationPose) {
    HandOverrideSession session;
    session.SetEnabled(true);
    auto retargeted = UniformPose(QuatFromAxisAngle(Vec3{1, 0, 0}, 60.0f * kDeg));
    auto out = UniformPose(Quat{});
    auto first = UniformPose(QuatFromAxisAngle(Vec3{0, 1, 0}, 5.0f * kDeg));
    auto second = UniformPose(QuatFromAxisAngle(Vec3{0, 1, 0}, 25.0f * kDeg));

    ASSERT_EQ(session.Apply(first, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 1}, out),
              15u);
    ASSERT_EQ(session.Apply(second, retargeted, 1.0f, OverrideFrame{AnimationPhase::AfterEvaluation, 2}, out),
              15u);
    ASSERT_EQ(session.Restore(out), 15u);
    EXPECT_NEAR(QuatAngleRadians(out[0], second[0]), 0.0f, 1e-5f);
}

} // Anonymous namespace
} // namespace Core::Unreal
