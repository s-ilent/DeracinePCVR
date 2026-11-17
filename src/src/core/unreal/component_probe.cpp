// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/component_probe.h"

#include <algorithm>
#include <cstring>

namespace Core::Unreal {
namespace {

constexpr u64 kMaxArrayCount = 0x10000;
constexpr u32 kMaxCandidates = 16;
constexpr u64 kMinObjectSize = 0x28;

bool ReadU64(const GuestMemory& memory, VAddr address, u64& out) {
    return memory.Read(address, &out, sizeof(out));
}

bool ReadS32(const GuestMemory& memory, VAddr address, s32& out) {
    return memory.Read(address, &out, sizeof(out));
}

bool LooksLikeClass(const ObjectIdentity& identity, std::string_view needle) {
    return identity.class_name.find(needle) != std::string::npos ||
           identity.name.find(needle) != std::string::npos;
}

bool ContainsArrayMatch(const ExpectedBoneCounts& expected, u32 count) {
    if (expected.mesh == 0 && expected.skeleton == 0 && expected.lod_active == 0) {
        return count > 0 && count <= 0x4000;
    }
    return expected.Matches(count);
}

} // Anonymous namespace

ComponentProbeResult ProbeSkeletalMeshComponent(const GuestMemory& memory, VAddr component,
                                                const ExpectedBoneCounts& expected,
                                                const ObjectNamer& namer, u64 object_span) {
    ComponentProbeResult result;
    result.component = component;
    result.expected_bones = std::max({expected.mesh, expected.skeleton, expected.lod_active});
    if (component == 0) {
        result.error = "no component address";
        return result;
    }
    if (object_span < 0x40) {
        result.error = "component span too small";
        return result;
    }

    // Object references: a pointer slot that resolves to an object whose class says what it is.
    // Nothing here assumes a field offset; the class name is the evidence.
    for (u64 offset = 0; offset + 8 <= object_span; offset += 8) {
        u64 pointer = 0;
        if (!ReadU64(memory, component + offset, pointer) || pointer == 0) {
            continue;
        }
        ObjectIdentity identity;
        if (!namer || !namer(pointer, identity)) {
            continue;
        }
        if (result.mesh.empty() && LooksLikeClass(identity, "SkeletalMesh") &&
            !LooksLikeClass(identity, "Component")) {
            result.mesh = identity;
        } else if (result.skeleton.empty() && LooksLikeClass(identity, "Skeleton")) {
            result.skeleton = identity;
        } else if (result.anim_instance.empty() &&
                   (LooksLikeClass(identity, "AnimInstance") || LooksLikeClass(identity, "AnimBlueprint") ||
                    LooksLikeClass(identity, "AnimBP"))) {
            result.anim_instance = identity;
        } else if (result.owner.empty() && offset < 0x40 &&
                   (LooksLikeClass(identity, "BP_C") || LooksLikeClass(identity, "Actor"))) {
            result.owner = identity;
        }
    }

    // TArray-shaped fields: {data, num, max} where the data validates as transforms of a length the
    // component could legitimately hold.
    for (u64 offset = 0; offset + 0x10 <= object_span; offset += 8) {
        u64 data = 0;
        s32 count = 0;
        s32 capacity = 0;
        if (!ReadU64(memory, component + offset, data) ||
            !ReadS32(memory, component + offset + 8, count) ||
            !ReadS32(memory, component + offset + 12, capacity)) {
            continue;
        }
        if (data == 0 || count <= 0 || count > static_cast<s32>(kMaxArrayCount) || capacity < count) {
            continue;
        }
        if (!ContainsArrayMatch(expected, static_cast<u32>(count))) {
            continue;
        }
        for (const auto& layout : CandidateTransformLayouts()) {
            auto stats = ValidateTransformArray(memory, data, static_cast<u32>(count), layout);
            if (!stats.valid()) {
                continue;
            }
            result.candidates.push_back({component + offset, data, static_cast<u32>(count),
                                         static_cast<u32>(capacity), layout, stats});
            break;
        }
        if (result.candidates.size() >= kMaxCandidates) {
            break;
        }
    }

    // Prefer arrays whose length equals one of the known bone counts, then the ones with the most
    // elements: a mismatch is still useful evidence, but it should not outrank an exact fit.
    std::ranges::stable_sort(result.candidates, [&](const auto& a, const auto& b) {
        const bool a_exact = expected.Matches(a.count);
        const bool b_exact = expected.Matches(b.count);
        if (a_exact != b_exact) {
            return a_exact;
        }
        return a.count > b.count;
    });

    if (result.mesh.empty()) {
        result.error = "no skeletal mesh reference resolved";
    }
    return result;
}

} // namespace Core::Unreal
