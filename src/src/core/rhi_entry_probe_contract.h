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

/// Opt-in for the end-to-end draw provenance trace. Separate from the RHI entry probe so the
/// existing bounded observation keeps its current behaviour and cost.
inline constexpr std::string_view ProvenanceEnvironmentVariable =
    "SHADPS4_DERACINE_DRAW_PROVENANCE";

/// Bounded number of provenance records. Each record follows one guest draw from the deferred
/// command wrapper through packet construction to the Vulkan draw, so the bound is deliberately
/// small: a verified sample is worth more than a large speculative one.
inline constexpr size_t MaxProvenanceRecords = 256;

/// A provenance record is only useful while its packet can still be observed. Records older than
/// this are reported as expired rather than silently dropped.
inline constexpr u64 MaxProvenanceAgeUs = 5'000'000;

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

/// Lifecycle of one provenance record. A record advances monotonically; a state never goes
/// backwards, so a stale or already-consumed record cannot be re-claimed.
enum class ProvenanceState : u8 {
    /// Selected at the RHI entry; no packet yet.
    Selected = 0,
    /// A packet was constructed into the guest command buffer.
    Constructed = 1,
    /// The submission carrying that packet was observed.
    Submitted = 2,
    /// The packet was decoded on the GPU thread.
    Decoded = 3,
    /// The rasterizer emitted a Vulkan draw for it.
    Recorded = 4,
    /// The operation was intentionally omitted.
    Skipped = 5,
    /// A prerequisite prevented recording.
    Failed = 6,
    /// The packet was never observed within the age bound.
    Expired = 7,
    /// The association was rejected as ambiguous or inconsistent.
    Rejected = 8,
};

[[nodiscard]] constexpr bool IsTerminalProvenanceState(ProvenanceState state) noexcept {
    return state == ProvenanceState::Recorded || state == ProvenanceState::Skipped ||
           state == ProvenanceState::Failed || state == ProvenanceState::Expired ||
           state == ProvenanceState::Rejected;
}

/// A packet registered at construction time, waiting to be matched at decode time.
///
/// The identity is `submission_id + buffer identity + packet offset`. `submission_id` is zero
/// until the submission that carries the packet is observed, and a packet with no submission
/// identity is never matched: an unresolved observation is acceptable, a wrong one is not.
struct PacketKey {
    u64 submission_id{};
    /// Guest command-buffer pointer the packet was constructed into.
    VAddr source_buffer{};
    /// Byte offset of the packet within that buffer. Zero is a valid offset.
    u64 packet_offset{};
    /// Set once the packet has been claimed by a decode, so it cannot be claimed twice.
    bool consumed{};
};

/// Why a decode failed to resolve a packet. Reported so unmatched decodes are explainable.
enum class MatchOutcome : u8 {
    Matched = 0,
    /// No packet shares this submission and offset.
    NoCandidate = 1,
    /// More than one packet shares this submission and offset.
    Ambiguous = 2,
    /// The only candidate was already consumed by an earlier decode.
    AlreadyConsumed = 3,
    /// The candidate's packet fields disagree with the decoded packet.
    Inconsistent = 4,
    /// The decode carried no submission identity, so nothing can be resolved exactly.
    NoSubmissionIdentity = 5,
};

/// Resolves a decoded packet to exactly one registered packet.
///
/// This is the production matcher: the live capture path calls it, and the tests exercise the
/// same function. Matching is by exact identity only. Index count and index address are
/// consistency checks applied after identity is established, never a fallback.
///
/// `packets` is the registered packet table; entries with `submission_id == 0` are not yet
/// submitted and are skipped. `out_index` receives the matched table index.
[[nodiscard]] inline MatchOutcome MatchDecodedPacket(
    std::span<const PacketKey> packets, u64 submission_id, VAddr source_buffer,
    u64 packet_offset, u32 index_count, u64 index_addr,
    std::span<const u32> packet_index_counts, std::span<const u64> packet_index_addrs,
    std::size_t& out_index) noexcept {
    out_index = 0;
    if (submission_id == 0) {
        return MatchOutcome::NoSubmissionIdentity;
    }

    std::size_t found = 0;
    std::size_t found_count = 0;
    bool saw_consumed = false;
    bool saw_inconsistent = false;

    for (std::size_t i = 0; i < packets.size(); ++i) {
        const auto& packet = packets[i];
        if (packet.submission_id == 0) {
            continue;
        }
        if (packet.submission_id != submission_id) {
            continue;
        }
        if (packet.source_buffer != source_buffer) {
            continue;
        }
        if (packet.packet_offset != packet_offset) {
            continue;
        }
        // Identity is established. The packet fields must then agree, or the association is
        // rejected rather than accepted on identity alone.
        if (i >= packet_index_counts.size() || i >= packet_index_addrs.size()) {
            saw_inconsistent = true;
            continue;
        }
        if (packet_index_counts[i] != index_count || packet_index_addrs[i] != index_addr) {
            saw_inconsistent = true;
            continue;
        }
        if (packet.consumed) {
            saw_consumed = true;
            continue;
        }
        found = i;
        ++found_count;
    }

    if (found_count == 1) {
        out_index = found;
        return MatchOutcome::Matched;
    }
    if (found_count > 1) {
        return MatchOutcome::Ambiguous;
    }
    if (saw_consumed) {
        return MatchOutcome::AlreadyConsumed;
    }
    if (saw_inconsistent) {
        return MatchOutcome::Inconsistent;
    }
    return MatchOutcome::NoCandidate;
}

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
