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

#include <map>
#include <tuple>

#include "common/logging/log.h"
#include "core/draw_package.h"

namespace Core::ShaderParamCapture {

/// Writes one package for the first draw that has at least one host-verified resource.
void WriteFirstPackage(const char* directory) noexcept;

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

/// Per-draw binding observation, filled by the production binding path. Thread-local because the
/// rasterizer records on the GPU thread and a draw's bindings never span threads.
struct PendingBindings {
    u64 provenance_id{};
    u32 count{};
    std::array<BufferBinding, MaxBindingsPerDraw> bindings{};
};
thread_local PendingBindings g_pending_bindings{};

/// One observed host upload: a guest range copied into a host-visible buffer. The bytes are
/// retained here, at the moment the production path copied them, because that copy is the
/// authoritative content the draw reads.
struct HostUpload {
    u64 guest_addr{};
    u64 size{};
    u64 host_buffer_handle{};
    u64 host_offset{};
    u64 content_hash{};
    u32 retained_bytes{};
    std::array<u8, 4096> retained{};
};

inline constexpr size_t MaxHostUploads = 512;
std::array<HostUpload, MaxHostUploads> g_host_uploads{};
std::atomic<u32> g_host_upload_count{};

/// Retained host bytes live in one pool rather than inside each binding record, so a binding stays
/// small and the memory cost is bounded by the pool size rather than by records times bytes.
std::array<u8, MaxTotalBytes> g_retained_pool{};
std::atomic<u32> g_retained_used{};


[[nodiscard]] bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::string_view{value} == "1";
}

const char* StatusName(u8 status) noexcept {
    switch (static_cast<CaptureStatus>(status)) {
    case CaptureStatus::GuestMemoryObserved:
        return "guest_memory_observed";
    case CaptureStatus::HostContentVerified:
        return "host_content_verified";
    case CaptureStatus::HostContentCaptured:
        return "host_content_captured";
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

void BeginDrawBindings(u64 provenance_id) noexcept {
    g_pending_bindings.provenance_id = provenance_id;
    g_pending_bindings.count = 0;
}

bool RecordBinding(const BufferBinding& binding) noexcept {
    if (g_pending_bindings.provenance_id == 0)
        return false;
    if (g_pending_bindings.count >= MaxBindingsPerDraw)
        return false;
    g_pending_bindings.bindings[g_pending_bindings.count++] = binding;
    return true;
}

void EndDrawBindings(DrawSnapshot& snapshot) noexcept {
    snapshot.binding_count = g_pending_bindings.count;
    for (u32 i = 0; i < g_pending_bindings.count && i < MaxBindingsPerDraw; ++i) {
        snapshot.bindings[i] = g_pending_bindings.bindings[i];
    }
    g_pending_bindings.provenance_id = 0;
    g_pending_bindings.count = 0;
}

void NoteHostUpload(u64 guest_addr, u64 size, const u8* host_bytes, u64 host_buffer_handle,
                    u64 host_offset) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return;
        if (host_bytes == nullptr || size == 0)
            return;
        const u32 slot = g_host_upload_count.fetch_add(1, std::memory_order_relaxed);
        HostUpload upload{};
        upload.guest_addr = guest_addr;
        upload.size = size;
        upload.host_buffer_handle = host_buffer_handle;
        upload.host_offset = host_offset;
        // Retain a bounded prefix of the host copy itself, not a later guest read. The hash covers
        // exactly the bytes retained, so the two always agree.
        upload.retained_bytes = static_cast<u32>(std::min<size_t>(size, upload.retained.size()));
        std::memcpy(upload.retained.data(), host_bytes, upload.retained_bytes);
        upload.content_hash = HashCapturedBytes(upload.retained.data(), upload.retained_bytes);
        g_host_uploads[slot % MaxHostUploads] = upload;
    } catch (...) {
        // Observation must never affect the upload path.
    }
}

