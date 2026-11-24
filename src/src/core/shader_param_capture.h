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
    /// The exact host upload this binding resolved to, or zero when it did not resolve.
    u64 upload_sequence{};
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

/// One vertex binding as the production path bound it, with the host allocation it resolved to.
struct VertexBindingRecord {
    u32 binding{};
    u64 host_buffer{};
    u64 host_offset{};
    u64 host_size{};
    u64 guest_base{};
    u64 guest_size{};
    u32 stride{};
    u32 input_rate{};
    u32 divisor{};
    /// The exact upload this range resolved to, or zero.
    u64 upload_sequence{};
    u64 content_hash{};
    u32 retained_offset{};
    u32 retained_bytes{};
    u8 status{};

    /// The attribute this binding feeds, as the production vertex-input construction describes it.
    /// A binding is one attribute in shadPS4's layout, so these travel with the binding rather than
    /// in a separate list.
    u32 semantic{};
    u32 vk_format{};
    u32 attr_offset{};
    u32 element_count{};
    /// Shader-side identity, for correlating a draw with its vertex shader.
    u64 vs_hash{};
    u64 ps_hash{};
    u64 pipeline_hash{};
};

/// The index binding as the production path bound it.
struct IndexBindingRecord {
    u64 host_buffer{};
    u64 host_offset{};
    u64 guest_base{};
    u64 guest_size{};
    u32 index_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 primitive_type{};
    u8 index_type{};
    u8 swap_mode{};
    u64 upload_sequence{};
    u64 content_hash{};
    u32 retained_offset{};
    u32 retained_bytes{};
    u8 status{};
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

    /// Geometry: the vertex bindings and index binding this draw actually used.
    u32 vertex_binding_count{};
    VertexBindingRecord vertex_bindings[MaxBindingsPerDraw]{};
    bool has_index_binding{};
    IndexBindingRecord index_binding{};

    u32 total_captured_bytes{};
    u32 partial_resources{};
    u32 unsupported_resources{};
    u32 invalid_resources{};

    /// The graphics pipeline state the draw used, as the renderer actually configured it. Captured
    /// here rather than reconstructed, so a consumer gets the state the draw ran with.
    struct {
        u32 topology{};
        u32 cull_mode{};
        u32 front_face{};
        u32 polygon_mode{};
        bool depth_test{};
        bool depth_write{};
        u32 depth_compare_op{};
        u32 depth_format{};
        u32 samples{};
        u32 color_format_count{};
        u32 color_formats[8]{};
        u32 blend_enable[8]{};
        u32 write_mask[8]{};
        float viewport_x{};
        float viewport_y{};
        float viewport_width{};
        float viewport_height{};
        float viewport_min_depth{};
        float viewport_max_depth{};
        u32 push_constant_offset{};
        u32 push_constant_size{};
        u32 push_constant_word_count{};
        u32 push_constant_words[16]{};
    } pipeline_state;
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

/// An exact identity for one host upload.
///
/// Guest address and size are consistency checks, not identity: two uploads of the same guest
/// range into different host allocations, or into different offsets of the same allocation, are
/// different uploads and must not be confused. The identity is the upload sequence plus the host
/// allocation and range it landed in.
struct UploadKey {
    /// Monotonic sequence, unique per observed upload.
    u64 sequence{};
    /// Native host buffer identity the bytes were copied into.
    u64 host_buffer{};
    /// Byte offset within that host buffer.
    u64 host_offset{};
    /// Byte length of the uploaded range.
    u64 size{};
    /// Guest source range, kept as a consistency check.
    u64 guest_addr{};
};

/// Records that a guest range was copied into a host-visible buffer by the production upload path.
/// `host_bytes` is the destination of the copy, which is the authoritative content the draw reads.
/// Returns the key assigned to this upload, or a zero sequence when the capture is off or the
/// upload could not be recorded.
[[nodiscard]] UploadKey NoteHostUpload(u64 guest_addr, u64 size, const u8* host_bytes,
                                       u64 host_buffer_handle, u64 host_offset) noexcept;

/// Resolves the upload that a production descriptor actually used.
///
/// The caller supplies the host buffer and offset its descriptor was built from, so the match is
/// on the host allocation rather than on the guest address. A guest range that was re-uploaded
/// into a different allocation does not resolve to the earlier one.
[[nodiscard]] bool FindHostUploadForBinding(u64 host_buffer, u64 host_offset, u64 size,
                                            u64 guest_addr, UploadKey& key,
                                            u64& content_hash) noexcept;

/// Copies the retained host bytes for an exact upload into `out`. Returns false when the upload is
/// unknown or the retained prefix is shorter than the request, so a caller never receives a
/// partial buffer presented as complete.
[[nodiscard]] bool CopyUploadBytes(const UploadKey& key, u8* out, u64& copied) noexcept;

/// Reserves space in the retained-byte pool and copies an exact upload's host bytes there.
///
/// `content_hash` receives the hash of the bytes actually retained, which for a binding inside a
/// merged upload is a slice rather than the whole upload. The package must carry the slice's hash,
/// because that is what its bytes are.
[[nodiscard]] bool RetainUploadBytes(const UploadKey& key, u32& pool_offset, u32& retained,
                                     u64& content_hash) noexcept;

/// Reads retained bytes back out of the pool. Used by the package writer.
[[nodiscard]] bool ReadRetainedBytes(u32 pool_offset, u32 size, u8* out) noexcept;

/// Records one vertex binding at the point production binds it.
void NoteVertexBinding(const VertexBindingRecord& record) noexcept;

/// Records the index binding at the point production binds it.
void NoteIndexBinding(const IndexBindingRecord& record) noexcept;

/// Clears the per-draw geometry records. Called when a new draw begins.
void BeginDrawGeometry() noexcept;

/// Hands the collected geometry records to the draw snapshot.
void EndDrawGeometry(DrawSnapshot& snapshot) noexcept;

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
