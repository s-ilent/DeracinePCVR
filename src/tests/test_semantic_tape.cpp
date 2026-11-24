// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the semantic command tape contract.
//
// These exercise the production functions the exporter and the interpreter both call, not a
// reimplementation of their formulas. Where a case needs a real capture that is not available in a
// unit test, the test is labelled synthetic and says so.

#include "core/semantic_tape_contract.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>

namespace {

using Core::SemanticTape::AppendOperation;
using Core::SemanticTape::AppendResource;
using Core::SemanticTape::AppendUnresolved;
using Core::SemanticTape::CheckExecutable;
using Core::SemanticTape::DeriveDraw;
using Core::SemanticTape::DerivedDraw;
using Core::SemanticTape::DrawIndexedPayload;
using Core::SemanticTape::Evidence;
using Core::SemanticTape::FindResource;
using Core::SemanticTape::IndicesPerPrimitive;
using Core::SemanticTape::MaxTapeOperations;
using Core::SemanticTape::OpKind;
using Core::SemanticTape::Operation;
using Core::SemanticTape::PrimitiveTypeTriangleList;
using Core::SemanticTape::PrimitiveTypeTriangleStrip;
using Core::SemanticTape::Resource;
using Core::SemanticTape::ResourceKind;
using Core::SemanticTape::ResourceProvenance;
using Core::SemanticTape::SchemaVersion;
using Core::SemanticTape::Tape;
using Core::SemanticTape::TapeProblem;
using Core::SemanticTape::TapeProblemName;
using Core::SemanticTape::ValidateTape;
using Core::SemanticTape::BuildOutcome;
using Core::SemanticTape::BuildOutcomeName;
using Core::SemanticTape::BuildTapeFromCapture;
using Core::SemanticTape::CapturedCommandRecord;
using Core::SemanticTape::CapturedDrawCandidate;
using Core::SemanticTape::DerivedMatchesPayload;
using Core::SemanticTape::DrawIdentity;
using Core::SemanticTape::DrawMatchOutcome;
using Core::SemanticTape::DrawMatchOutcomeName;
using Core::SemanticTape::MatchDrawCommand;
using Core::SemanticTape::MaxTapeResources;
using Core::SemanticTape::MaxTapeUnresolved;
using Core::SemanticTape::TapeSource;
using Core::SemanticTape::CaptureCommandDrawIndexedPrimitive;
using Core::SemanticTape::CaptureCommandSetRenderTargets;
using Core::SemanticTape::CaptureCommandSetStreamSource;
using Core::SemanticTape::CaptureCommandSetViewport;
using Core::SemanticTape::CollectProblems;
using Core::SemanticTape::TapeProblems;

/// The classroom draw captured by SC-09H, as the deferred command payload carried it.
///
/// These values are the real ones: NumVertices 593, NumPrimitives 1012, NumInstances 2, StartIndex
/// 0, BaseVertexIndex 0, EPrimitiveType 0 (PT_TriangleList). The derived index count of 3036 is
/// what the downstream GNM draw reported, which is what makes this a conversion test rather than a
/// formula test.
DrawIndexedPayload ClassroomPayload() {
    DrawIndexedPayload payload{};
    payload.index_buffer_resource = 1;
    payload.primitive_type = PrimitiveTypeTriangleList;
    payload.base_vertex_index = 0;
    payload.first_instance = 0;
    payload.num_vertices = 593;
    payload.start_index = 0;
    payload.num_primitives = 1012;
    payload.num_instances = 2;
    return payload;
}

/// A minimal tape that passes structural validation and is executable.
Tape ExecutableTape() {
    Tape tape{};
    tape.schema_version = SchemaVersion;
    tape.provenance_id = 82;

    Resource index{};
    index.id = 1;
    index.kind = ResourceKind::IndexBuffer;
    index.provenance = ResourceProvenance::Ue4Object;
    index.element_size = 2;
    (void)AppendResource(tape, index);

    Resource target{};
    target.id = 2;
    target.kind = ResourceKind::RenderTarget;
    target.provenance = ResourceProvenance::Ue4Object;
    (void)AppendResource(tape, target);

    Operation viewport{};
    viewport.kind = OpKind::SetViewport;
    viewport.evidence = Evidence::RuntimeConfirmed;
    viewport.viewport_max_x = 1024.0f;
    viewport.viewport_max_y = 1024.0f;
    viewport.viewport_max_z = 1.0f;
    (void)AppendOperation(tape, viewport);

    Operation targets{};
    targets.kind = OpKind::SetRenderTargets;
    targets.evidence = Evidence::VerifiedTargetBinary;
    targets.color_target_count = 1;
    targets.color_targets[0] = 2;
    (void)AppendOperation(tape, targets);

    Operation draw{};
    draw.kind = OpKind::DrawIndexed;
    draw.evidence = Evidence::VerifiedTargetBinary;
    draw.draw = ClassroomPayload();
    draw.derived = DeriveDraw(draw.draw, 2);
    (void)AppendOperation(tape, draw);

    return tape;
}

// ---------------------------------------------------------------------------------------------
// Draw conversion
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeDraw, TriangleListIndexCountIsThreePerPrimitive) {
    const DerivedDraw derived = DeriveDraw(ClassroomPayload(), 2);
    ASSERT_TRUE(derived.valid);
    EXPECT_EQ(derived.index_count, 3036u);
    EXPECT_EQ(derived.first_index, 0u);
    EXPECT_EQ(derived.vertex_offset, 0);
    EXPECT_EQ(derived.instance_count, 2u);
    EXPECT_EQ(derived.index_byte_offset, 0u);
}

