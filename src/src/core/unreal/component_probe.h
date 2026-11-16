// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/unreal/guest_memory.h"
#include "core/unreal/transform_validation.h"

namespace Core::Unreal {

struct ObjectIdentity {
    std::string name;
    std::string class_name;

    bool empty() const {
        return name.empty() && class_name.empty();
    }
};

// Resolves a guest address to an object's name and class. The inspector provides this; tests
// provide a table, which is what keeps the probe independent of the object-array layout.
using ObjectNamer = std::function<bool(VAddr address, ObjectIdentity& out)>;

struct ComponentProbeResult {
    VAddr component = 0;
    ObjectIdentity owner;
    ObjectIdentity mesh;
    ObjectIdentity skeleton;
    ObjectIdentity anim_instance;
    u32 expected_bones = 0;
    std::vector<TransformArrayCandidate> candidates;
    std::string error;

    bool found_mesh() const {
        return !mesh.empty();
    }
    bool found_transform_array() const {
        return !candidates.empty();
    }
};

// Scans a USkeletalMeshComponent object for its mesh/skeleton/animation instance references and for
// TArray-shaped fields whose contents validate as transform arrays. Read-only: it never writes.
ComponentProbeResult ProbeSkeletalMeshComponent(const GuestMemory& memory, VAddr component,
                                                const ExpectedBoneCounts& expected,
                                                const ObjectNamer& namer,
                                                u64 object_span = 0x800);

} // namespace Core::Unreal
