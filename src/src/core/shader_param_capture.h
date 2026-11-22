// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "common/types.h"

namespace Core {

namespace ShaderParamCapture {

/// Opt-in for the shader-parameter and resource-content snapshot. Separate from the vertex-input
/// trace so each observation can be enabled on its own.
inline constexpr std::string_view EnableEnvironmentVariable =
    "SHADPS4_DERACINE_SHADER_PARAMS";

/// Bounded number of captured draws. The SC-07R sample is 128 draws, so this covers it while
/// staying a fixed array.
inline constexpr size_t MaxCapturedDraws = 128;

/// Bounded number of buffer bindings per draw, across all stages.
inline constexpr size_t MaxBindingsPerDraw = 48;

/// Bounded number of user-data registers recorded per stage. The PS4 exposes 32; only the ones a
/// shader actually declares are recorded, so this is an upper bound rather than a fixed dump.
inline constexpr size_t MaxUserDataRegs = 32;

/// Bounded bytes captured per resource. A draw that needs more is reported as partial rather than
/// silently truncated.
inline constexpr size_t MaxBytesPerResource = 64 * 1024;

/// Bounded total bytes captured across the whole run. The title screen binds large buffers (a
/// single draw can reference 276 KB), so this covers a representative sample without truncating
/// most of it. A draw that exceeds the remaining budget is reported as partial.
inline constexpr size_t MaxTotalBytes = 64 * 1024 * 1024;

/// How completely a resource was captured.
///
/// `Complete` is deliberately absent: a guest-memory hash does not establish that the bytes are
/// the contents of the host buffer the draw consumed. The states below say exactly what was
/// established, so a package cannot imply more than it has.
enum class CaptureStatus : u8 {
    /// Guest bytes were sampled and hashed; host equivalence is unverified.
    GuestMemoryObserved = 0,
    /// The bytes correspond to the actual bound host resource, established at upload time.
    HostContentVerified = 1,
    /// Verified host-resource bytes were retained for export.
    HostContentCaptured = 2,
    /// Descriptor and binding captured without contents.
    MetadataOnly = 3,
    /// The required range was not fully available.
    Partial = 4,
    /// Capture cannot safely support this resource.
    Unsupported = 5,
    /// The observation was inconsistent, stale or invalid.
    Invalid = 6,
};

/// One shader buffer binding, recorded at the point production constructs it.
///
/// Every field is written by the production binding path for this specific descriptor, so no
/// relationship is reconstructed afterwards from parallel array indices.
struct BufferBinding {
    u8 stage{};
    u32 resource_index{};
    /// Index into the shader's flattened user-data buffer where the descriptor begins. A
    /// BufferResource carries only this index; it has no separate SGPR base or dword offset, which
    /// is a property of vertex-fetch attributes instead.
    u32 sharp_idx{};

    /// Raw descriptor words, so the original sharp can be reconstructed.
    u32 sharp_words[4]{};

    /// Descriptor-requested guest range, before clamping.
    u64 requested_base{};
    u64 requested_size{};
    /// The validated range production actually used, after ClampRangeSize.
    u64 clamped_base{};
    u64 clamped_size{};
    /// True when clamping reduced the range, so an incomplete descriptor is reported as such.
    bool range_clamped{};

    u32 stride{};
    u32 num_records{};
    u8 data_format{};
    u8 num_format{};
    u8 dst_sel_x{};
    u8 dst_sel_y{};
    u8 dst_sel_z{};
    u8 dst_sel_w{};

    /// How the renderer classified and bound it.
    bool is_storage{};
    bool is_written{};
    bool is_formatted{};
    bool used_as_readconst{};
    bool is_special{};
    u8 buffer_type{};

    /// The unified Vulkan descriptor binding the renderer assigned, and the values it supplied.
    u32 vk_binding{};
    u64 vk_buffer_handle{};
    u64 vk_offset{};
    u64 vk_range{};
    /// The push-constant offset adjustment applied for this binding, if any.
    u32 push_offset_adjust{};

    /// The buffer-cache resource identity.
    u64 buffer_id{};

