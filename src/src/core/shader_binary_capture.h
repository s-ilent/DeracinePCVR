// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::ShaderBinaryCapture {

/// Opt-in for retaining the effective shader binaries of a captured draw. Separate from every other
/// capture flag, so a binary is only retained when explicitly asked for.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_SHADER_BINARY";

/// Bounded number of distinct (program hash, permutation) pairs retained. A title screen compiles
/// a few hundred stages; the bound keeps the table a fixed array rather than an allocation.
inline constexpr size_t MaxRetainedBinaries = 512;

/// Bounded bytes retained per binary. The largest observed Déraciné stage is well under this; the
/// bound exists so a corrupt length cannot exhaust memory.
inline constexpr size_t MaxBinaryBytes = 512 * 1024;

/// The identity of one compiled shader module.
///
/// The distinction this type exists to preserve: the GCN program hash is NOT a unique identifier
/// for the effective compiled shader. The same program compiles to several permutations, because
/// the specialization depends on runtime state, resource shapes, fetch-shader state and binding
/// allocation. The effective module is identified by (program hash, permutation index), and the
/// permutation hash is what the graphics pipeline key carries.
struct ShaderIdentity {
    /// The original GCN program hash, from the guest program's own header.
    u64 program_hash{};
    /// The permutation index within that program. Zero is the first compilation.
    u32 permutation_index{};
    /// HashCombine(program_hash, permutation_index). This is what GraphicsPipelineKey carries in
    /// stage_hashes, so it is the value a draw's pipeline key can be matched against.
    u64 permutation_hash{};
    /// The hardware stage the program was compiled for.
    u32 hardware_stage{};
    /// The logical stage, which differs from the hardware stage when tessellation is emulated.
    u32 logical_stage{};
};

/// One retained shader binary.
struct RetainedBinary {
    ShaderIdentity identity{};
    /// The GCN instruction words the program was compiled from.
    std::vector<u32> gcn_code;
    /// The SPIR-V the compiler emitted.
    std::vector<u32> spirv;
    /// The patch SPIR-V, when a shader patch was active for this module.
    std::vector<u32> patch_spirv;
    /// Whether the patch was applied, which decides which of the two is the effective module.
    bool patched{};
    /// SHA-256 of the effective module's bytes, so a consumer can verify what it loaded.
    std::array<u8, 32> effective_sha256{};
    /// SHA-256 of the generated SPIR-V, before any patch.
    std::array<u8, 32> generated_sha256{};
    /// The entry point the module exports.
    std::string_view entry_point;
};

/// Records a compiled module. Called from the production compilation path, so the bytes retained are
/// the ones the renderer actually compiled rather than a parallel reconstruction.
///
/// A repeated (program hash, permutation index) is not stored twice: the first compilation of a
/// permutation is the one the renderer keeps, and a later draw reusing it refers to the same module.
void NoteCompiledShader(const ShaderIdentity& identity, std::span<const u32> gcn_code,
                        std::span<const u32> spirv, std::span<const u32> patch_spirv,
                        bool patched) noexcept;

/// Looks up a retained binary by permutation hash. Returns false when it was not retained.
[[nodiscard]] bool FindByPermutationHash(u64 permutation_hash, RetainedBinary& out) noexcept;

/// Number of retained binaries, or 0 when the capture is not enabled.
[[nodiscard]] u32 RetainedCount() noexcept;

/// Copies one retained binary by index.
[[nodiscard]] bool GetRetained(u32 index, RetainedBinary& out) noexcept;

/// Whether the capture is enabled. Cheap enough to call on the compilation path.
[[nodiscard]] bool IsEnabled() noexcept;

/// Reads the environment. Called once before the guest starts.
void TryInstall() noexcept;

/// Writes the retained binaries and a summary. Called after the guest has stopped.
void ExportBinaries() noexcept;

} // namespace Core::ShaderBinaryCapture
