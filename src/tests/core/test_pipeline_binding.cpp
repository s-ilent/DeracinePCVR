// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for pipeline-scoped descriptor resolution.
//
// The defect these exist to prevent: the resolution iterated every retained interface and applied
// whichever pipeline's binding bases called it most recently. Shader permutations are reusable, so
// one permutation can serve several pipelines with different stage combinations and different
// binding bases. A mapping is therefore a property of a pipeline, not of a permutation, and a later
// pipeline must not corrupt an earlier draw's mapping.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "core/shader_interface_capture.h"

namespace {

using Core::ShaderInterface::DescriptorClass;
using Core::ShaderInterface::FindPipelineBindings;
using Core::ShaderInterface::Interface;
using Core::ShaderInterface::PipelineBinding;
using Core::ShaderInterface::PipelineStageBinding;
using Core::ShaderInterface::ResolveDescriptorBinding;
using Core::ShaderInterface::ResolvePipelineBindings;

constexpr u32 kFragment = 0;
constexpr u32 kVertex = 3;

/// A declaration with `count` descriptors, each one binding wide.
Interface MakeDeclaration(u64 program_hash, u32 logical_stage, u32 count) {
    Interface interface{};
    interface.program_hash = program_hash;
    interface.permutation_index = 0;
    interface.permutation_hash = program_hash;
    interface.logical_stage = logical_stage;
    interface.unified_binding_count = count;
    for (u32 i = 0; i < count; ++i) {
        auto& descriptor = interface.descriptors[interface.descriptor_count++];
        descriptor.local_binding = i;
        descriptor.cls = DescriptorClass::UniformBuffer;
        descriptor.count = 1;
    }
    return interface;
}

PipelineStageBinding Stage(u32 logical_stage, u64 program_hash, u32 base, u32 count) {
    PipelineStageBinding stage{};
    stage.logical_stage = logical_stage;
    stage.program_hash = program_hash;
    stage.binding_base = base;
    stage.binding_count = count;
    return stage;
}

void Reset() {
    Core::ShaderInterface::ResetForTesting();
}

TEST(PipelineBinding, OnePermutationUsedByTwoPipelinesKeepsBothMappings) {
    // The same fragment permutation serves two pipelines with different bases. Each pipeline gets
    // its own mapping, and the first is not overwritten by the second.
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 2));

    const std::array<PipelineStageBinding, 1> first{Stage(kFragment, 0xAA, 0, 2)};
    const std::array<PipelineStageBinding, 1> second{Stage(kFragment, 0xAA, 5, 2)};
    ASSERT_TRUE(ResolvePipelineBindings(1, first));
    ASSERT_TRUE(ResolvePipelineBindings(2, second));

    u32 binding = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(1, kFragment, 0, binding));
    EXPECT_EQ(binding, 0u);
    ASSERT_TRUE(ResolveDescriptorBinding(2, kFragment, 0, binding));
    EXPECT_EQ(binding, 5u);

    // The first pipeline's mapping is still intact after the second was built.
    ASSERT_TRUE(ResolveDescriptorBinding(1, kFragment, 1, binding));
    EXPECT_EQ(binding, 1u);
}

TEST(PipelineBinding, TwoPipelinesWithTheSameCountsButDifferentModulesStayDistinct) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xBB, kFragment, 1));

    const std::array<PipelineStageBinding, 1> first{Stage(kFragment, 0xAA, 0, 1)};
    const std::array<PipelineStageBinding, 1> second{Stage(kFragment, 0xBB, 3, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(10, first));
    ASSERT_TRUE(ResolvePipelineBindings(20, second));

    u32 binding = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(10, kFragment, 0, binding));
    EXPECT_EQ(binding, 0u);
    ASSERT_TRUE(ResolveDescriptorBinding(20, kFragment, 0, binding));
    EXPECT_EQ(binding, 3u);
}

TEST(PipelineBinding, CreationOrderDoesNotAffectTheResult) {
    // The same two pipelines built in the opposite order produce the same mappings.
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 2));
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xBB, kVertex, 1));

    const std::array<PipelineStageBinding, 1> fragment{Stage(kFragment, 0xAA, 0, 2)};
    const std::array<PipelineStageBinding, 1> vertex{Stage(kVertex, 0xBB, 2, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(2, vertex));
    ASSERT_TRUE(ResolvePipelineBindings(1, fragment));

    u32 binding = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(1, kFragment, 1, binding));
    EXPECT_EQ(binding, 1u);
    ASSERT_TRUE(ResolveDescriptorBinding(2, kVertex, 0, binding));
    EXPECT_EQ(binding, 2u);
}

TEST(PipelineBinding, APipelineWithASubsetOfStagesResolvesOnlyItsOwn) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xBB, kVertex, 1));

    // Only the fragment stage participates.
    const std::array<PipelineStageBinding, 1> fragment{Stage(kFragment, 0xAA, 0, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(7, fragment));

    u32 binding = 99;
    EXPECT_TRUE(ResolveDescriptorBinding(7, kFragment, 0, binding));
    // The vertex stage was not allocated, so it has no binding rather than binding zero.
    EXPECT_FALSE(ResolveDescriptorBinding(7, kVertex, 0, binding));
}

