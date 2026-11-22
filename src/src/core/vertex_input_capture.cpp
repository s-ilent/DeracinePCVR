// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vertex_input_capture.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include "common/logging/log.h"

namespace Core::VertexInputCapture {

namespace {

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<u32> draw_count{};
    std::atomic<u32> dropped_count{};
    std::atomic<bool> limit_logged{};
    std::mutex mutex;
    std::array<DrawSnapshot, MaxCapturedDraws> draws{};
};

CaptureState g_state{};

[[nodiscard]] bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

} // namespace

void NoteDrawInputs(const DrawSnapshot& snapshot) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return;

        const u32 index = g_state.draw_count.fetch_add(1, std::memory_order_relaxed);
        if (index >= MaxCapturedDraws) {
            g_state.dropped_count.fetch_add(1, std::memory_order_relaxed);
            if (!g_state.limit_logged.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO(Core_Linker, "Vertex input capture reached its {}-draw limit",
                         MaxCapturedDraws);
            }
            return;
        }

        std::scoped_lock lock{g_state.mutex};
        g_state.draws[index] = snapshot;
    } catch (...) {
        // Observation must never affect draw processing.
    }
}

void ExportVertexInputs() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return;

        const u32 draws = g_state.draw_count.load(std::memory_order_relaxed);
        const u32 dropped = g_state.dropped_count.load(std::memory_order_relaxed);

        u32 with_attributes = 0;
        u32 without_attributes = 0;
        u32 indexed = 0;
        u32 instanced = 0;
        u32 index16 = 0;
        u32 index32 = 0;
        u32 total_attributes = 0;
        u32 max_attributes = 0;

        {
            std::scoped_lock lock{g_state.mutex};
            for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
                const auto& draw = g_state.draws[i];
                if (draw.provenance_id == 0)
                    continue;
                if (draw.attribute_count != 0) {
                    ++with_attributes;
                    total_attributes += draw.attribute_count;
                    max_attributes = std::max(max_attributes, draw.attribute_count);
                } else {
                    ++without_attributes;
                }
                if (draw.is_indexed) {
                    ++indexed;
                    if (draw.index_type == 0) {
                        ++index16;
                    } else {
                        ++index32;
                    }
                }
                if (draw.num_instances > 1) {
                    ++instanced;
                }
            }
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_VERTEX_INPUT_SUMMARY draws={} dropped={} with_attributes={} "
                 "without_attributes={} indexed={} index16={} index32={} instanced={} "
                 "total_attributes={} max_attributes={} limit={}",
                 draws, dropped, with_attributes, without_attributes, indexed, index16, index32,
                 instanced, total_attributes, max_attributes, MaxCapturedDraws);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
            const auto& draw = g_state.draws[i];
            if (draw.provenance_id == 0)
                continue;

            LOG_INFO(Core_Linker,
                     "DERACINE_VERTEX_INPUT_DRAW provenance_id={} vs={:#x} ps={:#x} "
                     "pipeline={:#x} prim={} indexed={} num_indices={} instances={} "
                     "index_offset={} vertex_offset={} instance_offset={} index_type={} "
                     "index_swap={} index_base={:#x} index_address={:#x} index_size={} "
                     "index_bytes={} index_vk_offset={:#x} mask={:#x} db={:#x} z_en={} z_wr={} "
                     "attributes={}",
                     draw.provenance_id, draw.vs_hash, draw.ps_hash, draw.pipeline_hash,
                     draw.primitive_type, draw.is_indexed, draw.num_indices, draw.num_instances,
                     draw.index_offset, draw.vertex_offset, draw.instance_offset, draw.index_type,
                     draw.index_swap_mode, draw.index_base_address, draw.index_address,
                     draw.index_size, draw.index_buffer_size, draw.index_vk_offset, draw.color_mask,
                     draw.depth_address, draw.depth_test, draw.depth_write, draw.attribute_count);

            for (u32 a = 0; a < draw.attribute_count && a < MaxAttributesPerDraw; ++a) {
                const auto& attr = draw.attributes[a];
                LOG_INFO(Core_Linker,
                         "DERACINE_VERTEX_ATTRIBUTE provenance_id={} semantic={} vgpr={} "
                         "elements={} sgpr_base={} dword_offset={} instance_data={} "
                         "inst_offset={} data_format={} num_format={} base={:#x} stride={} "
                         "records={} desc_data_format={} desc_num_format={} dst_sel={}{}{}{} "
                         "element_size={} index_stride={} add_tid={} binding={} attr_offset={} "
                         "vk_format={} input_rate={} divisor={}",
                         draw.provenance_id, attr.semantic, attr.dest_vgpr, attr.num_elements,
                         attr.sgpr_base, attr.dword_offset, attr.instance_data, attr.inst_offset,
                         attr.data_format, attr.num_format, attr.base_address, attr.stride,
                         attr.num_records, attr.descriptor_data_format, attr.descriptor_num_format,
                         attr.dst_sel_x, attr.dst_sel_y, attr.dst_sel_z, attr.dst_sel_w,
                         attr.element_size, attr.index_stride, attr.add_tid_enable, attr.binding,
                         attr.attribute_offset, attr.vk_format, attr.input_rate, attr.divisor);
            }

            if (draw.index_sample_bytes != 0) {
                // Emit the sampled index bytes as hex so the format can be verified offline
                // without keeping a binary blob in the log.
                char hex[MaxIndexSampleBytes * 2 + 1]{};
                static constexpr char digits[] = "0123456789abcdef";
                const u32 count = std::min<u32>(draw.index_sample_bytes, MaxIndexSampleBytes);
                for (u32 b = 0; b < count; ++b) {
                    hex[b * 2] = digits[draw.index_sample[b] >> 4];
                    hex[b * 2 + 1] = digits[draw.index_sample[b] & 0xF];
                }
                hex[count * 2] = '\0';
                LOG_INFO(Core_Linker,
                         "DERACINE_VERTEX_INDEX_SAMPLE provenance_id={} bytes={} hex={}",
                         draw.provenance_id, count, hex);
            }
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

void TryInstall() noexcept {
    if (!IsOptedIn(std::getenv(EnableEnvironmentVariable.data())))
        return;
    g_state.enabled.store(true, std::memory_order_relaxed);
    LOG_INFO(Core_Linker, "DERACINE_VERTEX_INPUT_INSTALL draw_limit={} attribute_limit={}",
             MaxCapturedDraws, MaxAttributesPerDraw);
}

} // namespace Core::VertexInputCapture