    /// Capture outcome for this binding.
    u8 capture_status{};
    u32 captured_bytes{};
    u64 captured_hash{};
    /// Package-local resource id, so two bindings of the same content share one entry.
    u32 resource_ref{};
    /// Offset into the retained-byte pool, and how many bytes are there. Zero bytes means no
    /// content was retained, which is the case when only guest memory was observed.
    u32 retained_offset{};
    u32 retained_bytes{};
};

/// One stage's user-data state at the draw.
struct StageUserData {
    u8 stage{};
    u64 pgm_hash{};
    u32 user_data_count{};
    u32 user_data[MaxUserDataRegs]{};
    /// The push-constant block the renderer actually submitted.
    u32 push_data_words{};
    u32 push_data[16]{};
};

struct DrawSnapshot {
    u64 provenance_id{};
    u64 pipeline_hash{};
    u64 vs_hash{};
    u64 ps_hash{};
    u32 primitive_type{};
    bool is_indexed{};
    u32 num_indices{};
    u32 num_instances{};

    u32 stage_count{};
    StageUserData stages[3]{};

    u32 binding_count{};
    BufferBinding bindings[MaxBindingsPerDraw]{};

    u32 total_captured_bytes{};
    u32 partial_resources{};
    u32 unsupported_resources{};
    u32 invalid_resources{};
};

/// Records one draw's shader parameters and resource metadata. Called from the rasterizer after
/// the descriptor writes are built, so the values are the ones actually supplied to Vulkan.
void NoteDrawParams(const DrawSnapshot& snapshot) noexcept;

/// Begins a per-draw binding observation. Called once per draw before the stages are walked, so
/// the production binding path can append each descriptor as it constructs it.
void BeginDrawBindings(u64 provenance_id) noexcept;

/// Appends one binding, recorded by the production binding path at the point it constructs the
/// descriptor. Returns false when the per-draw bound is reached.
bool RecordBinding(const BufferBinding& binding) noexcept;

/// Ends the observation and hands the collected bindings to the draw snapshot.
void EndDrawBindings(DrawSnapshot& snapshot) noexcept;

/// Records that a guest range was copied into a host-visible buffer by the production upload
/// path, so a later binding of that range can be reported as host-verified rather than
/// guest-observed. `host_bytes` is the destination of the copy, which is the authoritative
/// content the draw will read.
void NoteHostUpload(u64 guest_addr, u64 size, const u8* host_bytes, u64 host_buffer_handle,
                    u64 host_offset) noexcept;

/// Looks up the most recent host upload covering a guest range. Returns false when the range was
/// never uploaded through a path this capture observed, in which case a binding of it stays
/// guest-observed.
[[nodiscard]] bool FindHostUpload(u64 guest_addr, u64 size, u64& host_buffer_handle,
                                  u64& host_offset, u64& content_hash) noexcept;

/// Copies the retained host bytes for a guest range into `out`. Returns false when the upload was
/// not observed or the retained prefix is shorter than the request, so a caller never receives a
/// partial buffer presented as complete.
[[nodiscard]] bool CopyHostUploadBytes(u64 guest_addr, u64 size, u8* out, u64& copied) noexcept;

/// Reserves space in the retained-byte pool and copies the host bytes there. Returns the pool
/// offset and the number of bytes stored, or a zero length when the pool is exhausted. The pool is
/// separate from the per-binding records so a binding stays small.
[[nodiscard]] bool RetainHostBytes(u64 guest_addr, u64 size, u32& pool_offset,
                                   u32& retained) noexcept;

/// Reads retained bytes back out of the pool. Used by the package writer.
[[nodiscard]] bool ReadRetainedBytes(u32 pool_offset, u32 size, u8* out) noexcept;

/// Content hash of a captured byte range. The buffer cache exposes no per-resource generation
/// counter, so a content hash is the only version identity it can honestly provide.
///
/// Header-only so the contract tests can exercise it without linking the logging stack.
[[nodiscard]] inline u64 HashCapturedBytes(const u8* data, size_t size) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// Running total of captured bytes, so the caller can enforce the global byte budget without
/// reaching into the capture state.
[[nodiscard]] size_t CapturedByteTotal() noexcept;

/// Adds to the running total. Returns the amount actually accepted, which may be less than
/// requested when the budget is exhausted.
size_t ReserveCaptureBytes(size_t requested) noexcept;

/// Reads the opt-in environment variable. Called once before the guest starts.
void TryInstall() noexcept;

/// Writes the summary and per-draw records. Runs after the guest has stopped.
void ExportShaderParams() noexcept;

} // namespace ShaderParamCapture
} // namespace Core
