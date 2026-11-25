// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the global Vulkan binding resolution.
//
// The defect these exist to prevent: `GraphicsPipeline::BuildDescSetLayout` allocates one binding
// counter across every stage in logical-stage order, but the shader interface capture counted from
// zero within each stage. A fragment stage's first buffer is binding 0 while the vertex stage's
// first buffer is binding N, so the captured interface named the wrong binding for every stage after
// the first.
//
// The resolution is a pure function of the layout's stage order and per-stage counts, so it is
// tested here without a GPU.

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "core/shader_interface_capture.h"

namespace {

using Core::ShaderInterface::Descriptor;
using Core::ShaderInterface::DescriptorClass;
using Core::ShaderInterface::Interface;
using Core::ShaderInterface::ResolveGlobalBindings;

/// The logical stage values, as the shader compiler defines them. Fragment is 0 and Vertex is 3,
/// which is why the layout allocates fragment bindings first.
constexpr u32 kFragment = 0;
constexpr u32 kVertex = 3;

/// An interface with `count` descriptors, each one binding wide, counted from zero.
///
/// Each call gets a distinct program hash, because the capture deduplicates by
/// (program hash, permutation index) and two stages sharing a hash would collapse into one.
Interface MakeInterface(u32 logical_stage, u32 count) {
    static u64 next_program_hash = 1;
    Interface interface{};
    interface.program_hash = next_program_hash++;
    interface.permutation_index = 0;
    interface.permutation_hash = interface.program_hash;
    interface.logical_stage = logical_stage;
    interface.unified_binding_count = count;
    for (u32 i = 0; i < count; ++i) {
        auto& descriptor = interface.descriptors[interface.descriptor_count++];
        descriptor.local_binding = i;
        descriptor.binding = i;
        descriptor.binding_resolved = false;
        descriptor.cls = DescriptorClass::UniformBuffer;
        descriptor.count = 1;
    }
    return interface;
}

/// Resolves a layout of the given stage order and counts, returning the interfaces it produced.
std::vector<Interface> Resolve(const std::vector<u32>& order, const std::vector<u32>& counts,
                               const std::vector<Interface>& interfaces) {
    Core::ShaderInterface::ResetForTesting();
    for (const auto& interface : interfaces) {
        Core::ShaderInterface::NoteInterface(interface);
    }
    const bool ok = ResolveGlobalBindings(std::span<const u32>{order.data(), order.size()},
                                          std::span<const u32>{counts.data(), counts.size()});
    std::vector<Interface> out;
    if (!ok) {
        return out;
    }
    for (u32 i = 0; i < Core::ShaderInterface::RetainedCount(); ++i) {
        Interface resolved{};
        if (Core::ShaderInterface::GetRetained(i, resolved)) {
            out.push_back(resolved);
        }
    }
    return out;
}

TEST(GlobalBinding, AVertexStageWithOneBufferStartsAtZeroWhenItIsFirst) {
    // A layout that allocates the vertex stage first gives it binding 0.
    const auto resolved = Resolve({kVertex}, {1}, {MakeInterface(kVertex, 1)});
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_TRUE(resolved[0].stage_binding_base_known);
    EXPECT_EQ(resolved[0].stage_binding_base, 0u);
    EXPECT_EQ(resolved[0].descriptors[0].binding, 0u);
    EXPECT_TRUE(resolved[0].descriptors[0].binding_resolved);
}

TEST(GlobalBinding, AFragmentStageAfterAVertexStageIsOffsetByItsCount) {
    // The real layout order: fragment is logical stage 0, so it is allocated first.
    const auto resolved = Resolve({kFragment, kVertex}, {2, 3},
                                  {MakeInterface(kFragment, 2), MakeInterface(kVertex, 3)});
    ASSERT_EQ(resolved.size(), 2u);
    for (const auto& interface : resolved) {
        if (interface.logical_stage == kFragment) {
            EXPECT_EQ(interface.stage_binding_base, 0u);
            EXPECT_EQ(interface.descriptors[0].binding, 0u);
            EXPECT_EQ(interface.descriptors[1].binding, 1u);
        } else {
            EXPECT_EQ(interface.stage_binding_base, 2u);
            EXPECT_EQ(interface.descriptors[0].binding, 2u);
            EXPECT_EQ(interface.descriptors[1].binding, 3u);
            EXPECT_EQ(interface.descriptors[2].binding, 4u);
        }
    }
}

TEST(GlobalBinding, BothStagesDeclaringLocalBindingZeroGetDistinctGlobalBindings) {
    // This is the defect in its smallest form: two stages both call their first descriptor binding
    // zero, and only the layout knows they are different bindings.
    const auto resolved = Resolve({kFragment, kVertex}, {1, 1},
                                  {MakeInterface(kFragment, 1), MakeInterface(kVertex, 1)});
    ASSERT_EQ(resolved.size(), 2u);
    u32 fragment_binding = 99;
    u32 vertex_binding = 99;
    for (const auto& interface : resolved) {
        if (interface.logical_stage == kFragment) {
            fragment_binding = interface.descriptors[0].binding;
        } else {
            vertex_binding = interface.descriptors[0].binding;
        }
    }
    EXPECT_EQ(fragment_binding, 0u);
    EXPECT_EQ(vertex_binding, 1u);
    EXPECT_NE(fragment_binding, vertex_binding);
}

TEST(GlobalBinding, AStageOrderDifferentFromTheDefaultIsHonoured) {
    // The resolution must follow the layout's order, not an assumed one.
    const auto resolved = Resolve({kVertex, kFragment}, {2, 1},
                                  {MakeInterface(kVertex, 2), MakeInterface(kFragment, 1)});
    ASSERT_EQ(resolved.size(), 2u);
    for (const auto& interface : resolved) {
        if (interface.logical_stage == kVertex) {
            EXPECT_EQ(interface.stage_binding_base, 0u);
            EXPECT_EQ(interface.descriptors[1].binding, 1u);
        } else {
            EXPECT_EQ(interface.stage_binding_base, 2u);
            EXPECT_EQ(interface.descriptors[0].binding, 2u);
        }
    }
}

TEST(GlobalBinding, ACountThatDisagreesWithTheLayoutIsRefused) {
    // The capture and the layout describe different interfaces, so no binding number can be
    // trusted.
    const auto resolved = Resolve({kFragment, kVertex}, {2, 3},
                                  {MakeInterface(kFragment, 2), MakeInterface(kVertex, 2)});
    EXPECT_TRUE(resolved.empty());
}

TEST(GlobalBinding, AStageTheLayoutDidNotAllocateIsLeftUnresolved) {
    // A guessed number would be worse than an absent one.
    const auto resolved = Resolve({kFragment}, {1},
                                  {MakeInterface(kFragment, 1), MakeInterface(kVertex, 1)});
    ASSERT_EQ(resolved.size(), 2u);
    for (const auto& interface : resolved) {
        if (interface.logical_stage == kVertex) {
            EXPECT_FALSE(interface.stage_binding_base_known);
            EXPECT_FALSE(interface.descriptors[0].binding_resolved);
        }
    }
}

TEST(GlobalBinding, MismatchedOrderAndCountLengthsAreRefused) {
    Core::ShaderInterface::ResetForTesting();
    Core::ShaderInterface::NoteInterface(MakeInterface(kFragment, 1));
    const std::array<u32, 2> order{kFragment, kVertex};
    const std::array<u32, 1> counts{1};
    EXPECT_FALSE(ResolveGlobalBindings(order, counts));
}

TEST(GlobalBinding, TheLocalBindingIsPreservedAlongsideTheGlobalOne) {
    // Both identities are kept, because a consumer may need to name the shader's own declaration.
    const auto resolved = Resolve({kFragment, kVertex}, {2, 1},
                                  {MakeInterface(kFragment, 2), MakeInterface(kVertex, 1)});
    ASSERT_EQ(resolved.size(), 2u);
    for (const auto& interface : resolved) {
        if (interface.logical_stage == kVertex) {
            EXPECT_EQ(interface.descriptors[0].local_binding, 0u);
            EXPECT_EQ(interface.descriptors[0].binding, 2u);
        }
    }
}

} // namespace
