// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Core::RhiEntryProbe {

inline constexpr std::string_view TitleId = "CUSA11919";
inline constexpr std::string_view ModuleName = "eboot.bin";
inline constexpr std::string_view EbootV100Sha256 =
    "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0";
inline constexpr std::string_view EbootV101Sha256 =
    "8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75";
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_RHI_PROBE";

/// Optional bounded start delay in seconds, so the probe can observe the same interval as the
/// command capture instead of the first 128 draws after boot. Absent or zero means observe from
/// the first entry.
inline constexpr std::string_view StartDelayEnvironmentVariable =
    "SHADPS4_DERACINE_RHI_PROBE_AFTER";

/// Upper bound on the accepted start delay. A larger value is refused rather than clamped.
inline constexpr u32 MaxStartDelaySeconds = 600;

inline constexpr VAddr TargetRva = 0x0CBB9C0;

// The exact 17-byte entry sequence in both approved executable images. The displacements are
// absent from this prologue, so the displaced instructions need no address-dependent fixups.
inline constexpr std::array<u8, 17> TargetPrologue{
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41,
    0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC, 0x28,
};

/// The observer is opt-in; only the exact value "1" enables it.
[[nodiscard]] inline bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

inline constexpr u64 MaxRhiToGnmCorrelationAgeUs = 250'000;

[[nodiscard]] inline bool IsRhiToGnmCorrelationValid(u64 rhi_sequence, u64 rhi_thread_id,
                                                     u64 gnm_thread_id, u64 age_us) noexcept {
    return rhi_sequence != 0 && rhi_thread_id == gnm_thread_id &&
           age_us <= MaxRhiToGnmCorrelationAgeUs;
}

[[nodiscard]] inline bool IsApprovedImageHash(std::string_view image_sha256) noexcept {
    return image_sha256 == EbootV100Sha256 || image_sha256 == EbootV101Sha256;
}

/// Version metadata is intentionally not an identity gate: package metadata can describe a
/// different installed layer from the selected private eboot.bin. The complete executable hash
/// remains the identity authority for either approved image.
[[nodiscard]] inline bool IsTargetBuild(std::string_view module_name, std::string_view title_id,
                                        std::string_view image_sha256) noexcept {
    return module_name == ModuleName && title_id == TitleId && IsApprovedImageHash(image_sha256);
}

[[nodiscard]] inline bool MatchesTargetPrologue(std::span<const u8> bytes) noexcept {
    return bytes.size() >= TargetPrologue.size() &&
           std::equal(TargetPrologue.begin(), TargetPrologue.end(), bytes.begin());
}

/// Check a range against the file-backed and memory-backed extents of a PF_X PT_LOAD segment.
[[nodiscard]] inline bool IsExecutableLoadRange(VAddr rva, u64 size, VAddr segment_rva,
                                                u64 file_size, u64 memory_size) noexcept {
    if (size == 0 || file_size > memory_size || rva < segment_rva) {
        return false;
    }
    const u64 offset = rva - segment_rva;
    return offset <= file_size && size <= file_size - offset && offset <= memory_size &&
           size <= memory_size - offset;
}

} // namespace Core::RhiEntryProbe