TEST(PipelineBinding, ALayoutThatDisagreesWithTheDeclarationIsRefused) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 2));

    // The layout allocated three bindings but the declaration says two.
    const std::array<PipelineStageBinding, 1> stage{Stage(kFragment, 0xAA, 0, 3)};
    EXPECT_FALSE(ResolvePipelineBindings(1, stage));

    // Nothing was stored as resolved.
    PipelineBinding out{};
    EXPECT_FALSE(FindPipelineBindings(1, out));
}

TEST(PipelineBinding, AModuleTheCaptureNeverSawIsRefused) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));

    const std::array<PipelineStageBinding, 1> stage{Stage(kFragment, 0xCC, 0, 1)};
    EXPECT_FALSE(ResolvePipelineBindings(1, stage));
}

TEST(PipelineBinding, ADrawCapturedBeforeAnotherPipelineIsStable) {
    // Pipeline 1 is used by a draw, then pipeline 2 is built, then the draw's mapping is read.
    //
    // The hashes are unique to this test: the capture deduplicates a declaration by
    // (program hash, permutation index), so a hash shared with another test would share its
    // declaration.
    Reset();
    // A stale pipeline from an earlier test must not survive the reset, and a lookup must not
    // find a slot outside the live range.
    {
        PipelineBinding stale{};
        EXPECT_FALSE(FindPipelineBindings(1, stale)) << "pipeline 1 survived the reset";
    }
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xD1, kFragment, 2));
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xD2, kFragment, 2));

    const std::array<PipelineStageBinding, 1> first{Stage(kFragment, 0xD1, 0, 2)};
    ASSERT_TRUE(ResolvePipelineBindings(1, first));

    u32 before = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(1, kFragment, 1, before));

    const std::array<PipelineStageBinding, 1> second{Stage(kFragment, 0xD2, 4, 2)};
    ASSERT_TRUE(ResolvePipelineBindings(2, second));

    u32 after = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(1, kFragment, 1, after));
    EXPECT_EQ(before, after);
    EXPECT_EQ(after, 1u);
}

TEST(PipelineBinding, APreloadedPipelineFollowedByRuntimeConstructionBothResolve) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xBB, kVertex, 1));

    // Preload allocates only the fragment stage.
    const std::array<PipelineStageBinding, 1> preload{Stage(kFragment, 0xAA, 0, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(100, preload));

    // Runtime construction allocates both.
    const std::array<PipelineStageBinding, 2> runtime{Stage(kFragment, 0xAA, 0, 1),
                                                      Stage(kVertex, 0xBB, 1, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(200, runtime));

    u32 binding = 99;
    ASSERT_TRUE(ResolveDescriptorBinding(100, kFragment, 0, binding));
    EXPECT_EQ(binding, 0u);
    ASSERT_TRUE(ResolveDescriptorBinding(200, kVertex, 0, binding));
    EXPECT_EQ(binding, 1u);
}

TEST(PipelineBinding, ALocalBindingBeyondTheStageCountIsRefused) {
    // A local binding past the stage's allocation would otherwise resolve to a number belonging to
    // the next stage.
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));

    const std::array<PipelineStageBinding, 1> stage{Stage(kFragment, 0xAA, 0, 1)};
    ASSERT_TRUE(ResolvePipelineBindings(1, stage));

    u32 binding = 99;
    EXPECT_FALSE(ResolveDescriptorBinding(1, kFragment, 1, binding));
}

TEST(PipelineBinding, AnUnknownPipelineHasNoBinding) {
    Reset();
    u32 binding = 99;
    EXPECT_FALSE(ResolveDescriptorBinding(999, kFragment, 0, binding));
}

TEST(PipelineBinding, AZeroPipelineIdentityIsRefused) {
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 1));
    const std::array<PipelineStageBinding, 1> stage{Stage(kFragment, 0xAA, 0, 1)};
    EXPECT_FALSE(ResolvePipelineBindings(0, stage));
}

TEST(PipelineBinding, TheDeclarationIsNotMutatedByResolution) {
    // The shared declaration must stay immutable: a pipeline's mapping lives in its own record.
    Reset();
    Core::ShaderInterface::NoteInterface(MakeDeclaration(0xAA, kFragment, 2));

    const std::array<PipelineStageBinding, 1> stage{Stage(kFragment, 0xAA, 7, 2)};
    ASSERT_TRUE(ResolvePipelineBindings(1, stage));

    Interface declaration{};
    ASSERT_TRUE(Core::ShaderInterface::FindByPermutationHash(0xAA, declaration));
    // The declaration still reports its own local bindings, not the pipeline's global ones.
    EXPECT_EQ(declaration.descriptors[0].local_binding, 0u);
    EXPECT_FALSE(declaration.descriptors[0].binding_resolved);
}

} // namespace
