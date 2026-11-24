// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "draw_identity.h"

#include <sstream>

namespace tape {

namespace {

std::string Hex(u64 value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string Dec(u64 value) {
    return std::to_string(value);
}

std::string SignedDec(s32 value) {
    return std::to_string(value);
}

} // namespace

const char* JoinOutcomeName(JoinOutcome outcome) noexcept {
    switch (outcome) {
    case JoinOutcome::Joined:
        return "joined";
    case JoinOutcome::Mismatched:
        return "mismatched";
    case JoinOutcome::InsufficientEvidence:
        return "insufficient_evidence";
    case JoinOutcome::MissingIdentity:
        return "missing_identity";
    }
    return "unknown";
}

JoinResult JoinTapeToPackage(const TapeIdentity& tape_identity,
                             const PackageIdentity& package_identity) noexcept {
    JoinResult result{};

    // --- Provenance ---------------------------------------------------------------------------
    // The same record id from the same execution. A disagreement is decisive: the two captures
    // observed different draws, and no other field can repair that.
    {
        IdentityField field{};
        field.name = "provenance_id";
        field.tape_value = Dec(tape_identity.provenance_id);
        field.package_value = Dec(package_identity.provenance_id);
        field.comparable = tape_identity.provenance_id != 0 && package_identity.provenance_id != 0;
        field.agrees = field.comparable &&
                       tape_identity.provenance_id == package_identity.provenance_id;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape and the package were captured for different draws";
            return result;
        }
    }

    // --- Index resource -----------------------------------------------------------------------
    // The guest address and size of the index buffer. The tape reads the address from the RHI
    // invocation argument and the package from the production binding, so agreement means both
    // observed the same resource rather than a same-sized one.
    bool index_identity_agrees = false;
    {
        IdentityField field{};
        field.name = "index_guest_base";
        field.tape_value = Hex(tape_identity.index_guest_base);
        field.package_value = Hex(package_identity.index_guest_base);
        field.comparable =
            tape_identity.index_guest_base != 0 && package_identity.index_guest_base != 0;
        field.agrees =
            field.comparable && tape_identity.index_guest_base == package_identity.index_guest_base;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape and the package name different index buffers";
            return result;
        }
        index_identity_agrees = field.agrees;
    }
    {
        IdentityField field{};
        field.name = "index_guest_size";
        field.tape_value = Dec(tape_identity.index_guest_size);
        field.package_value = Dec(package_identity.index_guest_size);
        field.comparable =
            tape_identity.index_guest_size != 0 && package_identity.index_guest_size != 0;
        field.agrees =
            field.comparable && tape_identity.index_guest_size == package_identity.index_guest_size;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape and the package name different index buffer extents";
            return result;
        }
    }

    // --- Element width ------------------------------------------------------------------------
    // Resolved independently on each side. A disagreement means one of them is describing a
    // different buffer, because the width is a property of the resource.
    {
        IdentityField field{};
        field.name = "index_element_size";
        field.tape_value = Dec(tape_identity.index_element_size);
        field.package_value = Dec(package_identity.index_element_size);
        field.comparable =
            tape_identity.index_element_size != 0 && package_identity.index_element_size != 0;
        field.agrees = field.comparable &&
                       tape_identity.index_element_size == package_identity.index_element_size;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape and the package disagree on the index element width";
            return result;
        }
    }

    // --- Derived draw -------------------------------------------------------------------------
    // A consistency check, not identity. Two different draws can share every one of these, so
    // agreement alone never establishes the join; disagreement is still informative.
    const auto compare = [&result](std::string_view name, u64 tape_value, u64 package_value,
                                   bool comparable) {
        IdentityField field{};
        field.name = name;
        field.tape_value = Dec(tape_value);
        field.package_value = Dec(package_value);
        field.comparable = comparable;
        field.agrees = comparable && tape_value == package_value;
        result.fields.push_back(field);
        return field;
    };

    const auto index_count = compare("index_count", tape_identity.derived_index_count,
                                     package_identity.index_count,
                                     tape_identity.derived_index_count != 0 &&
                                         package_identity.index_count != 0);
    if (index_count.comparable && !index_count.agrees) {
        result.outcome = JoinOutcome::Mismatched;
        result.reason = "the tape's derived index count does not match the package's index count";
        return result;
    }
    const auto first_index =
        compare("first_index", tape_identity.start_index, package_identity.first_index,
                package_identity.has_index_binding);
    if (first_index.comparable && !first_index.agrees) {
        result.outcome = JoinOutcome::Mismatched;
        result.reason = "the tape's StartIndex does not match the package's first index";
        return result;
    }
    // The base vertex is signed on both sides, so a negative value is compared as signed rather
    // than through a cast that would make it a huge positive number.
    {
        IdentityField field{};
        field.name = "base_vertex";
        field.tape_value = SignedDec(tape_identity.base_vertex_index);
        field.package_value = SignedDec(static_cast<s32>(package_identity.base_vertex));
        field.comparable = package_identity.has_index_binding;
        field.agrees = field.comparable &&
                       tape_identity.base_vertex_index ==
                           static_cast<s32>(package_identity.base_vertex);
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape's BaseVertexIndex does not match the package's base vertex";
            return result;
        }
    }
    const auto instances = compare("instance_count", tape_identity.num_instances,
                                   package_identity.instance_count,
                                   package_identity.has_index_binding);
    if (instances.comparable && !instances.agrees) {
        result.outcome = JoinOutcome::Mismatched;
        result.reason = "the tape's NumInstances does not match the package's instance count";
        return result;
    }
    const auto first_instance = compare("first_instance", tape_identity.first_instance,
                                        package_identity.first_instance,
                                        package_identity.has_index_binding);
    if (first_instance.comparable && !first_instance.agrees) {
        result.outcome = JoinOutcome::Mismatched;
        result.reason = "the tape's FirstInstance does not match the package's first instance";
        return result;
    }

    // --- Verdict ------------------------------------------------------------------------------
    // The join succeeds only when a real identity field established it. The derived fields are a
    // consistency check, so a tape and a package that agree on every derived value but share no
    // identity are reported as insufficient evidence rather than joined.
    if (!index_identity_agrees) {
        result.outcome = JoinOutcome::InsufficientEvidence;
        result.reason =
            "no identity field establishes that the tape and the package describe the same draw";
        return result;
    }

    result.outcome = JoinOutcome::Joined;
    result.reason.clear();
    return result;
}

} // namespace tape
