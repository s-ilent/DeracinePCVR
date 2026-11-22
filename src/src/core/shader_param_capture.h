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

/// Bounded total bytes captured across the whole run.
inline constexpr size_t MaxTotalBytes = 4 * 1024 * 1024;

/// How completely a resource was captured.
enum class CaptureStatus : u8 {
    /// The full requested range was captured and hashed.
    Complete = 0,
    /// Only metadata was recorded; no bytes were taken.
    MetadataOnly = 1,
    /// Fewer bytes than requested were captured.
    Partial = 2,
    /// The resource type is not supported by this capture.
    Unsupported = 3,
    /// The resource was invalid, null or stale at capture time.
    Invalid = 4,
};

/// One shader buffer binding, as the production binding path resolved it.
struct BufferBinding {
    u8 stage{};
    u32 resource_index{};
    u32 sharp_idx{};
    u32 sgpr_base{};
    u32 dword_offset{};

    /// Raw descriptor words, so the original sharp can be reconstructed.
    u32 sharp_words[4]{};

    u64 base_address{};
    u64 size{};
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
    u8 buffer_type{};

    /// The Vulkan binding the renderer assigned, and the values it supplied.
    u32 vk_binding{};
    u64 vk_buffer_handle{};
    u64 vk_offset{};
    u64 vk_range{};

    /// The buffer-cache resource identity and the guest range it covers.
    u64 buffer_id{};
    u64 buffer_cpu_addr{};
    u64 buffer_size_bytes{};

    /// Capture outcome for this binding.
    u8 capture_status{};
    u32 captured_bytes{};
    u64 captured_hash{};
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