TEST(SemanticTapeDraw, TriangleStripConsumesTwoMoreIndicesThanPrimitives) {
    DrawIndexedPayload payload = ClassroomPayload();
    payload.primitive_type = PrimitiveTypeTriangleStrip;
    payload.num_primitives = 10;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    ASSERT_TRUE(derived.valid);
    EXPECT_EQ(derived.index_count, 12u);
}

TEST(SemanticTapeDraw, EmptyTriangleStripIsRefusedRatherThanDerivedAsTwo) {
    DrawIndexedPayload payload = ClassroomPayload();
    payload.primitive_type = PrimitiveTypeTriangleStrip;
    payload.num_primitives = 0;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    EXPECT_FALSE(derived.valid);
    EXPECT_FALSE(derived.reason.empty());
}

TEST(SemanticTapeDraw, StartIndexBecomesAnIndexOffsetNotAByteOffset) {
    // The PS4 RHI multiplies StartIndex by the element size when it forms the hardware address, so
    // the semantic value stays an index offset and the byte offset is derived separately.
    DrawIndexedPayload payload = ClassroomPayload();
    payload.start_index = 168;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    ASSERT_TRUE(derived.valid);
    EXPECT_EQ(derived.first_index, 168u);
    EXPECT_EQ(derived.index_byte_offset, 336u);
    // The index count is unaffected by where the indices start.
    EXPECT_EQ(derived.index_count, 3036u);
}

TEST(SemanticTapeDraw, StartIndexByteOffsetScalesWithElementWidth) {
    DrawIndexedPayload payload = ClassroomPayload();
    payload.start_index = 100;
    const DerivedDraw wide = DeriveDraw(payload, 4);
    ASSERT_TRUE(wide.valid);
    EXPECT_EQ(wide.index_byte_offset, 400u);
    const DerivedDraw narrow = DeriveDraw(payload, 2);
    ASSERT_TRUE(narrow.valid);
    EXPECT_EQ(narrow.index_byte_offset, 200u);
}

TEST(SemanticTapeDraw, NegativeBaseVertexIsPreservedAsSigned) {
    DrawIndexedPayload payload = ClassroomPayload();
    payload.base_vertex_index = -7;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    ASSERT_TRUE(derived.valid);
    EXPECT_EQ(derived.vertex_offset, -7);
}

TEST(SemanticTapeDraw, UnknownIndexWidthIsRefusedRatherThanGuessed) {
    for (const u32 width : {0u, 1u, 3u, 8u}) {
        const DerivedDraw derived = DeriveDraw(ClassroomPayload(), width);
        EXPECT_FALSE(derived.valid) << "width " << width << " should be refused";
        EXPECT_FALSE(derived.reason.empty());
    }
}

TEST(SemanticTapeDraw, UnknownTopologyIsRefusedRatherThanGuessed) {
    // PT_PointList (2) is not converted by this tape: a point consumes no index triple.
    DrawIndexedPayload payload = ClassroomPayload();
    payload.primitive_type = 2;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    EXPECT_FALSE(derived.valid);
    EXPECT_EQ(IndicesPerPrimitive(2), 0u);
}

TEST(SemanticTapeDraw, Ue4TriangleListIsZeroNotTheGpuTopology) {
    // UE4's EPrimitiveType begins PT_TriangleList = 0. The GPU's VGT topology numbers triangle list
    // as 4. Conflating them is the defect SC-07R identified, so the tape uses the UE4 value and the
    // conversion is checked against the downstream index count rather than against the GPU number.
    EXPECT_EQ(PrimitiveTypeTriangleList, 0u);
    EXPECT_EQ(PrimitiveTypeTriangleStrip, 1u);
    EXPECT_EQ(IndicesPerPrimitive(0), 3u);
    EXPECT_EQ(IndicesPerPrimitive(1), 1u);
}

TEST(SemanticTapeDraw, ClassroomConversionMatchesTheDownstreamIndexCount) {
    // The strongest available check on the conversion: the UE4 payload says 1012 primitives of a
    // triangle list, and the downstream GNM draw for the same execution reported 3036 indices.
    // If the conversion were wrong, these would disagree.
    const DerivedDraw derived = DeriveDraw(ClassroomPayload(), 2);
    ASSERT_TRUE(derived.valid);
    EXPECT_EQ(derived.index_count, 3036u);
    EXPECT_EQ(derived.instance_count, 2u);
}

TEST(SemanticTapeDraw, PrimitiveCountOverflowIsRefusedRatherThanWrapping) {
    DrawIndexedPayload payload = ClassroomPayload();
    payload.num_primitives = 0xFFFFFFFFu;
    const DerivedDraw derived = DeriveDraw(payload, 2);
    EXPECT_FALSE(derived.valid);
}

// ---------------------------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeValidate, ExecutableTapePasses) {
    const Tape tape = ExecutableTape();
    EXPECT_EQ(ValidateTape(tape), TapeProblem::None);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::None);
}

TEST(SemanticTapeValidate, SchemaMismatchIsRefused) {
    Tape tape = ExecutableTape();
    tape.schema_version = SchemaVersion + 1;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::SchemaMismatch);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::SchemaMismatch);
}

TEST(SemanticTapeValidate, TapeWithNoDrawIsRefused) {
    Tape tape = ExecutableTape();
    tape.operation_count = 2; // drop the draw
    EXPECT_EQ(ValidateTape(tape), TapeProblem::NoDraw);
}

TEST(SemanticTapeValidate, DuplicateResourceIdIsRefused) {
    Tape tape = ExecutableTape();
    Resource duplicate{};
    duplicate.id = 1;
    (void)AppendResource(tape, duplicate);
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DuplicateResourceId);
}

TEST(SemanticTapeValidate, ZeroResourceIdIsRefused) {
    Tape tape = ExecutableTape();
    Resource zero{};
    zero.id = 0;
    (void)AppendResource(tape, zero);
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DuplicateResourceId);
}

