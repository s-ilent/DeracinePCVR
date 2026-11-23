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
    Tape tape = ExecutableTape();
    tape.operations[2].derived = {};
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

} // namespace