// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for single-execution capture selection.
//
// The defect these exist to prevent: a provenance id from one process execution being used to
// identify a draw in a second execution. A provenance id is a per-execution record ordinal, so the
// same id in two runs names different draws.

#include <gtest/gtest.h>

#include "core/capture_session_contract.h"

namespace {

using Core::CaptureSession::ResourceCandidate;
using Core::CaptureSession::SelectPair;
using Core::CaptureSession::SelectionOutcome;
using Core::CaptureSession::SemanticCandidate;
using Core::CaptureSession::u32;
using Core::CaptureSession::u64;

constexpr u64 kExecution = 0x1111222233334444ull;
constexpr u64 kOtherExecution = 0x5555666677778888ull;
constexpr u64 kGuestBase = 0x2015e40000ull;

SemanticCandidate MakeSemantic(u64 provenance_id, bool bridge_resolved = true,
                               u64 resolved_base = kGuestBase) {
    SemanticCandidate candidate{};
    candidate.provenance_id = provenance_id;
    candidate.command_capture_id = provenance_id + 100;
    candidate.command_list = 0x101065d400ull;
    candidate.index_resource = 0x101355bce8ull;
    candidate.primitive_type = 0;
    candidate.num_primitives = 1012;
    candidate.num_instances = 1;
    candidate.num_vertices = 593;
    candidate.start_index = 0;
    candidate.bridge_resolved = bridge_resolved;
    candidate.resolved_guest_base = bridge_resolved ? resolved_base : 0;
    return candidate;
}

ResourceCandidate MakeResource(u64 provenance_id, u64 guest_base = kGuestBase) {
    ResourceCandidate candidate{};
    candidate.provenance_id = provenance_id;
    candidate.index_guest_base = guest_base;
    candidate.index_guest_size = 6072;
    candidate.index_element_size = 2;
    candidate.index_count = 3036;
    candidate.first_index = 0;
    candidate.base_vertex = 0;
    candidate.instance_count = 1;
    candidate.first_instance = 0;
    candidate.primitive_type = 4;
    candidate.package_written = true;
    return candidate;
}

TEST(CaptureSession, ASingleCompatiblePairIsSelected) {
    const auto selection = SelectPair(kExecution, {MakeSemantic(42)}, kExecution,
                                      {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::Selected) << selection.reason;
    EXPECT_EQ(selection.provenance_id, 42u);
}

TEST(CaptureSession, DifferentExecutionsAreRefused) {
    // The same provenance id in two processes names different draws.
    const auto selection = SelectPair(kExecution, {MakeSemantic(42)}, kOtherExecution,
                                      {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::DifferentExecution) << selection.reason;
}

TEST(CaptureSession, NoSharedProvenanceIsReported) {
    const auto selection = SelectPair(kExecution, {MakeSemantic(42)}, kExecution,
                                      {MakeResource(43)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::NoSharedProvenance) << selection.reason;
}

TEST(CaptureSession, AnUnresolvedBridgeIsReportedRatherThanReplaced) {
    // A shared provenance id exists, but the bridge did not resolve. The selection must not fall
    // back to another draw.
    const auto selection = SelectPair(kExecution, {MakeSemantic(42, false)}, kExecution,
                                      {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::BridgeUnresolved) << selection.reason;
    EXPECT_NE(selection.reason.find("bridge"), std::string::npos) << selection.reason;
}

TEST(CaptureSession, ADisagreeingAllocationIsReported) {
    const auto selection = SelectPair(kExecution, {MakeSemantic(42, true, 0x2015e50000ull)},
                                      kExecution, {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::BridgeUnresolved) << selection.reason;
}

TEST(CaptureSession, MultipleCompatiblePairsAreAmbiguous) {
    // Two draws share a provenance id and both resolve. The choice is not determined.
    const auto selection = SelectPair(kExecution, {MakeSemantic(42), MakeSemantic(42)},
                                      kExecution, {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::Ambiguous) << selection.reason;
}

TEST(CaptureSession, AnEmptySideIsReported) {
    EXPECT_EQ(SelectPair(kExecution, {}, kExecution, {MakeResource(42)}).outcome,
              SelectionOutcome::Empty);
    EXPECT_EQ(SelectPair(kExecution, {MakeSemantic(42)}, kExecution, {}).outcome,
              SelectionOutcome::Empty);
}

TEST(CaptureSession, TheCorrectPairIsChosenAmongSeveral) {
    // Several candidates on each side, only one of which is compatible.
    const auto selection = SelectPair(
        kExecution, {MakeSemantic(10), MakeSemantic(42), MakeSemantic(77)}, kExecution,
        {MakeResource(10, 0x3000000000ull), MakeResource(42), MakeResource(77, 0x4000000000ull)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::Selected) << selection.reason;
    EXPECT_EQ(selection.provenance_id, 42u);
}

TEST(CaptureSession, AZeroExecutionIdDoesNotBlockTheSelection) {
    // An observer that did not record an execution id cannot be compared, but it must not be
    // treated as a mismatch either: the bridge still has to resolve.
    const auto selection = SelectPair(0, {MakeSemantic(42)}, 0, {MakeResource(42)});
    EXPECT_EQ(selection.outcome, SelectionOutcome::Selected) << selection.reason;
}

TEST(CaptureSession, EveryOutcomeHasAStableName) {
    for (const auto outcome :
         {SelectionOutcome::Selected, SelectionOutcome::NoSharedProvenance,
          SelectionOutcome::BridgeUnresolved, SelectionOutcome::Ambiguous,
          SelectionOutcome::DifferentExecution, SelectionOutcome::Empty}) {
        EXPECT_NE(Core::CaptureSession::SelectionOutcomeName(outcome), "unknown");
    }
}

} // namespace
