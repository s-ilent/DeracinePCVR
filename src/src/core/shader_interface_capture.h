// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::ShaderInterface {

/// Opt-in for retaining the descriptor interface of each compiled shader permutation. Separate from
/// the binary capture so the interface can be inspected without retaining the modules.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_SHADER_INTERFACE";

/// Bounded number of retained interfaces. One per (program hash, permutation index).
inline constexpr size_t MaxRetainedInterfaces = 512;

/// Bounded descriptors per interface. The widest observed stage declares far fewer; the bound keeps
/// the table a fixed array.
inline constexpr size_t MaxDescriptorsPerStage = 64;

/// What a descriptor is, in the vocabulary the SPIR-V uses.
enum class DescriptorClass : u8 {
    Unknown = 0,
    UniformBuffer = 1,
    StorageBuffer = 2,
    SampledImage = 3,
    StorageImage = 4,
    Sampler = 5,
    /// A combined image sampler, which the production layout does not emit but a patched module
    /// may declare.
    CombinedImageSampler = 6,
};

[[nodiscard]] constexpr std::string_view DescriptorClassName(DescriptorClass cls) noexcept {
    switch (cls) {
    case DescriptorClass::UniformBuffer:
        return "uniform_buffer";
    case DescriptorClass::StorageBuffer:
        return "storage_buffer";
    case DescriptorClass::SampledImage:
        return "sampled_image";
    case DescriptorClass::StorageImage:
        return "storage_image";
    case DescriptorClass::Sampler:
        return "sampler";
    case DescriptorClass::CombinedImageSampler:
        return "combined_image_sampler";
    default:
        return "unknown";
    }
}

/// One descriptor declaration.
struct Descriptor {
    /// The descriptor set. The production layout uses a single set, so this is 0 for every binding
    /// it emits; it is recorded rather than assumed so a patched module declaring another set is
    /// visible.
    u32 set{};
    /// The shader-local binding, counted from zero within this stage's own declaration order.
    ///
    /// This is NOT the Vulkan binding number. `GraphicsPipeline::BuildDescSetLayout` allocates one
    /// counter across every stage in logical-stage order, so a fragment stage's first buffer is
    /// binding 0 while the vertex stage's first buffer is binding N. The two identities are kept
    /// separate because conflating them makes a package's descriptor correspondence wrong for every
    /// stage after the first.
    u32 local_binding{};
    /// The effective Vulkan binding number, resolved once the full pipeline layout is known.
    ///
    /// Zero is a legitimate binding, so `binding_resolved` says whether this was established rather
    /// than leaving a zero to be read as either.
    u32 binding{};
    bool binding_resolved{};
    DescriptorClass cls{DescriptorClass::Unknown};
    /// Array elements. 1 for a non-array binding.
    u32 count{};
    /// The shader stages that can see this binding, as a Vulkan stage-flags mask.
    u32 stage_flags{};
    /// The guest shader-resource index this binding came from, or 0xFFFFFFFF when it did not come
    /// from a guest resource.
    u32 guest_resource_index{0xFFFFFFFFu};
    /// Whether the shader writes this resource.
    bool written{};
};

/// The push-constant block a stage declares.
struct PushConstant {
    u32 offset{};
    u32 size{};
    u32 stage_flags{};
};

/// The descriptor interface of one effective shader permutation.
///
/// The identity is (program hash, permutation index), matching the binary capture. One program can
/// produce permutations with different resource interfaces, so a lookup by program hash alone is
/// insufficient.
struct Interface {
    u64 program_hash{};
    u32 permutation_index{};
    u64 permutation_hash{};
    u32 hardware_stage{};
    u32 logical_stage{};
    /// Whether the effective module is a patch. A patch may declare a different interface from the
    /// generated module, so this decides which declarations are authoritative.
    bool patched{};
    /// SHA-256 of the effective module, so an interface cannot be paired with a different module.
    std::string effective_sha256;

    std::array<Descriptor, MaxDescriptorsPerStage> descriptors{};
    u32 descriptor_count{};
    PushConstant push_constant{};
    /// The number of unified bindings the stage consumed, which is what the production layout
    /// advances by.
    u32 unified_binding_count{};
    /// The global binding the production layout started this stage at.
    ///
    /// `BuildDescSetLayout` walks the stages in logical-stage order with one counter, so each
    /// stage's bindings begin at the sum of the counts of the stages before it. Recording the base
    /// lets a consumer verify that `local_binding + stage_binding_base == binding` rather than
    /// assuming it.
    u32 stage_binding_base{};
    bool stage_binding_base_known{};
};

/// Records the interface of a compiled permutation. Called from the production compilation path.
void NoteInterface(const Interface& entry) noexcept;

/// Resolves the effective Vulkan binding numbers for one pipeline layout.
///
/// `GraphicsPipeline::BuildDescSetLayout` walks the stages in logical-stage order with one counter,
/// so a stage's bindings begin at the sum of the counts of the stages before it. The per-stage
/// capture cannot know that base, because it sees one stage at a time. This is called once the full
/// layout is known, and it rewrites each retained interface's descriptors with the global binding
/// and the base it was allocated at.
///
/// param stage_order  The logical stages in the order the layout allocates them.
/// param stage_counts The number of bindings each stage consumes, in the same order.
///
/// Returns false when the counts do not match what the retained interfaces recorded, which means
/// the layout and the capture disagree and no binding number can be trusted.
[[nodiscard]] bool ResolveGlobalBindings(std::span<const u32> stage_order,
                                         std::span<const u32> stage_counts) noexcept;

/// Clears the retained interfaces and enables capture, for tests only.
///
/// The resolution is a pure function of the layout, so it is tested without a GPU. That needs a
/// known starting state, which the production path never has to reset.
void ResetForTesting() noexcept;

/// Looks up an interface by permutation hash. Returns false when it was not retained.
[[nodiscard]] bool FindByPermutationHash(u64 permutation_hash, Interface& out) noexcept;

/// Number of retained interfaces, or 0 when the capture is not enabled.
[[nodiscard]] u32 RetainedCount() noexcept;

/// Copies one retained interface by index.
[[nodiscard]] bool GetRetained(u32 index, Interface& out) noexcept;

[[nodiscard]] bool IsEnabled() noexcept;

/// Reads the environment. Called once before the guest starts.
void TryInstall() noexcept;

/// Writes the retained interfaces and a summary. Called after the guest has stopped.
void ExportInterfaces() noexcept;

} // namespace Core::ShaderInterface
