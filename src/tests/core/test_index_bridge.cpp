// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the original index-resource bridge.
//
// The defect these exist to prevent: treating an original UE4 object pointer and a guest GPU
// address as comparable because both are numbers. They are different namespaces, and the bridge is
// what resolves one into the other.
//
// A genuine match must not be obtainable by copying values between the two fixtures. Each test
// therefore varies exactly one side and asserts the classification.

#include <gtest/gtest.h>

#include "draw_identity.h"

namespace {

using tape::IndexBridge;
using tape::JoinOutcome;
using tape::JoinResult;
using tape::JoinTapeToPackageWithBridge;
using tape::PackageIdentity;
using tape::TapeIdentity;
using tape::u32;
using tape::u64;

constexpr u64 kSession = 0x9999888877776666ull;
constexpr u64 kExecution = 0x1111222233334444ull;
constexpr u64 kOtherExecution = 0x5555666677778888ull;
constexpr u64 kGuestBase = 0x2015e40000ull;
constexpr u64 kGuestSize = 6072;

/// The tape side: an original UE4 object pointer, in the object namespace.
TapeIdentity MakeTape() {
    TapeIdentity identity{};
    identity.provenance_id = 42;
    identity.session_id = kSession;
    // The tape carries the object pointer, not a guest address.
    identity.index_guest_base = 0x101355bce8ull;
    identity.index_guest_size = 0;
    identity.index_domain = tape::IndexDomain::Ue4Object;
    identity.index_element_size = 2;
    identity.primitive_type = 0;
    identity.num_primitives = 1012;
    identity.num_instances = 1;
    identity.num_vertices = 593;
    identity.start_index = 0;
    identity.base_vertex_index = 0;
    identity.first_instance = 0;
    identity.derived_index_count = 3036;
    return identity;
}

/// The package side: a guest GPU allocation.
PackageIdentity MakePackage() {
    PackageIdentity identity{};
    identity.provenance_id = 42;
    identity.session_id = kSession;
    identity.execution_id = kExecution;
    identity.index_guest_base = kGuestBase;
    identity.index_guest_size = kGuestSize;
    identity.index_domain = tape::IndexDomain::GuestAddress;
    identity.index_element_size = 2;
    identity.index_count = 3036;
    identity.first_index = 0;
    identity.base_vertex = 0;
    identity.instance_count = 1;
    identity.first_instance = 0;
    identity.primitive_type = 4;
    identity.has_index_binding = true;
    return identity;
}

/// The bridge: the object resolved to the guest allocation.
IndexBridge MakeBridge() {
    IndexBridge bridge{};
    bridge.session_id = kSession;
    bridge.execution_id = kExecution;
    bridge.original_rhi_object = 0x101355bce8ull;
    bridge.has_original_rhi_object = true;
    bridge.guest_allocation_base = kGuestBase;
    bridge.has_guest_allocation_base = true;
    bridge.guest_allocation_size = kGuestSize;
    bridge.has_guest_allocation_size = true;
    bridge.allocation_offset = 0;
    bridge.has_allocation_offset = true;
    bridge.index_element_width = 2;
    bridge.has_index_element_width = true;
    bridge.evidence = "runtime_observed";
    return bridge;
}

/// Whether a field with this name agreed.
bool FieldAgrees(const JoinResult& result, std::string_view name) {
    for (const auto& field : result.fields) {
        if (field.name == name) {
            return field.agrees;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// 1. The bridge resolves the object to the allocation the package names.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AVErifiedBridgeJoins) {
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), MakeBridge());
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_TRUE(FieldAgrees(result, "index_guest_base"));
    EXPECT_TRUE(FieldAgrees(result, "execution_id"));
}

// ---------------------------------------------------------------------------------------------
// 2. A different object pointer that resolves to the same allocation.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, ADifferentObjectResolvingToTheSameAllocationIsRefused) {
    // The bridge must name the exact original object the tape named. A different object that
    // happens to resolve to the same allocation is not this draw's bridge, so the join refuses it
    // rather than accepting an address coincidence.
    auto bridge = MakeBridge();
    bridge.original_rhi_object = 0x101355c000ull;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
    EXPECT_NE(result.reason.find("different original object"), std::string::npos)
        << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 3. The same object, a different allocation generation.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AGenerationMismatchIsRefused) {
    // The object is the same but the allocation was recreated. An address match alone would be a
    // coincidence, so the bridge reports the generation it cannot confirm.
    auto bridge = MakeBridge();
    bridge.resource_generation = 7;
    bridge.has_resource_generation = true;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence) << result.reason;
    EXPECT_NE(result.reason.find("generation"), std::string::npos) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 4. The same guest address reused for a different resource.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AReusedAddressWithADifferentGenerationIsRefused) {
    auto bridge = MakeBridge();
    bridge.resource_generation = 2;
    bridge.has_resource_generation = true;
    auto package = MakePackage();
    // The address agrees, but the generation says the memory was reused.
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), package, bridge);
    EXPECT_NE(result.outcome, JoinOutcome::Joined);
}

