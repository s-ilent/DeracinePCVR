// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Core {

class Module;

namespace GnmBindingCapture {

/// Opt-in for the binding-chain observation. Default OFF: unset, absent or anything but "1"
/// produces no record and reads no guest memory.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_GNM_BINDING";

/// Bounded number of captured writer invocations.
inline constexpr size_t MaxRecords = 256;

/// Bounded number of distinct (shader hash, stage, api_slot) chains kept for the export.
inline constexpr size_t MaxChains = 64;

/// The typed writer this phase observes. Atlas A-030's 16-byte immediate constant-buffer writer.
inline constexpr u32 CbufWriterRva = 0x02F62440;

/// The grouped graphics binder. Atlas A-031's first helper.
inline constexpr u32 GroupedBinderRva = 0x0CC0070;

/// The deferred-upload caller that actually invokes the 16-byte constant-buffer writer. Atlas
/// A-023's deferred upload family; the descriptor source is a stack buffer filled by the provider
/// at 0x0CC1010 from the upload object's +0x48.
inline constexpr u32 DeferredUploadCallerRva = 0x0CC0D50;

/// Upload-object field offsets, from the provider's disassembly.
inline constexpr u32 UploadObjectValid = 0x40;
inline constexpr u32 UploadObjectSource = 0x48;
inline constexpr u32 UploadObjectSize = 0x50;

/// The caller's stack buffer, relative to its entry stack pointer.
///
/// From the caller's prologue: push rbp; mov rsp,rbp; push r15,r14,r13,r12,rbx; sub rsp,0x28.
/// The provider's output buffer is `rbp-0x40`, so relative to the entry rsp (which points at the
/// return address) it is `rsp-0x48`. The writer is then called with that same address as its
/// source, which is what makes the association checkable rather than assumed.
inline constexpr u64 UploadCallerBufferFromEntryRsp = 0x48;

/// Bounded per-thread nesting depth for upload invocations. The observed path is not recursive,
/// so this is a safety cap rather than a working limit.
inline constexpr size_t MaxUploadNesting = 8;

/// Binder field offsets, from the original disassembly of 0x0CC0070.
inline constexpr u32 BinderDirtyMask = 0x928;
inline constexpr u32 BinderGroupMask = 0x200;
inline constexpr u32 BinderGroupCache = 0x568;
inline constexpr u32 BinderObjectArray = 0x20;
inline constexpr u32 BinderResourceList = 0x248;

/// Writer field offsets, from the original disassembly.
inline constexpr u32 WriterStageMapBase = 0xC180;
inline constexpr u32 WriterShadowBase = 0x90;
inline constexpr u32 WriterDirtyBase = 0xC250;
inline constexpr u32 WriterPacketCursor = 0xC090;

/// Metadata map offsets within the stage map object, per Atlas A-025/A-026.
inline constexpr u32 MapOffsetConstBuffer = 0xC2;
inline constexpr u32 MapOffsetResource = 0x22;
inline constexpr u32 MapOffsetSampler = 0xA2;
inline constexpr u32 MapOffsetVertexBuffer = 0xEA;
inline constexpr u32 MapOffsetRwResource = 0x82;

/// The stage map object is the shader resource's metadata at +0xB0 (Atlas A-025).
inline constexpr u32 ShaderResourceMetadataOffset = 0xB0;

/// The shader resource's program-data pointer (SC-02).
inline constexpr u32 ShaderResourceProgramData = 0x1D8;

/// How a field was obtained. Kept explicit so the report never presents a derived value as
/// observed.
enum class Provenance : u8 {
    /// Read directly from guest memory at the stated address.
    Observed = 0,
    /// Computed from observed values by a stated rule.
    Derived = 1,
    /// The value could not be obtained; the reason is recorded.
    Unavailable = 2,
};

/// How a writer invocation was tied to an upload object. Only `Matched` is a causal claim.
enum class UploadProvenance : u8 {
    /// No upload invocation was pending on this thread for this source buffer.
    Unmatched = 0,
    /// An upload invocation was pending and its expected provider buffer equals the writer's
    /// source. This is the causal association.
    Matched = 1,
    /// An upload invocation was pending but its expected buffer differs from the writer's source.
    /// Recorded as a mismatch, never treated as a match.
    SourceMismatch = 2,
};

/// One captured typed-writer invocation, joined to the shader that declared the binding.
struct BindingRecord {
    u64 sequence{};
    u64 native_thread_id{};
    u64 guest_thread_id{};