bool FindHostUpload(u64 guest_addr, u64 size, u64& host_buffer_handle, u64& host_offset,
                    u64& content_hash) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return false;
        const u32 count = std::min<u32>(g_host_upload_count.load(std::memory_order_relaxed),
                                        static_cast<u32>(MaxHostUploads));
        // Search newest first, so a reused range resolves to the most recent upload.
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = (g_host_upload_count.load(std::memory_order_relaxed) - 1 - i) %
                             MaxHostUploads;
            const auto& upload = g_host_uploads[slot];
            if (upload.guest_addr != guest_addr || upload.size < size)
                continue;
            // The hash covers only the retained prefix, so a larger range cannot be claimed.
            if (upload.retained_bytes < size)
                continue;
            host_buffer_handle = upload.host_buffer_handle;
            host_offset = upload.host_offset;
            content_hash = upload.content_hash;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool CopyHostUploadBytes(u64 guest_addr, u64 size, u8* out, u64& copied) noexcept {
    try {
        copied = 0;
        if (!g_state.enabled.load(std::memory_order_relaxed) || out == nullptr)
            return false;
        const u32 count = std::min<u32>(g_host_upload_count.load(std::memory_order_relaxed),
                                        static_cast<u32>(MaxHostUploads));
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = (g_host_upload_count.load(std::memory_order_relaxed) - 1 - i) %
                             MaxHostUploads;
            const auto& upload = g_host_uploads[slot];
            if (upload.guest_addr != guest_addr || upload.size < size)
                continue;
            if (upload.retained_bytes < size)
                return false;
            std::memcpy(out, upload.retained.data(), static_cast<size_t>(size));
            copied = size;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RetainHostBytes(u64 guest_addr, u64 size, u32& pool_offset, u32& retained) noexcept {
    try {
        pool_offset = 0;
        retained = 0;
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return false;
        const u32 want = static_cast<u32>(std::min<u64>(size, MaxBytesPerResource));
        if (want == 0)
            return false;
        const u32 offset = g_retained_used.fetch_add(want, std::memory_order_relaxed);
        if (static_cast<size_t>(offset) + want > g_retained_pool.size())
            return false;
        u64 copied = 0;
        if (!CopyHostUploadBytes(guest_addr, want, g_retained_pool.data() + offset, copied))
            return false;
        pool_offset = offset;
        retained = static_cast<u32>(copied);
        return true;
    } catch (...) {
        return false;
    }
}

bool ReadRetainedBytes(u32 pool_offset, u32 size, u8* out) noexcept {
    try {
        if (out == nullptr || size == 0)
            return false;
        if (static_cast<size_t>(pool_offset) + size > g_retained_pool.size())
            return false;
        std::memcpy(out, g_retained_pool.data() + pool_offset, size);
        return true;
    } catch (...) {
        return false;
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
        u32 guest_observed = 0;
        u32 host_verified = 0;
        u32 host_captured = 0;
        u32 metadata_only = 0;
        u32 partial = 0;
        u32 unsupported = 0;
        u32 invalid = 0;
        u32 clamped_bindings = 0;
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
                    case CaptureStatus::GuestMemoryObserved:
                        ++guest_observed;
                        break;
                    case CaptureStatus::HostContentVerified:
                        ++host_verified;
                        break;
                    case CaptureStatus::HostContentCaptured:
                        ++host_captured;
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
                    if (binding.range_clamped)
                        ++clamped_bindings;
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
                 "DERACINE_SHADER_PARAM_SUMMARY draws={} dropped={} bindings={} "
                 "guest_observed={} host_verified={} host_captured={} metadata_only={} "
                 "partial={} unsupported={} invalid={} clamped={} storage={} written={} "
                 "readconst={} draws_with_push={} total_bytes={} byte_limit={} draw_limit={}",
                 draws, dropped, total_bindings, guest_observed, host_verified, host_captured,
                 metadata_only, partial, unsupported, invalid, clamped_bindings, storage_bindings,
                 written_bindings, readconst_bindings, draws_with_push, total_bytes, MaxTotalBytes,
                 MaxCapturedDraws);

        {
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
                         "sharp_idx={} sharp=[{:#x},{:#x},{:#x},{:#x}] "
                         "req_base={:#x} req_size={} clamp_base={:#x} clamp_size={} clamped={} "
                         "stride={} records={} data_format={} num_format={} dst_sel={}{}{}{} "
                         "storage={} written={} formatted={} readconst={} special={} "
                         "buffer_type={} vk_binding={} vk_buffer={:#x} vk_offset={} "
                         "vk_range={} push_adjust={} buffer_id={} status={} captured={} "
                         "hash={:#x} resource_ref={}",
                         draw.provenance_id, binding.stage, binding.resource_index,
                         binding.sharp_idx, binding.sharp_words[0], binding.sharp_words[1],
                         binding.sharp_words[2], binding.sharp_words[3], binding.requested_base,
                         binding.requested_size, binding.clamped_base, binding.clamped_size,
                         binding.range_clamped, binding.stride, binding.num_records,
                         binding.data_format, binding.num_format, binding.dst_sel_x,
                         binding.dst_sel_y, binding.dst_sel_z, binding.dst_sel_w,
                         binding.is_storage, binding.is_written, binding.is_formatted,
                         binding.used_as_readconst, binding.is_special, binding.buffer_type,
                         binding.vk_binding, binding.vk_buffer_handle, binding.vk_offset,
                         binding.vk_range, binding.push_offset_adjust, binding.buffer_id,
                         StatusName(binding.capture_status), binding.captured_bytes,
                         binding.captured_hash, binding.resource_ref);
            }
        }
        }
    // Write a private package for the first draw that has at least one host-verified resource.
    // This runs outside the record lock, because the package writer takes it again.
    const char* package_dir = std::getenv(DrawPackage::EnableEnvironmentVariable.data());
    if (package_dir != nullptr && *package_dir != '\0') {
        LOG_INFO(Core_Linker, "DERACINE_DRAW_PACKAGE_REQUESTED dir={}", package_dir);
        WriteFirstPackage(package_dir);
    }
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker, "Shader param export threw: {}", e.what());
    } catch (...) {
        LOG_WARNING(Core_Linker, "Shader param export threw an unknown exception");
    }
}