TEST(SemanticTapeValidate, DanglingIndexResourceIsRefused) {
    Tape tape = ExecutableTape();
    tape.operations[2].draw.index_buffer_resource = 99;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DanglingResourceReference);
}

TEST(SemanticTapeValidate, MissingIndexResourceIsRefused) {
    Tape tape = ExecutableTape();
    tape.operations[2].draw.index_buffer_resource = 0;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::IndexResourceMissing);
}

TEST(SemanticTapeValidate, DanglingRenderTargetIsRefused) {
    Tape tape = ExecutableTape();
    tape.operations[1].color_targets[0] = 77;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DanglingResourceReference);
}

TEST(SemanticTapeValidate, TwoDrawsOnOneTapeAreRefused) {
    // A tape describes one draw. A second draw means the tape is not the ordered program it claims.
    Tape tape = ExecutableTape();
    Operation second{};
    second.kind = OpKind::DrawIndexed;
    second.draw = ClassroomPayload();
    second.derived = DeriveDraw(second.draw, 2);
    (void)AppendOperation(tape, second);
    EXPECT_EQ(ValidateTape(tape), TapeProblem::OperationOutOfOrder);
}

// ---------------------------------------------------------------------------------------------
// Executability: state that must precede the draw
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeExecutable, MissingViewportIsReportedNotDefaulted) {
    Tape tape = ExecutableTape();
    // Remove the viewport by shifting the remaining operations down.
    for (u32 i = 1; i < tape.operation_count; ++i)
        tape.operations[i - 1] = tape.operations[i];
    --tape.operation_count;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::None);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoViewport);
}

TEST(SemanticTapeExecutable, MissingRenderTargetsIsReportedNotDefaulted) {
    Tape tape = ExecutableTape();
    // Remove the render-target operation, keeping the viewport.
    tape.operations[1] = tape.operations[2];
    --tape.operation_count;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::None);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoRenderTargets);
}

TEST(SemanticTapeExecutable, StateAfterTheDrawDoesNotCount) {
    // A viewport that appears after the draw cannot be state the draw consumed.
    Tape tape{};
    tape.schema_version = SchemaVersion;
    tape.provenance_id = 1;

    Resource index{};
    index.id = 1;
    index.kind = ResourceKind::IndexBuffer;
    index.provenance = ResourceProvenance::Ue4Object;
    index.element_size = 2;
    (void)AppendResource(tape, index);

    Operation draw{};
    draw.kind = OpKind::DrawIndexed;
    draw.draw = ClassroomPayload();
    draw.derived = DeriveDraw(draw.draw, 2);
    (void)AppendOperation(tape, draw);

    Operation viewport{};
    viewport.kind = OpKind::SetViewport;
    (void)AppendOperation(tape, viewport);

    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoViewport);
}

TEST(SemanticTapeExecutable, UnresolvedIndexResourceIsNotExecutable) {
    Tape tape = ExecutableTape();
    tape.resources[0].provenance = ResourceProvenance::Unresolved;
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::IndexResourceUnresolved);
}

TEST(SemanticTapeExecutable, DownstreamOracleIndexResourceIsExecutable) {
    // A resource recovered through the oracle is a weaker claim than a UE4 object, but it is still
    // resolved, so it does not block execution. The distinction is recorded, not enforced here.
    Tape tape = ExecutableTape();
    tape.resources[0].provenance = ResourceProvenance::DownstreamOracle;
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::None);
}

TEST(SemanticTapeExecutable, UnderivableDrawIsReported) {
    // An invalid derivation is also an inconsistency with the payload, so ValidateTape refuses it
    // first. Both facts are true; the structural check is the one that fires.
    Tape tape = ExecutableTape();
    tape.operations[2].derived = {};
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DerivedInconsistent);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::DerivedInconsistent);
}

TEST(SemanticTapeExecutable, AnUnderivablePayloadIsReportedAsSuch) {
    // A payload whose topology this tape cannot convert is not an inconsistency: the derivation is
    // invalid on both sides, so the structural check passes and executability reports the real
    // reason.
    Tape tape = ExecutableTape();
    tape.operations[2].draw.primitive_type = 2; // PT_PointList
    tape.operations[2].derived = DeriveDraw(tape.operations[2].draw, 2);
    EXPECT_EQ(ValidateTape(tape), TapeProblem::None);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::DrawNotDerivable);
}

// ---------------------------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeBounds, OperationBoundIsEnforcedRatherThanOverrun) {
    Tape tape{};
    Operation operation{};
    operation.kind = OpKind::Opaque;
    for (u32 i = 0; i < MaxTapeOperations; ++i)
        ASSERT_TRUE(AppendOperation(tape, operation));
    EXPECT_FALSE(AppendOperation(tape, operation));
    EXPECT_EQ(tape.operation_count, MaxTapeOperations);
}

TEST(SemanticTapeBounds, ResourceBoundIsEnforcedRatherThanOverrun) {
    Tape tape{};
    Resource resource{};
    u32 appended = 0;
    while (AppendResource(tape, resource))
        ++appended;
    EXPECT_EQ(appended, tape.resources.size());
}

TEST(SemanticTapeBounds, UnresolvedBoundIsEnforcedRatherThanOverrun) {
    Tape tape{};
    Core::SemanticTape::UnresolvedDependency dependency{};
    u32 appended = 0;
    while (AppendUnresolved(tape, dependency))
        ++appended;
    EXPECT_EQ(appended, tape.unresolved.size());
}

// ---------------------------------------------------------------------------------------------
// Resource lookup
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeResources, FindResourceResolvesById) {
    const Tape tape = ExecutableTape();
    const Resource* found = FindResource(tape, 1);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->kind, ResourceKind::IndexBuffer);
    EXPECT_EQ(FindResource(tape, 12345), nullptr);
}

