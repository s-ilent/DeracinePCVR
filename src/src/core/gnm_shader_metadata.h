// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/gnm_shader_metadata_parse.h"

namespace Core::GnmShaderMetadata {

/// Opt-in for the GNM shader-metadata observation. Default OFF: with the variable unset, absent or
/// anything other than "1", no record is produced and no guest memory is read.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_GNM_SHADER";

/// Opt-in for the private raw-region dump. Separate from the metadata export because the region
/// contains original shader bytes, which must never be committed.
inline constexpr std::string_view RawEnvironmentVariable = "SHADPS4_DERACINE_GNM_SHADER_RAW";

/// Bounded number of distinct (shader hash, stage) records. The title screen compiles far fewer
/// distinct shaders than this, so the bound is a safety cap rather than a sampling window.
inline constexpr size_t MaxShaders = 512;

/// One observed shader stage. Everything here is metadata; no shader code is retained.
struct Record {
    u64 shader_hash{};
    u32 stage{};
    /// Guest address of the first shader instruction word.
    VAddr code_address{};
    /// Guest address of the OrbShdr trailer.
    VAddr trailer_address{};
    /// trailer_address - code_address, i.e. where the trailer sits inside the program.
    u64 trailer_offset_in_code{};
    /// The trailer's own 24-bit length field. This is the length of the compiled code, not of the
    /// whole program: the trailer sits after it, past the chunk data and the declaration table.
    u32 bininfo_length{};
    /// trailer_offset_in_code + 28, i.e. the end of the trailer. This is the candidate for the
    /// original decoder's stage-size field (program + size_field - 0x1C == trailer), and it is a
    /// different number from bininfo_length.
    u64 program_size{};
    /// Whether the trailer lies inside the span the renderer compiles, [code, code + length).
    /// False means the compiled span excludes the trailer and its declaration table.
    bool trailer_within_bininfo_length{};

    ParseResult parsed{};
};

/// Whether the observation is opted in. Cheap enough to call per stage; the environment is read
/// once.
[[nodiscard]] bool Enabled() noexcept;

/// Records one stage, once per distinct (shader hash, stage). Reads guest memory only after
/// validating the whole trailer and declaration table against the emulator's own mapping table.
///
/// param stage           Shader::Stage value of the stage being compiled.
/// param code_address    Guest address of the shader program.
/// param trailer_address Guest address of the OrbShdr trailer the renderer located.
void Observe(u32 stage, VAddr code_address, VAddr trailer_address) noexcept;

/// Writes the metadata export, and the private raw region when that is opted in. Safe to call
/// more than once; only the first call writes.
void Export() noexcept;

/// Test seam: the number of records taken so far.
[[nodiscard]] size_t RecordCount() noexcept;

/// Test seam: the records taken so far, for assertions.
[[nodiscard]] std::span<const Record> Records() noexcept;

/// Test seam: clears the collector. Not called by the emulator.
void ResetForTesting() noexcept;

} // namespace Core::GnmShaderMetadata
