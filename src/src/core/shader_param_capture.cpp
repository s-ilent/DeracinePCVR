// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/shader_param_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include "common/logging/log.h"

namespace Core::ShaderParamCapture {

namespace {

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<u32> draw_count{};
    std::atomic<u32> dropped_count{};
    std::atomic<size_t> total_bytes{};
    std::atomic<bool> limit_logged{};
    std::mutex mutex;
    std::array<DrawSnapshot, MaxCapturedDraws> draws{};
};

CaptureState g_state{};

[[nodiscard]] bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

const char* StatusName(u8 status) noexcept {
    switch (static_cast<CaptureStatus>(status)) {
    case CaptureStatus::Complete:
        return "complete";
    case CaptureStatus::MetadataOnly:
        return "metadata_only";
    case CaptureStatus::Partial:
        return "partial";
    case CaptureStatus::Unsupported:
        return "unsupported";
    case CaptureStatus::Invalid:
        return "invalid";
    }
    return "unknown";
}

} // namespace

size_t CapturedByteTotal() noexcept {
    return g_state.total_bytes.load(std::memory_order_relaxed);
}

size_t ReserveCaptureBytes(size_t requested) noexcept {
    // Reserve against the global budget atomically, so concurrent producers cannot jointly exceed
    // it. A request that does not fit is reduced to what remains rather than refused outright, and
    // the caller reports the shortfall as a partial capture.
    size_t current = g_state.total_bytes.load(std::memory_order_relaxed);
    for (;;) {
        if (current >= MaxTotalBytes)
            return 0;
        const size_t take = std::min(requested, MaxTotalBytes - current);
        if (g_state.total_bytes.compare_exchange_weak(current, current + take,
                                                      std::memory_order_relaxed)) {
            return take;
        }
    }
}