TEST(SemanticTapeResources, CommandAndResourceProvenanceStaySeparate) {
    // The phase's central distinction: a UE4-origin command can reference a resource whose identity
    // came from the downstream oracle. The tape must be able to say both at once.
    Tape tape = ExecutableTape();
    tape.operations[2].evidence = Evidence::VerifiedTargetBinary;
    tape.resources[0].provenance = ResourceProvenance::DownstreamOracle;
    EXPECT_EQ(tape.operations[2].evidence, Evidence::VerifiedTargetBinary);
    EXPECT_EQ(tape.resources[0].provenance, ResourceProvenance::DownstreamOracle);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::None);
}

TEST(SemanticTapeResources, PackageReferenceIsRecordedSeparatelyFromProvenance) {
    Tape tape = ExecutableTape();
    tape.resources[0].package_resource_ref = 11;
    EXPECT_EQ(tape.resources[0].package_resource_ref, 11u);
    EXPECT_EQ(tape.resources[0].provenance, ResourceProvenance::Ue4Object);
}

// ---------------------------------------------------------------------------------------------
// Problem naming
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeProblems, EveryProblemHasAStableName) {
    const std::array<TapeProblem, 11> problems{
        TapeProblem::None,          TapeProblem::SchemaMismatch,
        TapeProblem::NoDraw,        TapeProblem::DrawNotDerivable,
        TapeProblem::IndexResourceMissing, TapeProblem::IndexResourceUnresolved,
        TapeProblem::NoViewport,    TapeProblem::NoRenderTargets,
        TapeProblem::DuplicateResourceId, TapeProblem::DanglingResourceReference,
        TapeProblem::OperationOutOfOrder,
    };
    for (const auto problem : problems) {
        const auto name = TapeProblemName(problem);
        EXPECT_FALSE(name.empty());
        EXPECT_NE(name, "unknown") << "problem " << static_cast<int>(problem);
    }
}


// ---------------------------------------------------------------------------------------------
// Draw identity matching
//
// The defect these cover: the live exporter compared the capture-slot ordinal against the
// deferred-command ordinal. Those are different counters, so it selected the wrong command.
// ---------------------------------------------------------------------------------------------

CapturedDrawCandidate Candidate(u32 index, u64 list, u64 resource, u32 primitives,
                                u32 instances, u32 vertices, u32 start_index = 0,
                                u32 primitive_type = 0) {
    CapturedDrawCandidate candidate{};
    candidate.capture_index = index;
    candidate.command_list = list;
    candidate.index_resource = resource;
    candidate.primitive_type = primitive_type;
    candidate.num_primitives = primitives;
    candidate.num_instances = instances;
    candidate.num_vertices = vertices;
    candidate.start_index = start_index;
    return candidate;
}

TEST(SemanticTapeIdentity, ResolvesByListAndIndexResource) {
    const std::array<CapturedDrawCandidate, 3> candidates{
        Candidate(0, 0x100, 0xAAA, 3, 1, 10),
        Candidate(1, 0x100, 0xBBB, 1012, 2, 593),
        Candidate(2, 0x200, 0xBBB, 5, 1, 20),
    };
    DrawIdentity identity{};
    identity.command_list = 0x100;
    identity.index_resource = 0xBBB;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched), DrawMatchOutcome::Matched);
    EXPECT_EQ(matched, 1u);
}

TEST(SemanticTapeIdentity, ADrawOnAnotherListIsNotSelected) {
    const std::array<CapturedDrawCandidate, 3> candidates{
        Candidate(0, 0x100, 0xAAA, 3, 1, 10),
        Candidate(1, 0x100, 0xBBB, 1012, 2, 593),
        Candidate(2, 0x200, 0xBBB, 5, 1, 20),
    };
    DrawIdentity identity{};
    identity.command_list = 0x200;
    identity.index_resource = 0xBBB;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched), DrawMatchOutcome::Matched);
    EXPECT_EQ(matched, 2u);
}

TEST(SemanticTapeIdentity, AnAbsentDrawIsRefusedRatherThanGuessed) {
    const std::array<CapturedDrawCandidate, 1> candidates{Candidate(0, 0x100, 0xAAA, 3, 1, 10)};
    DrawIdentity identity{};
    identity.command_list = 0x300;
    identity.index_resource = 0xAAA;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched), DrawMatchOutcome::NoCandidate);
}

TEST(SemanticTapeIdentity, TwoDrawsSharingListAndResourceAreDisambiguatedByPayload) {
    // Two draws on the same list with the same index buffer, differing only in primitive count.
    const std::array<CapturedDrawCandidate, 2> candidates{
        Candidate(0, 0x100, 0xAAA, 1012, 2, 593),
        Candidate(1, 0x100, 0xAAA, 500, 1, 300),
    };
    DrawIdentity identity{};
    identity.command_list = 0x100;
    identity.index_resource = 0xAAA;
    identity.num_primitives = 500;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched), DrawMatchOutcome::Matched);
    EXPECT_EQ(matched, 1u);
}

TEST(SemanticTapeIdentity, IndistinguishableDrawsAreRefusedAsAmbiguous) {
    // Same list, same resource, same payload: nothing distinguishes them, so taking the first would
    // be a guess. A wrong draw is worse than a refused one.
    const std::array<CapturedDrawCandidate, 2> candidates{
        Candidate(0, 0x100, 0xAAA, 1012, 2, 593),
        Candidate(1, 0x100, 0xAAA, 1012, 2, 593),
    };
    DrawIdentity identity{};
    identity.command_list = 0x100;
    identity.index_resource = 0xAAA;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched), DrawMatchOutcome::Ambiguous);
}