    /// Writer entry arguments, observed at the original call boundary.
    VAddr writer{};
    u32 stage{};
    u32 api_slot{};
    u32 count{};
    VAddr source{};

    /// The stage map object the writer read, and the decoded word it selected.
    VAddr map_object{};
    u16 decoded_map_word{};
    bool map_word_present{};
    /// Bit 15 of the decoded word: direct PM4 emission versus shadow-register update.
    bool direct{};
    /// Low 14 bits: the destination index the writer used.
    u32 mapped_index{};

    /// Derived: the shader resource the map object belongs to.
    VAddr shader_resource{};
    bool shader_resource_plausible{};
    /// Derived: the program data pointer the shader resource holds.
    VAddr program_data{};
    /// Derived: the program address the stage record names, and the stage record's own 23-bit size
    /// field. The trailer must equal `code_address + stage_size - 28`, which is the original
    /// decoder's arithmetic; recording both makes that checkable rather than assumed.
    VAddr code_address{};
    u32 stage_size{};
    /// Derived: the OrbShdr trailer address inside the program.
    VAddr trailer{};
    /// Derived: the shader hash from the trailer.
    u64 shader_hash{};
    bool shader_hash_valid{};

    /// Descriptor bytes copied by value at the writer boundary.
    std::array<u8, 32> descriptor{};
    u32 descriptor_bytes{};

    /// Where the writer put the descriptor, when the direct path was taken.
    VAddr packet_cursor{};
    u32 packet_register_offset{};

    /// The deferred-upload object the descriptor came from, when the writer was reached through
    /// the deferred-upload caller rather than a grouped binder.
    VAddr upload_object{};
    u32 upload_size{};
    bool upload_valid{};
    /// How the upload object was associated with this writer invocation.
    UploadProvenance upload_provenance{UploadProvenance::Unmatched};
    /// The provider's output buffer this invocation expected, and whether the writer's source
    /// matched it. A match is the causal link; a mismatch is recorded, never resolved by guessing.
    VAddr expected_provider_buffer{};
    bool source_matches_provider_buffer{};
    /// FNV-1a over the descriptor bytes, so two captures can be compared without the bytes.
    u64 descriptor_fingerprint{};

    /// The declaration this writer call corresponds to, when the shader's metadata was parsed.
    bool declaration_matched{};
    u8 declaration_usage_type{};
    u8 declaration_api_slot{};
    u8 declaration_start_register{};
    u8 declaration_flags{};
};

/// One captured grouped-binder invocation: the layer between a shader declaration and a typed
/// writer call. Atlas A-031's packed entry selects a live guest resource object.
struct BinderRecord {
    u64 sequence{};
    u64 native_thread_id{};
    u64 guest_thread_id{};

    VAddr context{};
    VAddr shader_resource{};

    /// The binder's own gate: pending = dirty_mask & group_mask. Zero means no writer follows.
    u32 dirty_mask{};
    u32 group_mask{};
    u32 pending{};
    u32 group{};

    /// The packed binding list and the entry it selected.
    VAddr list{};
    u32 list_index{};
    u32 packed_entry{};
    u8 packed_api_slot{};
    u16 packed_object_index{};
    u8 packed_group{};

    /// The cached group and the resource object the entry selected.
    VAddr cached_group{};
    VAddr object_array{};
    VAddr selected_object{};
    u64 object_vptr{};
};

/// Installs the observation points. Called before the guest thread starts, after relocation.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

/// Writes the export. Safe to call more than once; only the first call writes.
void Export() noexcept;

/// Test seam.
[[nodiscard]] size_t RecordCount() noexcept;
[[nodiscard]] std::span<const BindingRecord> Records() noexcept;
[[nodiscard]] std::span<const BinderRecord> BinderRecords() noexcept;
void ResetForTesting() noexcept;

/// Test seam: the map-word decode, exposed so the arithmetic is pinned without a guest.
[[nodiscard]] constexpr u16 EncodeMapWord(u8 usage_type, u8 start_register, u8 flags) noexcept {
    // Atlas A-025/A-030: kinds 1,2,3 encode start<16 ? 0x8000|start : start-16.
    // Kinds 0 and 4 additionally OR bit 14 when the GNM flags byte has bit 1 clear.
    u16 encoded = start_register < 16 ? static_cast<u16>(0x8000 | start_register)
                                      : static_cast<u16>(start_register - 16);
    if (usage_type == 0x00 || usage_type == 0x04) {
        if ((flags & 2) == 0) {
            encoded |= 0x4000;
        }
    }
    return encoded;
}

} // namespace GnmBindingCapture
} // namespace Core