// ---------------------------------------------------------------------------------------------
// 5. An allocation with a nonzero offset.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, ANonzeroOffsetWithinTheAllocationJoins) {
    // The draw starts partway into a larger allocation. The range still fits, so this is a match.
    // The allocation must be large enough for the offset plus the draw's byte range: 1024 + 6072
    // exceeds the 6072-byte fixture, which is why this test uses a larger one.
    auto bridge = MakeBridge();
    bridge.allocation_offset = 1024;
    bridge.has_allocation_offset = true;
    bridge.guest_allocation_size = kGuestSize + 1024;
    bridge.has_guest_allocation_size = true;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 6. A valid index range within the resolved allocation.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AValidIndexRangeJoins) {
    auto bridge = MakeBridge();
    bridge.allocation_offset = 0;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_TRUE(FieldAgrees(result, "index_range"));
}

// ---------------------------------------------------------------------------------------------
// 7. An out-of-range index access.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AnOutOfRangeIndexAccessIsRefused) {
    // The offset pushes the draw past the end of the allocation.
    auto bridge = MakeBridge();
    bridge.allocation_offset = kGuestSize - 16;
    bridge.has_allocation_offset = true;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
    EXPECT_NE(result.reason.find("outside"), std::string::npos) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 8. A missing mapping.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AnAbsentBridgeIsInsufficientEvidence) {
    // The object was never resolved. Falling back to comparing the object pointer against the
    // guest address would be the defect this module exists to prevent.
    IndexBridge bridge{};
    bridge.session_id = kSession;
    bridge.execution_id = kExecution;
    bridge.original_rhi_object = 0x101355bce8ull;
    bridge.has_original_rhi_object = true;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 9. Multiple candidate matches.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, ADisagreeingAllocationIsRefused) {
    // Two candidates would be resolved by the caller; here the single candidate disagrees.
    auto bridge = MakeBridge();
    bridge.guest_allocation_base = 0x2015e50000ull;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
    EXPECT_NE(result.reason.find("different buffers"), std::string::npos) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 10. Different execution identifiers.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, ADifferentExecutionIsRefused) {
    auto bridge = MakeBridge();
    bridge.execution_id = kOtherExecution;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
    EXPECT_NE(result.reason.find("different executions"), std::string::npos) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 11. Reused provenance ids across separate executions.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AReusedProvenanceIdAcrossExecutionsIsRefused) {
    // The provenance id agrees, which is exactly the trap: ids are per-execution, so a match on
    // the id alone would join two unrelated draws from two runs.
    auto bridge = MakeBridge();
    bridge.execution_id = kOtherExecution;
    auto package = MakePackage();
    EXPECT_EQ(bridge.execution_id != package.execution_id, true);
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), package, bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 12. A synthetic tape constructed from package data.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, ATapeCarryingThePackageAddressDirectlyIsNotABridge) {
    // The earlier synthetic fixture copied the package guest address into the tape. That makes the
    // tape look like a guest address, but it is not a recovered bridge: the object was never
    // resolved. The bridge join must not accept it.
    auto tape = MakeTape();
    tape.index_guest_base = kGuestBase;
    tape.index_domain = tape::IndexDomain::GuestAddress;
    IndexBridge bridge{};
    bridge.execution_id = kExecution;
    // No allocation: the object was never resolved.
    const auto result = JoinTapeToPackageWithBridge(tape, MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 13. A positive identity disagreement.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AProvenanceDisagreementIsRefused) {
    auto package = MakePackage();
    package.provenance_id = 43;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), package, MakeBridge());
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
}

// ---------------------------------------------------------------------------------------------
// 14. The renderer must not execute when identity remains insufficient.
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, InsufficientEvidenceIsNeverJoined) {
    // Every path that cannot establish identity must report something other than Joined, so a
    // caller that gates execution on Joined cannot run an unverified draw.
    const auto check = [](const IndexBridge& bridge, const PackageIdentity& package) {
        const auto result = JoinTapeToPackageWithBridge(MakeTape(), package, bridge);
        return result.outcome != JoinOutcome::Joined;
    };

    IndexBridge no_allocation{};
    no_allocation.execution_id = kExecution;
    EXPECT_TRUE(check(no_allocation, MakePackage()));

    auto wrong_execution = MakeBridge();
    wrong_execution.execution_id = kOtherExecution;
    EXPECT_TRUE(check(wrong_execution, MakePackage()));

    auto wrong_address = MakeBridge();
    wrong_address.guest_allocation_base = 0x2015e50000ull;
    EXPECT_TRUE(check(wrong_address, MakePackage()));

    auto generation = MakeBridge();
    generation.resource_generation = 3;
    generation.has_resource_generation = true;
    EXPECT_TRUE(check(generation, MakePackage()));

    auto no_package_index = MakePackage();
    no_package_index.index_guest_base = 0;
    EXPECT_TRUE(check(MakeBridge(), no_package_index));
}

// ---------------------------------------------------------------------------------------------
// Element width
// ---------------------------------------------------------------------------------------------

TEST(IndexBridge, AnElementWidthDisagreementIsRefused) {
    auto bridge = MakeBridge();
    bridge.index_element_width = 4;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched) << result.reason;
    EXPECT_NE(result.reason.find("element width"), std::string::npos) << result.reason;
}

TEST(IndexBridge, AnUnknownElementWidthDoesNotBlockTheJoin) {
    // The width is a consistency check, not identity. A bridge that could not recover it still
    // establishes the resource.
    auto bridge = MakeBridge();
    bridge.has_index_element_width = false;
    bridge.index_element_width = 0;
    const auto result = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), bridge);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
}

} // namespace