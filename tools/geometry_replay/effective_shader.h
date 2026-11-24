// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Package-backed effective-shader validation.
//
// This module decides whether a schema-4 package can be executed with its own captured shader
// modules, and reports precisely why not when it cannot. It does not touch Vulkan: the renderer
// consumes its verdict.
//
// The distinction it exists to preserve:
//
//   original UE4 shader resource
//     -> original GCN program hash
//     -> shadPS4 permutation index and hash
//     -> effective SPIR-V module
//     -> Vulkan pipeline
//
// A program hash alone is not a host shader identity. One program compiles to several permutations
// with different resource interfaces, so a lookup by program hash would bind the wrong module.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "package_reader.h"

namespace effective {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Why a package cannot be executed with its own shaders.
enum class Problem : u8 {
    None = 0,
    /// The package is not schema 4, so it carries no effective shader state.
    NotEffectiveSchema,
    /// A stage's effective module is absent.
    MissingModule,
    /// A stage's module bytes are not valid SPIR-V.
    InvalidModule,
    /// The permutation hash is not the combination of the program hash and the index.
    PermutationHashMismatch,
    /// The captured descriptor interface disagrees with what the module declares.
    InterfaceMismatch,
    /// A descriptor the module declares has no captured binding.
    MissingBinding,
    /// A captured binding names a resource the package does not carry.
    MissingResource,
    /// The pipeline state the module needs was not captured.
    MissingPipelineState,
    /// A descriptor class this renderer cannot bind.
    UnsupportedDescriptor,
    /// The two stages are not one vertex and one fragment stage.
    UnsupportedStageSet,
};

[[nodiscard]] const char* ProblemName(Problem problem) noexcept;

/// One stage, resolved and validated.
struct ResolvedStage {
    /// The logical stage: 0 vertex, 4 fragment, in the producer's vocabulary.
    u32 logical_stage{};
    /// The effective module's bytes.
    std::vector<u8> bytes;
    /// The permutation identity, preserved so a report can name it.
    u64 program_hash{};
    u32 permutation_index{};
    u64 permutation_hash{};
    bool patched{};
    /// The descriptor interface the module itself declares.
    std::vector<package::ShaderDescriptor> declared;
    /// The push-constant block the module declares.
    u32 push_constant_offset{};
    u32 push_constant_size{};
};

/// One descriptor to bind, resolved to the resource that supplies it.
struct ResolvedBinding {
    u32 set{};
    u32 binding{};
    /// The Vulkan descriptor type, derived from the module's declared class.
    u32 descriptor_type{};
    u32 count{1};
    /// The stage mask, from the module's declaration.
    u32 stage_flags{};
    /// The package resource whose bytes are bound, or 0 when the descriptor needs an image.
    u32 resource_ref{};
    /// The offset and range the production path wrote.
    u64 offset{};
    u64 range{};
};

/// The verdict.
struct Resolution {
    Problem problem{Problem::None};
    /// A precise explanation when the package cannot be executed.
    std::string reason;
    std::vector<ResolvedStage> stages;
    std::vector<ResolvedBinding> bindings;
    /// The push-constant range the pipeline layout must declare.
    u32 push_constant_offset{};
    u32 push_constant_size{};
    u32 push_constant_stages{};
    /// The push-constant bytes the draw submitted.
    std::vector<u8> push_constant_bytes;
};

/// The permutation hash, as shadPS4 computes it.
///
/// `HashCombine(seed, value) = seed ^ (value + 0x9e3779b9 + (seed << 12) + (seed >> 4))`. The
/// package records the result, and this recomputes it so a mis-associated module is refused rather
/// than trusted.
[[nodiscard]] u64 HashCombine(u64 seed, u64 value) noexcept;

/// Resolves a package's effective shader state.
///
/// Every check is a refusal rather than a fallback. A package that cannot supply a module, a
/// binding or the pipeline state its modules need is reported with the specific gap, because
/// substituting a diagnostic shader in effective mode would present a test render as original
/// shader execution.
[[nodiscard]] Resolution Resolve(const package::Package& package);

} // namespace effective
