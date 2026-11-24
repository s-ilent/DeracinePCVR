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

const char* IndexDomainName(IndexDomain domain) noexcept {
    switch (domain) {
    case IndexDomain::Unknown:
        return "unknown";
    case IndexDomain::Ue4Object:
        return "ue4_object";
    case IndexDomain::GuestAddress:
        return "guest_address";
    case IndexDomain::HostBuffer:
        return "host_buffer";
    }
    return "unknown";
}

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
        // A comparison is only meaningful when both values are in the same namespace. The tape
        // records the RHI invocation's index-buffer argument, which is an original UE4 object
        // pointer; the package records the guest GPU address. Comparing those numbers would
        // produce a meaningless match or a meaningless mismatch, so a cross-domain pair is
        // reported as not comparable and the join cannot rest on it.
        const bool same_domain = tape_identity.index_domain != IndexDomain::Unknown &&
                                 tape_identity.index_domain == package_identity.index_domain;
        IdentityField field{};
        field.name = "index_guest_base";
        field.tape_value = Hex(tape_identity.index_guest_base);
        field.package_value = Hex(package_identity.index_guest_base);
        field.comparable = same_domain && tape_identity.index_guest_base != 0 &&
                           package_identity.index_guest_base != 0;
        field.agrees =
            field.comparable && tape_identity.index_guest_base == package_identity.index_guest_base;
        result.fields.push_back(field);

        // The domains themselves are reported, so a reader can see why a comparison was refused.
        IdentityField domains{};
        domains.name = "index_domain";
        domains.tape_value = IndexDomainName(tape_identity.index_domain);
        domains.package_value = IndexDomainName(package_identity.index_domain);
        domains.comparable = true;
        domains.agrees = same_domain;
        result.fields.push_back(domains);

        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the tape and the package name different index buffers";
            return result;
        }
        if (!same_domain) {
            // The domains differ or are unknown, so the index resource cannot establish the join.
            // This is reported rather than silently falling through to the derived fields, which
            // are a consistency check and never identity.
            result.outcome = JoinOutcome::InsufficientEvidence;
            result.reason =
                std::string{"the index resource cannot be compared: the tape records a "} +
                IndexDomainName(tape_identity.index_domain) + " and the package a " +
                IndexDomainName(package_identity.index_domain) +
                ", which are different namespaces";
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

    // These are consistency checks, not identity. A semantic mutation changes the draw arguments
    // on purpose while keeping the resource, and that is a legitimate execution rather than a
    // mismatched capture. A disagreement is recorded so a reader can tell a mutated tape from an
    // unmutated one, but it does not by itself mean the two captures describe different draws.
    const auto consistency = [&result](std::string_view name, u64 tape_value, u64 package_value,
                                       bool comparable) {
        IdentityField field{};
        field.name = name;
        field.tape_value = Dec(tape_value);
        field.package_value = Dec(package_value);
        field.comparable = comparable;
        field.agrees = comparable && tape_value == package_value;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.derived_consistent = false;
            result.derived_disagreements.push_back(name);
        }
        return field;
    };

    consistency("index_count", tape_identity.derived_index_count, package_identity.index_count,
                tape_identity.derived_index_count != 0 && package_identity.index_count != 0);
    consistency("first_index", tape_identity.start_index, package_identity.first_index,
                package_identity.has_index_binding);
    // The base vertex is signed on both sides, so a negative value is compared as signed rather
    // than through a cast that would make it a huge positive number.
    {
        IdentityField field{};
        field.name = "base_vertex";
        field.tape_value = SignedDec(tape_identity.base_vertex_index);
        field.package_value = SignedDec(static_cast<s32>(package_identity.base_vertex));
        field.comparable = package_identity.has_index_binding;
        field.agrees = field.comparable && tape_identity.base_vertex_index ==
                                               static_cast<s32>(package_identity.base_vertex);
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.derived_consistent = false;
            result.derived_disagreements.push_back("base_vertex");
        }
    }
    consistency("instance_count", tape_identity.num_instances, package_identity.instance_count,
                package_identity.has_index_binding);
    consistency("first_instance", tape_identity.first_instance, package_identity.first_instance,
                package_identity.has_index_binding);

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

bool IsBridgeComparable(const IndexBridge& bridge) noexcept {
    return bridge.has_guest_allocation_base && bridge.guest_allocation_base != 0;
}

