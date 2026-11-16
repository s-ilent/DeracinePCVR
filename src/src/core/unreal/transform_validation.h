// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/unreal/guest_memory.h"
#include "core/unreal/hand_retarget.h"

namespace Core::Unreal {

// FTransform on 64-bit is 0x30 bytes with a 16-byte aligned quaternion, but the two vectors sit at
// 0x10/0x1C in some builds and 0x20/0x2C in others. Nothing here assumes one: candidates are scored
// by their data.
struct TransformLayout {
    u64 stride = 0x30;
    u64 rotation = 0x00;
    u64 translation = 0x10;
    u64 scale = 0x1C;

    bool operator==(const TransformLayout&) const = default;
};

struct Transform {
    Quat rotation;
    Vec3 translation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Result of reading a transform array and checking it against the invariants a component-space pose
// has: unit quaternions, finite translations, sane scales.
struct TransformArrayStats {
    u32 requested = 0;
    u32 read = 0;
    u32 unit_rotations = 0;
    u32 finite = 0;
    u32 plausible_scales = 0;
    float min_scale = 0.0f;
    float max_scale = 0.0f;

    // The array is only accepted when every element that was read passed every check.
    bool valid() const {
        return requested > 0 && read == requested && unit_rotations == requested &&
               finite == requested && plausible_scales == requested;
    }
};

// The layouts worth trying when looking for a pose array, narrowest first.
std::span<const TransformLayout> CandidateTransformLayouts();

bool ReadTransform(const GuestMemory& memory, VAddr array, u32 index, const TransformLayout& layout,
                   Transform& out);

TransformArrayStats ValidateTransformArray(const GuestMemory& memory, VAddr array, u32 count,
                                           const TransformLayout& layout);

// A pose array candidate found inside a component object: a TArray-shaped {data, num, max} field
// whose contents validate as transforms of a known length.
struct TransformArrayCandidate {
    VAddr field_address = 0;
    VAddr data = 0;
    u32 count = 0;
    u32 capacity = 0;
    TransformLayout layout;
    TransformArrayStats stats;
};

// Which bone counts a component could legitimately hold: the mesh's own list, the skeleton's list,
// and the LOD's active subset are different numbers (79 / 105 / narrower).
struct ExpectedBoneCounts {
    u32 mesh = 0;
    u32 skeleton = 0;
    u32 lod_active = 0;

    bool Matches(u32 count) const {
        return count != 0 && (count == mesh || count == skeleton || count == lod_active);
    }
};

// Checks each finger chain (parent links, indices in range) against a bone-name list, and returns
// the indices that failed. Empty means the rig and the runtime bone list agree.
std::vector<u32> ValidateFingerChainIndices(const HandRig& rig,
                                            std::span<const std::string> bone_names);

// Compares the rig's reference local rotations with a runtime reference pose: each must be within
// `tolerance_degrees` of the pose rotation, and every referenced parent index must be consistent.
struct ReferencePoseCheck {
    u32 checked = 0;
    u32 mismatched = 0;
    float worst_degrees = 0.0f;
    u32 first_bad_joint = 0;

    bool ok() const {
        return checked > 0 && mismatched == 0;
    }
};
ReferencePoseCheck CompareReferencePose(const HandRig& rig, std::span<const Transform> pose,
                                        float tolerance_degrees = 1.0f);

} // namespace Core::Unreal