/// Writes one package for the first draw that has at least one host-verified resource. A package
/// is only produced when it would be internally consistent, so a reader never sees a half-built
/// one.
void WriteFirstPackage(const char* directory) noexcept {
    try {
        std::scoped_lock lock{g_state.mutex};
        const u32 draws = g_state.draw_count.load(std::memory_order_relaxed);
        for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
            const auto& draw = g_state.draws[i];
            if (draw.provenance_id == 0)
                continue;

            DrawPackage::Package package{};
            package.scene = "classroom-title";
            package.provenance_id = draw.provenance_id;
            package.pipeline_hash = draw.pipeline_hash;
            package.vs_hash = draw.vs_hash;
            package.ps_hash = draw.ps_hash;
            package.primitive_type = draw.primitive_type;
            package.is_indexed = draw.is_indexed;
            package.num_indices = draw.num_indices;
            package.num_instances = draw.num_instances;

            // One resource entry per distinct (buffer, range, hash) triple, so two bindings of the
            // same content share one entry rather than duplicating the bytes.
            std::map<std::tuple<u64, u64, u64>, u32> by_content;
            u32 next_id = 1;
            u64 retained = 0;

            for (u32 b = 0; b < draw.binding_count && b < MaxBindingsPerDraw; ++b) {
                const auto& binding = draw.bindings[b];
                DrawPackage::Binding out{};
                out.stage = binding.stage;
                out.resource_index = binding.resource_index;
                out.sharp_idx = binding.sharp_idx;
                out.vk_binding = binding.vk_binding;
                out.vk_offset = binding.vk_offset;
                out.vk_range = binding.vk_range;
                out.buffer_id = binding.buffer_id;
                out.status = binding.capture_status;

                if (binding.retained_bytes == 0) {
                    out.resource_ref = 0;
                    package.bindings.push_back(out);
                    continue;
                }

                const auto key = std::make_tuple(binding.buffer_id, binding.clamped_base,
                                                 binding.captured_hash);
                if (const auto it = by_content.find(key); it != by_content.end()) {
                    out.resource_ref = it->second;
                    package.bindings.push_back(out);
                    continue;
                }

                DrawPackage::Resource resource{};
                resource.id = next_id++;
                resource.requested_base = binding.requested_base;
                resource.requested_size = binding.requested_size;
                resource.clamped_base = binding.clamped_base;
                resource.clamped_size = binding.clamped_size;
                resource.content_hash = binding.captured_hash;
                resource.status = binding.capture_status;
                // The bytes were retained at capture time from the host copy, so this is the
                // version the draw consumed rather than a later guest read.
                resource.bytes.resize(binding.retained_bytes);
                if (!ReadRetainedBytes(binding.retained_offset, binding.retained_bytes,
                                       resource.bytes.data())) {
                    resource.bytes.clear();
                }
                retained += resource.bytes.size();
                by_content[key] = resource.id;
                out.resource_ref = resource.id;
                package.resources.push_back(std::move(resource));
                package.bindings.push_back(out);
            }

            // Replay readiness is derived, not asserted: every binding must reference a resource
            // whose content came from the host.
            package.replay_ready = !package.bindings.empty();
            for (const auto& binding : package.bindings) {
                if (binding.resource_ref == 0)
                    continue;
                const auto& resource = package.resources[binding.resource_ref - 1];
                if (resource.status != static_cast<u8>(CaptureStatus::HostContentVerified) &&
                    resource.status != static_cast<u8>(CaptureStatus::HostContentCaptured)) {
                    package.replay_ready = false;
                    package.not_ready_reason = "a required resource is guest-observed only";
                    break;
                }
            }
            if (package.bindings.empty()) {
                package.not_ready_reason = "draw has no bindings";
            }

            std::string error;
            if (!DrawPackage::ValidatePackage(package, error)) {
                LOG_WARNING(Core_Linker, "Draw package for draw {} refused: {}",
                            draw.provenance_id, error);
                continue;
            }
            if (DrawPackage::WritePackage(package, directory)) {
                return;
            }
        }
        LOG_INFO(Core_Linker, "DERACINE_DRAW_PACKAGE_NONE no draw had verified host content");
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker, "Draw package build threw: {}", e.what());
    } catch (...) {
        LOG_WARNING(Core_Linker, "Draw package build threw an unknown exception");
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