TEST(SemanticTapeIdentity, AZeroIdentityIsUnusableRatherThanMatchingEverything) {
    const std::array<CapturedDrawCandidate, 1> candidates{Candidate(0, 0x100, 0xAAA, 3, 1, 10)};
    DrawIdentity identity{};
    identity.command_list = 0;
    identity.index_resource = 0xAAA;
    u32 matched = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched),
              DrawMatchOutcome::UnusableIdentity);

    identity.command_list = 0x100;
    identity.index_resource = 0;
    EXPECT_EQ(MatchDrawCommand(candidates, identity, matched),
              DrawMatchOutcome::UnusableIdentity);
}

TEST(SemanticTapeIdentity, EveryOutcomeHasAStableName) {
    for (const auto outcome : {DrawMatchOutcome::Matched, DrawMatchOutcome::NoCandidate,
                               DrawMatchOutcome::Ambiguous,
                               DrawMatchOutcome::UnusableIdentity}) {
        EXPECT_NE(DrawMatchOutcomeName(outcome), "unknown");
    }
}

// ---------------------------------------------------------------------------------------------
// Sequence-counter independence
//
// The capture-slot ordinal and the deferred-command ordinal are different counters. The matcher
// must not depend on either, so varying them independently must not change the selection.
// ---------------------------------------------------------------------------------------------

CapturedCommandRecord DrawRecord(u64 sequence, u64 list, u64 resource, u32 primitives,
                                 u32 instances, u32 vertices) {
    CapturedCommandRecord record{};
    record.sequence = sequence;
    record.command_id = CaptureCommandDrawIndexedPrimitive;
    record.command_list = list;
    record.payload[0] = resource;
    record.payload[1] = 0; // PT_TriangleList
    record.payload[2] = 0;
    record.payload[3] = 0;
    record.payload[4] = vertices;
    record.payload[5] = 0;
    record.payload[6] = primitives;
    record.payload[7] = instances;
    return record;
}

TapeSource ClassroomSource() {
    TapeSource source{};
    source.provenance_id = 82;
    source.command_list = 0x100;
    source.index_resource = 0xAAA;
    source.ue4_primitive_type = 0;
    source.index_element_size = 2;
    source.submission_id = 8913;
    source.packet_offset = 0x1B64;
    source.packet_offset_known = true;
    return source;
}

TEST(SemanticTapeSequenceIndependence, SelectionDoesNotDependOnEitherCounter) {
    // The same draw, recorded at different capture slots. The provenance record carries a deferred
    // ordinal that matches none of them, which is exactly the live situation.
    for (const u64 slot : {1ull, 7ull, 174ull, 9999ull}) {
        const std::array<CapturedCommandRecord, 2> records{
            DrawRecord(slot, 0x100, 0xAAA, 1012, 2, 593),
            DrawRecord(slot + 1, 0x200, 0xBBB, 5, 1, 20),
        };
        Tape tape{};
        EXPECT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok)
            << "slot " << slot;
        const Operation* draw = nullptr;
        for (u32 i = 0; i < tape.operation_count; ++i) {
            if (tape.operations[i].kind == OpKind::DrawIndexed)
                draw = &tape.operations[i];
        }
        ASSERT_NE(draw, nullptr);
        EXPECT_EQ(draw->draw.num_primitives, 1012u) << "slot " << slot;
        EXPECT_EQ(draw->draw.num_vertices, 593u) << "slot " << slot;
    }
}

TEST(SemanticTapeSequenceIndependence, TheWrongDrawIsNotSelectedWhenCountersDisagree) {
    // A decoy draw sits at the slot number the deferred ordinal would have suggested. Matching on
    // that counter would pick the decoy; matching on identity picks the real draw.
    const std::array<CapturedCommandRecord, 2> records{
        DrawRecord(635, 0x200, 0xBBB, 958, 2, 1356), // decoy at the deferred ordinal
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593), // the real draw
    };
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    const Operation* draw = nullptr;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        if (tape.operations[i].kind == OpKind::DrawIndexed)
            draw = &tape.operations[i];
    }
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->draw.num_primitives, 1012u);
    EXPECT_EQ(draw->draw.num_vertices, 593u);
}

// ---------------------------------------------------------------------------------------------
// Resource capacity
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeCapacity, TheObservedThirtyThreeResourceTapeIsSupported) {
    // The RHI-01 offline tape carries 33 resources. The contract must accommodate it.
    EXPECT_GE(MaxTapeResources, 33u);

    Tape tape{};
    for (u32 i = 0; i < 33; ++i) {
        Resource resource{};
        resource.id = i + 1;
        resource.kind = ResourceKind::VertexBuffer;
        resource.provenance = ResourceProvenance::Ue4Object;
        ASSERT_TRUE(AppendResource(tape, resource)) << "resource " << i;
    }
    EXPECT_EQ(tape.resource_count, 33u);
}

TEST(SemanticTapeCapacity, ExactlyAtCapacityIsAccepted) {
    Tape tape{};
    for (u32 i = 0; i < MaxTapeResources; ++i) {
        Resource resource{};
        resource.id = i + 1;
        ASSERT_TRUE(AppendResource(tape, resource)) << "resource " << i;
    }
    EXPECT_EQ(tape.resource_count, MaxTapeResources);
}

TEST(SemanticTapeCapacity, OneOverCapacityIsRefused) {
    Tape tape{};
    for (u32 i = 0; i < MaxTapeResources; ++i) {
        Resource resource{};
        resource.id = i + 1;
        ASSERT_TRUE(AppendResource(tape, resource));
    }
    Resource extra{};
    extra.id = MaxTapeResources + 1;
    EXPECT_FALSE(AppendResource(tape, extra));
    EXPECT_EQ(tape.resource_count, MaxTapeResources);
}

