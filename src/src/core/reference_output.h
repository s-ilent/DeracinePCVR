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

namespace Core::ReferenceOutput {

/// Opt-in for capturing the colour attachment immediately before and after the selected draw.
/// Separate from every other capture flag, so a reference is only taken when explicitly asked for.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_REFERENCE_OUTPUT";

/// Optional override for the per-capture byte bound. A 2880x3240 RGBA8 target is 37,324,800 bytes,
/// which is larger than any fixed default that would still be safe to allocate unconditionally, so
/// the bound is configurable and the default is sized for a diagnostic target rather than a full
/// eye buffer. A capture that exceeds it fails explicitly rather than being cropped or downscaled.
inline constexpr std::string_view MaxBytesEnvironmentVariable =
    "SHADPS4_DERACINE_REFERENCE_OUTPUT_MAX_BYTES";

/// Default per-capture byte bound: 64 MiB, which covers a 2880x3240 RGBA8 target (35.6 MiB) with
/// room for a 16-bit-per-channel variant of the same extent.
inline constexpr size_t DefaultMaxReferenceBytes = 64 * 1024 * 1024;

/// Hard ceiling on the configurable bound, so a typo cannot ask for an unbounded allocation.
inline constexpr size_t HardMaxReferenceBytes = 512 * 1024 * 1024;

/// Bounded number of reference pairs retained. A reference is only useful for the draw being
/// packaged, so the bound is small.
inline constexpr size_t MaxReferences = 4;

/// Why a reference capture could not be taken. Reported rather than silently producing nothing.
enum class CaptureStatus : u8 {
    /// The capture is not enabled.
    Disabled = 0,
    /// The copy was recorded and the readback is pending.
    Pending = 1,
    /// The readback completed and the bytes are the image's contents.
    Complete = 2,
    /// There is no colour target bound for this draw.
    NoColorTarget = 3,
    /// The attachment format is not one this capture supports.
    UnsupportedFormat = 4,
    /// The attachment is multisampled, which this capture does not resolve.
    Multisampled = 5,
    /// The image is larger than the configured bound.
    Oversize = 6,
    /// The staging allocation could not be created.
    AllocationFailed = 7,
    /// The copy could not be recorded.
    CopyFailed = 8,
};

[[nodiscard]] constexpr std::string_view CaptureStatusName(CaptureStatus status) noexcept {
    switch (status) {
    case CaptureStatus::Disabled:
        return "disabled";
    case CaptureStatus::Pending:
        return "pending";
    case CaptureStatus::Complete:
        return "complete";
    case CaptureStatus::NoColorTarget:
        return "no_color_target";
    case CaptureStatus::UnsupportedFormat:
        return "unsupported_format";
    case CaptureStatus::Multisampled:
        return "multisampled";
    case CaptureStatus::Oversize:
        return "oversize";
    case CaptureStatus::AllocationFailed:
        return "allocation_failed";
    case CaptureStatus::CopyFailed:
        return "copy_failed";
    default:
        return "unknown";
    }
}

/// One captured reference image.
struct ReferenceImage {
    u32 width{};
    u32 height{};
    /// The host Vulkan format of the attachment.
    u32 format{};
    u32 bytes_per_pixel{};
    /// The bytes, tightly packed, in the attachment's row order.
    std::vector<u8> bytes;
    /// SHA-256 of the bytes, as lowercase hex. Empty until the readback completes.
    std::string sha256;
};

/// The state of one reference position (pre or post) for one draw.
struct ReferenceSlot {
    CaptureStatus status{CaptureStatus::Disabled};
    ReferenceImage image{};
    /// The transfer identity, unique per (provenance, position). A stale readback cannot be
    /// attributed to a different capture because the identity travels with it.
    u64 transfer_id{};
};

/// Whether the capture is enabled.
[[nodiscard]] bool IsEnabled() noexcept;

/// The configured per-capture byte bound.
[[nodiscard]] size_t MaxBytes() noexcept;

/// Reads the environment. Called once before the guest starts.
void TryInstall() noexcept;

/// Allocates a transfer identity for a capture. Called before the copy is recorded, so the
/// deferred readback can be matched to the capture that produced it.
[[nodiscard]] u64 NextTransferId() noexcept;

/// Records that a capture was attempted and why it failed, when it did not produce a copy.
void NoteCaptureFailure(u64 provenance_id, bool is_pre, CaptureStatus status) noexcept;

/// Records that a copy was recorded and its readback is pending.
void NoteCapturePending(u64 provenance_id, bool is_pre, u64 transfer_id, u32 width, u32 height,
                        u32 format, u32 bytes_per_pixel) noexcept;

/// Completes a pending capture with the bytes read back after the GPU copy finished.
///
/// Called from the deferred readback, which runs only once the scheduler has passed the tick the
/// copy was recorded at. The bytes are moved in, so the staging allocation can be released
/// immediately afterwards.
void CompleteCapture(u64 provenance_id, bool is_pre, u64 transfer_id, std::vector<u8>&& bytes,
                     std::string&& sha256) noexcept;

/// Retrieves the reference for a draw. Returns false when either position is not complete.
[[nodiscard]] bool GetReference(u64 provenance_id, ReferenceImage& pre, ReferenceImage& post,
                                bool& has_pre) noexcept;

/// Whether any capture for this draw is still pending. The exporter uses this to decide between
/// waiting and marking the reference unavailable.
[[nodiscard]] bool HasPending(u64 provenance_id) noexcept;

/// Waits for every pending capture to complete, up to a bounded number of scheduler ticks.
/// Returns false when a capture is still pending afterwards.
[[nodiscard]] bool WaitForPending() noexcept;

/// Writes the reference summary. Called after the guest has stopped.
void ExportReferences() noexcept;

} // namespace Core::ReferenceOutput
