// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

#include "common/types.h"

namespace Core {

class Module;

namespace VertexInputCapture {

/// Opt-in for the effective vertex-input snapshot. Separate from the draw provenance trace so the
/// existing bounded observations keep their behaviour and cost.
inline constexpr std::string_view EnableEnvironmentVariable =
    "SHADPS4_DERACINE_VERTEX_INPUT";

/// Bounded number of captured draws. Each snapshot records every fetched attribute plus the draw
/// arguments, so the bound is deliberately small.
inline constexpr size_t MaxCapturedDraws = 128;

/// Bounded number of attributes per draw. The GCN fetch shader has at most 32 semantic slots.
inline constexpr size_t MaxAttributesPerDraw = 32;

/// Bounded number of index-buffer bytes sampled per draw, for format verification.
inline constexpr size_t MaxIndexSampleBytes = 64;

/// One fetched vertex attribute, as the production binding path resolved it at draw time.
///
/// Every field here is copied from the values the renderer actually used, not recomputed from a
/// parallel model, so a snapshot describes the real draw.
struct AttributeSnapshot {
    u8 semantic{};
    u8 dest_vgpr{};
    u8 num_elements{};
    u8 sgpr_base{};
    u16 dword_offset{};
    u8 instance_data{};
    u16 inst_offset{};
    u8 data_format{};
    u8 num_format{};

    /// Resolved buffer descriptor fields, after `GetSharp` applied the instruction offset and any
    /// format override.
    u64 base_address{};
    u32 stride{};
    u32 num_records{};
    u8 descriptor_data_format{};
    u8 descriptor_num_format{};
    u8 dst_sel_x{};
    u8 dst_sel_y{};
    u8 dst_sel_z{};
    u8 dst_sel_w{};
    u8 element_size{};
    u8 index_stride{};
    u8 add_tid_enable{};

    /// The Vulkan binding the renderer assigned: the semantic index, with attribute offset zero.
    u32 binding{};
    u32 attribute_offset{};
    /// Vulkan format the renderer selected for this attribute.
    u32 vk_format{};
    /// Vertex or instance input rate, and the divisor when instanced.
    u8 input_rate{};
    u32 divisor{};
};

/// The draw itself, with the arguments the renderer actually passed to Vulkan.
struct DrawSnapshot {
    u64 provenance_id{};
    u64 vs_hash{};
    u64 ps_hash{};
    u64 pipeline_hash{};

    u32 primitive_type{};
    bool is_indexed{};
    u32 num_indices{};
    u32 num_instances{};
    u32 index_offset{};
    u32 vertex_offset{};
    u32 instance_offset{};

    /// Index-buffer state as bound.
    u8 index_type{};
    u8 index_swap_mode{};
    u64 index_base_address{};
    u64 index_address{};
    u32 index_size{};
    u32 index_buffer_size{};
    u64 index_vk_offset{};

    /// Colour and depth attachment state at draw time.
    u32 color_mask{};
    u64 depth_address{};
    bool depth_test{};
    bool depth_write{};

    u32 attribute_count{};
    AttributeSnapshot attributes[MaxAttributesPerDraw]{};

    /// First bytes of the index buffer, for format verification. Zero length when not sampled.
    u32 index_sample_bytes{};
    u8 index_sample[MaxIndexSampleBytes]{};
};

/// Records one draw's effective vertex and index input state. Called from the rasterizer at the
/// point the Vulkan draw is emitted, so the values are the ones actually used.
void NoteDrawInputs(const DrawSnapshot& snapshot) noexcept;

/// Reads the opt-in environment variable. Called once before the guest starts.
void TryInstall() noexcept;

/// Writes the summary and per-draw records. Runs after the guest has stopped.
void ExportVertexInputs() noexcept;

} // namespace VertexInputCapture
} // namespace Core