TEST(SemanticTapeCapacity, RepeatedReferencesConsumeNoAdditionalSlots) {
    // The same guest identity referenced many times must intern to one resource.
    std::array<CapturedCommandRecord, 40> records{};
    for (u32 i = 0; i < 39; ++i) {
        records[i].sequence = i + 1;
        records[i].command_id = CaptureCommandSetStreamSource;
        records[i].command_list = 0x100;
        records[i].payload[0] = 0;
        records[i].payload[1] = 0xAAA; // the same resource every time
    }
    records[39] = DrawRecord(40, 0x100, 0xAAA, 1012, 2, 593);

    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    // The index buffer and the vertex stream share one guest identity, so they intern to a single
    // resource. Forty references consume one slot, not forty.
    EXPECT_EQ(tape.resource_count, 1u);
}

TEST(SemanticTapeCapacity, LateOverflowAfterManyValidOperationsFailsExplicitly) {
    // Fill the resource table with distinct vertex streams, then add the draw. The overflow happens
    // late, after many otherwise valid operations, and must still fail rather than truncate.
    const u32 stream_count = MaxTapeResources + 4;
    std::vector<CapturedCommandRecord> records;
    records.reserve(stream_count + 1);
    for (u32 i = 0; i < stream_count; ++i) {
        CapturedCommandRecord record{};
        record.sequence = i + 1;
        record.command_id = CaptureCommandSetStreamSource;
        record.command_list = 0x100;
        record.payload[0] = i;
        record.payload[1] = 0x1000 + i; // a distinct resource each time
        records.push_back(record);
    }
    records.push_back(DrawRecord(stream_count + 1, 0x100, 0xAAA, 1012, 2, 593));

    Tape tape{};
    EXPECT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape),
              BuildOutcome::ResourceCapacityExceeded);
}

TEST(SemanticTapeCapacity, TheReaderRejectsATapeOverCapacity) {
    Tape tape = ExecutableTape();
    // Force the count past the contract without appending, which is what a corrupt file would do.
    tape.resource_count = MaxTapeResources + 1;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::ResourceCapacityExceeded);
}

TEST(SemanticTapeCapacity, TheReaderRejectsATapeOverTheOperationBound) {
    Tape tape = ExecutableTape();
    tape.operation_count = MaxTapeOperations + 1;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::OperationCapacityExceeded);
}

// ---------------------------------------------------------------------------------------------
// Derived-versus-payload consistency
//
// The UE4 semantic payload is authoritative. A tape whose stored derived values contradict it is
// inconsistent data and must be refused rather than executed.
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeConsistency, AConsistentDerivationIsAccepted) {
    const DrawIndexedPayload payload = ClassroomPayload();
    const DerivedDraw derived = DeriveDraw(payload, 2);
    EXPECT_TRUE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedPrimitiveCountIsDetected) {
    // This is the negative consistency test: change only the payload, leave the derived values.
    DrawIndexedPayload payload = ClassroomPayload();
    const DerivedDraw derived = DeriveDraw(payload, 2);
    payload.num_primitives = 500;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedIndexCountIsDetected) {
    const DrawIndexedPayload payload = ClassroomPayload();
    DerivedDraw derived = DeriveDraw(payload, 2);
    derived.index_count = 1500;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedFirstIndexIsDetected) {
    const DrawIndexedPayload payload = ClassroomPayload();
    DerivedDraw derived = DeriveDraw(payload, 2);
    derived.first_index = 42;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedVertexOffsetIsDetected) {
    const DrawIndexedPayload payload = ClassroomPayload();
    DerivedDraw derived = DeriveDraw(payload, 2);
    derived.vertex_offset = -3;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedInstanceCountIsDetected) {
    const DrawIndexedPayload payload = ClassroomPayload();
    DerivedDraw derived = DeriveDraw(payload, 2);
    derived.instance_count = 7;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, AChangedByteOffsetIsDetected) {
    const DrawIndexedPayload payload = ClassroomPayload();
    DerivedDraw derived = DeriveDraw(payload, 2);
    derived.index_byte_offset = 99;
    EXPECT_FALSE(DerivedMatchesPayload(payload, derived, 2));
}

TEST(SemanticTapeConsistency, TheTapeValidatorRejectsAnInconsistentDraw) {
    Tape tape = ExecutableTape();
    // Change only the payload. The stored derived block still says 3036.
    tape.operations[2].draw.num_primitives = 500;
    EXPECT_EQ(ValidateTape(tape), TapeProblem::DerivedInconsistent);
}

TEST(SemanticTapeConsistency, TheTapeValidatorAcceptsTheRegeneratedDraw) {
    // The positive path: change the payload and regenerate through the production derivation.
    Tape tape = ExecutableTape();
    tape.operations[2].draw.num_primitives = 500;
    tape.operations[2].derived = DeriveDraw(tape.operations[2].draw, 2);
    EXPECT_EQ(ValidateTape(tape), TapeProblem::None);
    EXPECT_EQ(tape.operations[2].derived.index_count, 1500u);
}

// ---------------------------------------------------------------------------------------------
// Producer parity: the shared builder
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeBuilder, BuildsTheClassroomDrawFromSyntheticCapture) {
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_EQ(tape.provenance_id, 82u);
    EXPECT_EQ(tape.submission_id, 8913u);
    EXPECT_EQ(tape.packet_offset, 0x1B64u);
    EXPECT_TRUE(tape.packet_offset_known);

    const Operation* draw = nullptr;
    for (u32 i = 0; i < tape.operation_count; ++i) {
        if (tape.operations[i].kind == OpKind::DrawIndexed)
            draw = &tape.operations[i];
    }
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->draw.primitive_type, 0u);
    EXPECT_EQ(draw->draw.num_primitives, 1012u);
    EXPECT_EQ(draw->draw.num_instances, 2u);
    EXPECT_EQ(draw->derived.index_count, 3036u);
    EXPECT_EQ(draw->derived.instance_count, 2u);
}