JoinResult JoinTapeToPackageWithBridge(const TapeIdentity& tape_identity,
                                       const PackageIdentity& package_identity,
                                       const IndexBridge& bridge) noexcept {
    JoinResult result{};

    // --- Execution identity -------------------------------------------------------------------
    // A bridge from a different execution than the package cannot establish a match. The same
    // address in two processes is not the same resource, and a reused provenance id across runs
    // is not the same draw.
    {
        IdentityField field{};
        field.name = "execution_id";
        field.tape_value = Hex(bridge.execution_id);
        field.package_value = Hex(package_identity.execution_id);
        field.comparable = bridge.execution_id != 0 && package_identity.execution_id != 0;
        field.agrees = field.comparable && bridge.execution_id == package_identity.execution_id;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason =
                "the bridge and the package were captured in different executions";
            return result;
        }
    }

    // --- Provenance ---------------------------------------------------------------------------
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

    // --- The bridge itself --------------------------------------------------------------------
    // The bridge must be present and comparable. Falling back to the object-pointer comparison
    // when it is absent would be the defect this module exists to prevent.
    if (!IsBridgeComparable(bridge)) {
        IdentityField field{};
        field.name = "index_bridge";
        field.tape_value = "absent";
        field.package_value = Hex(package_identity.index_guest_base);
        field.comparable = false;
        field.agrees = false;
        result.fields.push_back(field);
        result.outcome = JoinOutcome::InsufficientEvidence;
        result.reason =
            "the original index-buffer object was not resolved to a guest allocation, so the "
            "index resource cannot be compared";
        return result;
    }

    // --- Index resource, now in one namespace -------------------------------------------------
    {
        IdentityField field{};
        field.name = "index_guest_base";
        field.tape_value = Hex(bridge.guest_allocation_base);
        field.package_value = Hex(package_identity.index_guest_base);
        field.comparable = package_identity.index_guest_base != 0;
        field.agrees = field.comparable &&
                       bridge.guest_allocation_base == package_identity.index_guest_base;
        result.fields.push_back(field);
        if (field.comparable && !field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the resolved index allocation and the package name different buffers";
            return result;
        }
        if (!field.comparable) {
            result.outcome = JoinOutcome::InsufficientEvidence;
            result.reason = "the package carries no index allocation to compare against";
            return result;
        }
    }

    // --- Generation ---------------------------------------------------------------------------
    // The same address with a different generation is a reused allocation, not the same resource.
    if (bridge.has_resource_generation && bridge.resource_generation != 0) {
        IdentityField field{};
        field.name = "resource_generation";
        field.tape_value = Dec(bridge.resource_generation);
        field.package_value = "unknown";
        field.comparable = false;
        field.agrees = false;
        result.fields.push_back(field);
        result.outcome = JoinOutcome::InsufficientEvidence;
        result.reason =
            "the bridge carries a resource generation the package cannot confirm, so an address "
            "match alone does not establish identity";
        return result;
    }

    // --- Element width ------------------------------------------------------------------------
    if (bridge.has_index_element_width && package_identity.index_element_size != 0) {
        IdentityField field{};
        field.name = "index_element_size";
        field.tape_value = Dec(bridge.index_element_width);
        field.package_value = Dec(package_identity.index_element_size);
        field.comparable = true;
        field.agrees = bridge.index_element_width == package_identity.index_element_size;
        result.fields.push_back(field);
        if (!field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the bridge and the package disagree on the index element width";
            return result;
        }
    }

    // --- Range --------------------------------------------------------------------------------
    // The draw must read inside the allocation. An address match with a range that runs past the
    // end is not a usable identity.
    if (bridge.has_guest_allocation_size && bridge.guest_allocation_size != 0 &&
        package_identity.index_guest_size != 0) {
        const u64 offset = bridge.has_allocation_offset ? bridge.allocation_offset : 0;
        const u64 required = static_cast<u64>(package_identity.index_count) *
                             (package_identity.index_element_size != 0
                                  ? package_identity.index_element_size
                                  : 1);
        IdentityField field{};
        field.name = "index_range";
        field.tape_value = Dec(offset + required);
        field.package_value = Dec(bridge.guest_allocation_size);
        field.comparable = true;
        field.agrees = offset <= bridge.guest_allocation_size &&
                       required <= bridge.guest_allocation_size - offset;
        result.fields.push_back(field);
        if (!field.agrees) {
            result.outcome = JoinOutcome::Mismatched;
            result.reason = "the draw reads outside the resolved index allocation";
            return result;
        }
    }

    // --- Derived draw -------------------------------------------------------------------------
    // A consistency check, not identity. Two different draws can share every one of these.
    const auto consistency = [&result](std::string_view name, u64 tape_value, u64 package_value,
                                       bool comparable) {
        IdentityField field{};
        field.name = name;
        field.tape_value = Dec(tape_value);
        field.package_value = Dec(package_value);
        field.comparable = comparable;
        field.agrees = comparable && tape_value == package_value;
        result.fields.push_back(field);
        if (comparable && !field.agrees) {
            result.derived_consistent = false;
            result.derived_disagreements.push_back(name);
        }
    };
    consistency("index_count", tape_identity.derived_index_count, package_identity.index_count,
                package_identity.has_index_binding);
    consistency("first_index", tape_identity.start_index, package_identity.first_index,
                package_identity.has_index_binding);
    consistency("instance_count", tape_identity.num_instances, package_identity.instance_count,
                package_identity.has_index_binding);
    consistency("first_instance", tape_identity.first_instance, package_identity.first_instance,
                package_identity.has_index_binding);

    result.outcome = JoinOutcome::Joined;
    result.reason.clear();
    return result;
}

} // namespace tape