void NoteDrawParams(const DrawSnapshot& snapshot) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return;

        const u32 index = g_state.draw_count.fetch_add(1, std::memory_order_relaxed);
        if (index >= MaxCapturedDraws) {
            g_state.dropped_count.fetch_add(1, std::memory_order_relaxed);
            if (!g_state.limit_logged.exchange(true, std::memory_order_relaxed)) {
                LOG_INFO(Core_Linker, "Shader parameter capture reached its {}-draw limit",
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

void ExportShaderParams() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return;

        const u32 draws = g_state.draw_count.load(std::memory_order_relaxed);
        const u32 dropped = g_state.dropped_count.load(std::memory_order_relaxed);
        const size_t total_bytes = g_state.total_bytes.load(std::memory_order_relaxed);

        u32 total_bindings = 0;
        u32 complete = 0;
        u32 metadata_only = 0;
        u32 partial = 0;
        u32 unsupported = 0;
        u32 invalid = 0;
        u32 storage_bindings = 0;
        u32 written_bindings = 0;
        u32 readconst_bindings = 0;
        u32 draws_with_push = 0;

        {
            std::scoped_lock lock{g_state.mutex};
            for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
                const auto& draw = g_state.draws[i];
                if (draw.provenance_id == 0)
                    continue;
                total_bindings += draw.binding_count;
                for (u32 b = 0; b < draw.binding_count && b < MaxBindingsPerDraw; ++b) {
                    const auto& binding = draw.bindings[b];
                    switch (static_cast<CaptureStatus>(binding.capture_status)) {
                    case CaptureStatus::Complete:
                        ++complete;
                        break;
                    case CaptureStatus::MetadataOnly:
                        ++metadata_only;
                        break;
                    case CaptureStatus::Partial:
                        ++partial;
                        break;
                    case CaptureStatus::Unsupported:
                        ++unsupported;
                        break;
                    case CaptureStatus::Invalid:
                        ++invalid;
                        break;
                    }
                    if (binding.is_storage)
                        ++storage_bindings;
                    if (binding.is_written)
                        ++written_bindings;
                    if (binding.used_as_readconst)
                        ++readconst_bindings;
                }
                for (u32 s = 0; s < draw.stage_count && s < 3; ++s) {
                    if (draw.stages[s].push_data_words != 0) {
                        ++draws_with_push;
                        break;
                    }
                }
            }
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_SHADER_PARAM_SUMMARY draws={} dropped={} bindings={} complete={} "
                 "metadata_only={} partial={} unsupported={} invalid={} storage={} written={} "
                 "readconst={} draws_with_push={} total_bytes={} byte_limit={} draw_limit={}",
                 draws, dropped, total_bindings, complete, metadata_only, partial, unsupported,
                 invalid, storage_bindings, written_bindings, readconst_bindings, draws_with_push,
                 total_bytes, MaxTotalBytes, MaxCapturedDraws);

        std::scoped_lock lock{g_state.mutex};
        for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
            const auto& draw = g_state.draws[i];
            if (draw.provenance_id == 0)
                continue;

            LOG_INFO(Core_Linker,
                     "DERACINE_SHADER_PARAM_DRAW provenance_id={} pipeline={:#x} vs={:#x} "
                     "ps={:#x} prim={} indexed={} num_indices={} instances={} stages={} "
                     "bindings={} captured_bytes={} partial={} unsupported={} invalid={}",
                     draw.provenance_id, draw.pipeline_hash, draw.vs_hash, draw.ps_hash,
                     draw.primitive_type, draw.is_indexed, draw.num_indices, draw.num_instances,
                     draw.stage_count, draw.binding_count, draw.total_captured_bytes,
                     draw.partial_resources, draw.unsupported_resources, draw.invalid_resources);

            for (u32 s = 0; s < draw.stage_count && s < 3; ++s) {
                const auto& stage = draw.stages[s];
                // Only the registers the shader declares are emitted, so a stage that uses four
                // registers does not produce a 32-register dump.
                char ud[MaxUserDataRegs * 11 + 1]{};
                u32 written = 0;
                for (u32 r = 0; r < stage.user_data_count && r < MaxUserDataRegs; ++r) {
                    written += static_cast<u32>(std::snprintf(ud + written, sizeof(ud) - written,
                                                              "%s%08x", r == 0 ? "" : ",",
                                                              stage.user_data[r]));
                }
                char push[16 * 11 + 1]{};
                u32 push_written = 0;
                for (u32 r = 0; r < stage.push_data_words && r < 16; ++r) {
                    push_written += static_cast<u32>(
                        std::snprintf(push + push_written, sizeof(push) - push_written,
                                      "%s%08x", r == 0 ? "" : ",", stage.push_data[r]));
                }
                LOG_INFO(Core_Linker,
                         "DERACINE_SHADER_STAGE provenance_id={} stage={} pgm={:#x} ud_count={} "
                         "ud=[{}] push_count={} push=[{}]",
                         draw.provenance_id, stage.stage, stage.pgm_hash, stage.user_data_count,
                         ud, stage.push_data_words, push);
            }

            for (u32 b = 0; b < draw.binding_count && b < MaxBindingsPerDraw; ++b) {
                const auto& binding = draw.bindings[b];
                LOG_INFO(Core_Linker,
                         "DERACINE_SHADER_BINDING provenance_id={} stage={} resource={} "
                         "sharp_idx={} sharp=[{:#x},{:#x},{:#x},"
                         "{:#x}] base={:#x} size={} stride={} records={} data_format={} "
                         "num_format={} dst_sel={}{}{}{} storage={} written={} formatted={} "
                         "readconst={} buffer_type={} vk_binding={} vk_buffer={:#x} vk_offset={} "
                         "vk_range={} buffer_id={} buffer_cpu={:#x} buffer_bytes={} status={} "
                         "captured={} hash={:#x}",
                         draw.provenance_id, binding.stage, binding.resource_index,
                         binding.sharp_idx, binding.sharp_words[0], binding.sharp_words[1], binding.sharp_words[2],
                         binding.sharp_words[3], binding.base_address, binding.size,
                         binding.stride, binding.num_records, binding.data_format,
                         binding.num_format, binding.dst_sel_x, binding.dst_sel_y,
                         binding.dst_sel_z, binding.dst_sel_w, binding.is_storage,
                         binding.is_written, binding.is_formatted, binding.used_as_readconst,
                         binding.buffer_type, binding.vk_binding, binding.vk_buffer_handle,
                         binding.vk_offset, binding.vk_range, binding.buffer_id,
                         binding.buffer_cpu_addr, binding.buffer_size_bytes,
                         StatusName(binding.capture_status), binding.captured_bytes,
                         binding.captured_hash);
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
    LOG_INFO(Core_Linker,
             "DERACINE_SHADER_PARAM_INSTALL draw_limit={} binding_limit={} byte_limit={}",
             MaxCapturedDraws, MaxBindingsPerDraw, MaxTotalBytes);
}

} // namespace Core::ShaderParamCapture