TEST(SemanticTapeBuilder, PreservesOperationOrderAndExcludesOtherLists) {
    std::array<CapturedCommandRecord, 4> records{};
    records[0].sequence = 1;
    records[0].command_id = CaptureCommandSetRenderTargets;
    records[0].command_list = 0x100;
    records[0].render_target_count = 1;
    records[0].render_targets[0] = 0xCCC;
    records[1].sequence = 2;
    records[1].command_id = CaptureCommandSetViewport;
    records[1].command_list = 0x999; // another list: must be excluded
    records[1].payload[3] = 1024;
    records[1].payload[4] = 1024;
    records[1].payload[5] = 0x3F800000;
    records[2].sequence = 3;
    records[2].command_id = CaptureCommandSetStreamSource;
    records[2].command_list = 0x100;
    records[2].payload[1] = 0xDDD;
    records[3] = DrawRecord(4, 0x100, 0xAAA, 1012, 2, 593);

    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);

    // Render targets, then the vertex stream, then the draw. The other list is absent.
    ASSERT_GE(tape.operation_count, 3u);
    EXPECT_EQ(tape.operations[0].kind, OpKind::SetRenderTargets);
    EXPECT_EQ(tape.operations[1].kind, OpKind::BindVertexBuffers);
    EXPECT_EQ(tape.operations[2].kind, OpKind::DrawIndexed);
    for (u32 i = 0; i < tape.operation_count; ++i) {
        EXPECT_NE(tape.operations[i].kind, OpKind::SetViewport)
            << "a viewport from another list must not be merged in";
    }
}

TEST(SemanticTapeBuilder, ReportsAMissingViewportRatherThanInventingOne) {
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    bool found = false;
    for (u32 i = 0; i < tape.unresolved_count; ++i) {
        if (tape.unresolved[i].what == "viewport")
            found = true;
    }
    EXPECT_TRUE(found);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoViewport);
}

TEST(SemanticTapeBuilder, InsertsAnInheritedViewportSnapshotBeforeTheDraw) {
    // The RHI caches the viewport on the context, so a draw can inherit one set earlier on another
    // list. The snapshot read at draw entry is inserted as state the draw consumed.
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    TapeSource source = ClassroomSource();
    source.viewport_valid = true;
    source.viewport_min_x = 0.0f;
    source.viewport_min_y = 0.0f;
    source.viewport_min_z = 0.0f;
    source.viewport_max_x = 2880.0f;
    source.viewport_max_y = 3240.0f;
    source.viewport_max_z = 1.0f;

    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, source, tape), BuildOutcome::Ok);

    // The viewport must precede the draw, and carry the snapshot values.
    ASSERT_GE(tape.operation_count, 2u);
    EXPECT_EQ(tape.operations[0].kind, OpKind::SetViewport);
    EXPECT_EQ(tape.operations[0].evidence, Evidence::RuntimeConfirmed);
    EXPECT_FLOAT_EQ(tape.operations[0].viewport_max_x, 2880.0f);
    EXPECT_FLOAT_EQ(tape.operations[0].viewport_max_y, 3240.0f);
    EXPECT_EQ(tape.operations[1].kind, OpKind::DrawIndexed);
}

TEST(SemanticTapeBuilder, ReportsMissingRenderTargets) {
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    TapeSource source = ClassroomSource();
    source.viewport_valid = true;
    source.viewport_max_x = 640.0f;
    source.viewport_max_y = 640.0f;
    source.viewport_max_z = 1.0f;
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, source, tape), BuildOutcome::Ok);
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoRenderTargets);
}

TEST(SemanticTapeBuilder, ReportsMultipleUnresolvedDependencies) {
    // A tape missing several state dimensions must report all of them, not just the first.
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_GE(tape.unresolved_count, 3u);
    bool viewport = false;
    bool pipeline = false;
    bool shaders = false;
    for (u32 i = 0; i < tape.unresolved_count; ++i) {
        const auto what = tape.unresolved[i].what;
        if (what == "viewport")
            viewport = true;
        if (what == "graphics pipeline")
            pipeline = true;
        if (what == "shader resources")
            shaders = true;
    }
    EXPECT_TRUE(viewport);
    EXPECT_TRUE(pipeline);
    EXPECT_TRUE(shaders);
}

TEST(SemanticTapeBuilder, RefusesAnEmptyCapture) {
    const std::array<CapturedCommandRecord, 0> records{};
    Tape tape{};
    EXPECT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::NoCapture);
}

TEST(SemanticTapeBuilder, RefusesAnUnresolvableDraw) {
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x200, 0xBBB, 1012, 2, 593)};
    Tape tape{};
    EXPECT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape),
              BuildOutcome::DrawUnresolved);
}

TEST(SemanticTapeBuilder, EveryBuildOutcomeHasAStableName) {
    for (const auto outcome : {BuildOutcome::Ok, BuildOutcome::NoCapture,
                               BuildOutcome::DrawUnresolved,
                               BuildOutcome::ResourceCapacityExceeded,
                               BuildOutcome::OperationCapacityExceeded,
                               BuildOutcome::InvalidTape}) {
        EXPECT_NE(BuildOutcomeName(outcome), "unknown");
    }
}

// ---------------------------------------------------------------------------------------------
// Semantic mutation: the payload drives the executed draw
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeMutation, ChangingThePayloadChangesTheDerivedCount) {
    // 1012 triangles produce 3036 indices; 500 produce 1500. The derivation is the production one.
    const std::array<CapturedCommandRecord, 1> records{
        DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593)};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_EQ(tape.operations[0].derived.index_count, 3036u);

    const std::array<CapturedCommandRecord, 1> mutated{
        DrawRecord(174, 0x100, 0xAAA, 500, 2, 593)};
    Tape mutated_tape{};
    ASSERT_EQ(BuildTapeFromCapture(mutated, ClassroomSource(), mutated_tape), BuildOutcome::Ok);
    EXPECT_EQ(mutated_tape.operations[0].derived.index_count, 1500u);
}

