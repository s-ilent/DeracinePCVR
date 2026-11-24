// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::ReferenceOutput {

/// Opt-in for capturing the colour attachment immediately before and after the selected draw.
/// Separate from every other capture flag, so a reference is only taken when explicitly asked for.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_REFERENCE_OUTPUT";

/// Bounded bytes retained per reference image. A 640x640 RGBA8 target is 1.6 MB; the bound leaves
/// room for a larger attachment while keeping the capture a fixed allocation.
inline constexpr size_t MaxReferenceBytes = 32 * 1024 * 1024;

/// One captured reference image.
struct ReferenceImage {
    u32 width{};
    u32 height{};
    /// The guest image format, as the texture cache describes it.
    u32 format{};
    u32 bytes_per_pixel{};
    /// The bytes, tightly packed.
    std::vector<u8> bytes;
};

/// Whether the capture is enabled.
[[nodiscard]] bool IsEnabled() noexcept;

/// Reads the environment. Called once before the guest starts.
void TryInstall() noexcept;

/// Records the colour attachment contents for the draw being processed.
///
/// The pre-draw image is taken before the draw is recorded and the post-draw image after, so a
/// consumer can start from the state the draw actually inherited rather than from a cleared target.
/// Both are keyed by the provenance id, so a reference cannot be attributed to the wrong draw.
void NotePreDraw(u64 provenance_id, const ReferenceImage& image) noexcept;
void NotePostDraw(u64 provenance_id, const ReferenceImage& image) noexcept;

/// Retrieves the reference for a draw. Returns false when it was not captured.
[[nodiscard]] bool GetReference(u64 provenance_id, ReferenceImage& pre, ReferenceImage& post,
                                bool& has_pre) noexcept;

/// Writes the reference and a summary. Called after the guest has stopped.
void ExportReferences() noexcept;

} // namespace Core::ReferenceOutput