TEST(SemanticTapeMutation, NonzeroFirstIndexAndInstanceArePreserved) {
    CapturedCommandRecord record = DrawRecord(174, 0x100, 0xAAA, 1012, 3, 593);
    record.payload[5] = 168; // StartIndex
    record.payload[3] = 4;   // FirstInstance
    const std::array<CapturedCommandRecord, 1> records{record};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    const auto& draw = tape.operations[0];
    EXPECT_EQ(draw.draw.start_index, 168u);
    EXPECT_EQ(draw.draw.first_instance, 4u);
    EXPECT_EQ(draw.derived.first_index, 168u);
    EXPECT_EQ(draw.derived.index_byte_offset, 336u);
}

TEST(SemanticTapeMutation, NegativeBaseVertexSurvivesTheBuilder) {
    CapturedCommandRecord record = DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593);
    record.payload[2] = 0xFFFFFFF9ull; // -7 as a signed 32-bit value
    const std::array<CapturedCommandRecord, 1> records{record};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_EQ(tape.operations[0].draw.base_vertex_index, -7);
    EXPECT_EQ(tape.operations[0].derived.vertex_offset, -7);
}

TEST(SemanticTapeMutation, AnUnsupportedTopologyIsRefusedByTheDerivation) {
    CapturedCommandRecord record = DrawRecord(174, 0x100, 0xAAA, 1012, 2, 593);
    record.payload[1] = 2; // PT_PointList
    const std::array<CapturedCommandRecord, 1> records{record};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_FALSE(tape.operations[0].derived.valid);
    // The viewport is missing too, so CheckExecutable reports that first. The full problem set
    // names both, which is what a report needs.
    const auto problems = CollectProblems(tape);
    EXPECT_TRUE(problems.Has(TapeProblem::DrawNotDerivable));
    EXPECT_TRUE(problems.Has(TapeProblem::NoViewport));
    EXPECT_GE(problems.Count(), 2u);
}

TEST(SemanticTapeMutation, AnOverflowingPrimitiveCountIsRefused) {
    CapturedCommandRecord record = DrawRecord(174, 0x100, 0xAAA, 0xFFFFFFFFu, 2, 593);
    const std::array<CapturedCommandRecord, 1> records{record};
    Tape tape{};
    ASSERT_EQ(BuildTapeFromCapture(records, ClassroomSource(), tape), BuildOutcome::Ok);
    EXPECT_FALSE(tape.operations[0].derived.valid);
}

// ---------------------------------------------------------------------------------------------
// Multiple simultaneous problems
//
// A single CheckExecutable failure must not conceal another unresolved dependency. A tape missing
// its viewport, its render targets and its pipeline has three problems, and a report needs all
// three.
// ---------------------------------------------------------------------------------------------

TEST(SemanticTapeProblems, CollectsEveryMissingDimensionAtOnce) {
    // A tape with only a draw: no viewport, no render targets, no pipeline.
    Tape tape{};
    tape.schema_version = SchemaVersion;
    tape.provenance_id = 1;
    Resource index{};
    index.id = 1;
    index.kind = ResourceKind::IndexBuffer;
    index.provenance = ResourceProvenance::Ue4Object;
    index.element_size = 2;
    (void)AppendResource(tape, index);
    Operation draw{};
    draw.kind = OpKind::DrawIndexed;
    draw.draw = ClassroomPayload();
    draw.derived = DeriveDraw(draw.draw, 2);
    (void)AppendOperation(tape, draw);

    const auto problems = CollectProblems(tape);
    EXPECT_TRUE(problems.Has(TapeProblem::NoViewport));
    EXPECT_TRUE(problems.Has(TapeProblem::NoRenderTargets));
    EXPECT_TRUE(problems.Has(TapeProblem::NoGraphicsPipeline));
    EXPECT_GE(problems.Count(), 3u);

    // CheckExecutable reports only the first, which is why the collector exists.
    EXPECT_EQ(CheckExecutable(tape), TapeProblem::NoViewport);
}

TEST(SemanticTapeProblems, ACompleteTapeHasNoProblems) {
    Tape tape = ExecutableTape();
    const auto problems = CollectProblems(tape);
    // The pipeline is the one dimension the tape cannot carry, so it is always reported.
    EXPECT_TRUE(problems.Has(TapeProblem::NoGraphicsPipeline));
    EXPECT_FALSE(problems.Has(TapeProblem::NoViewport));
    EXPECT_FALSE(problems.Has(TapeProblem::NoRenderTargets));
    EXPECT_EQ(problems.Count(), 1u);
}

TEST(SemanticTapeProblems, AStructuralProblemIsIncludedAlongsideMissingState) {
    Tape tape = ExecutableTape();
    tape.operations[2].draw.num_primitives = 500; // inconsistent with the stored derived block
    const auto problems = CollectProblems(tape);
    EXPECT_TRUE(problems.Has(TapeProblem::DerivedInconsistent));
    EXPECT_TRUE(problems.Has(TapeProblem::NoGraphicsPipeline));
    EXPECT_GE(problems.Count(), 2u);
}

TEST(SemanticTapeProblems, NoGraphicsPipelineHasAStableName) {
    EXPECT_EQ(TapeProblemName(TapeProblem::NoGraphicsPipeline), "no_graphics_pipeline");
    EXPECT_EQ(TapeProblemName(TapeProblem::DerivedInconsistent), "derived_inconsistent");
    EXPECT_EQ(TapeProblemName(TapeProblem::ResourceCapacityExceeded),
              "resource_capacity_exceeded");
}
} // namespace