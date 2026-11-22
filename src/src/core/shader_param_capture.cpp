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

#include <functional>
#include <map>
#include <set>
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

/// Per-draw geometry observation, filled by the production vertex and index binding paths.
struct PendingGeometry {
    u32 vertex_count{};
    VertexBindingRecord vertices[MaxBindingsPerDraw]{};
    bool has_index{};
    IndexBindingRecord index{};
};
thread_local PendingGeometry g_pending_geometry{};

/// One observed host upload: a guest range copied into a host-visible buffer. The bytes are
/// retained here, at the moment the production path copied them, because that copy is the
/// authoritative content the draw reads.
///
/// The identity is the sequence plus the host allocation and range. Guest address and size are
/// consistency checks only: two uploads of the same guest range into different allocations, or into
/// different offsets of one allocation, are different uploads.
struct HostUpload {
    UploadKey key{};
    u64 content_hash{};
    u32 retained_bytes{};
    // The retained prefix must cover a real scene mesh's index buffer, which is far larger than
    // a UI quad's. 256 KB covers the largest draw in the title-screen sample; a resource beyond
    // that is reported as partial rather than silently truncated.
    std::array<u8, 256 * 1024> retained{};
};

// The ring is small because each slot retains a large prefix: 64 slots at 256 KB is 16 MB, which
// is bounded and still covers the uploads a draw's bindings can reference.
inline constexpr size_t MaxHostUploads = 64;

/// The upload ring is published under a mutex rather than by atomic index alone. A reader that saw
/// a slot index before the slot was written would observe a partially published record, and a slot
/// being replaced concurrently would tear. The lock is held only for the copy in and out of a slot,
/// never across GPU execution.
std::mutex g_upload_mutex;
std::array<HostUpload, MaxHostUploads> g_host_uploads{};
u64 g_host_upload_sequence{};
u32 g_host_upload_count{};

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

UploadKey NoteHostUpload(u64 guest_addr, u64 size, const u8* host_bytes,
                           u64 host_buffer_handle, u64 host_offset) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return {};
        if (host_bytes == nullptr || size == 0)
            return {};

        HostUpload upload{};
        upload.key.guest_addr = guest_addr;
        upload.key.size = size;
        upload.key.host_buffer = host_buffer_handle;
        upload.key.host_offset = host_offset;
        // Retain a bounded prefix of the host copy itself, not a later guest read. The hash covers
        // exactly the bytes retained, so the two always agree.
        upload.retained_bytes = static_cast<u32>(std::min<size_t>(size, upload.retained.size()));
        std::memcpy(upload.retained.data(), host_bytes, upload.retained_bytes);
        upload.content_hash = HashCapturedBytes(upload.retained.data(), upload.retained_bytes);

        std::scoped_lock lock{g_upload_mutex};
        upload.key.sequence = ++g_host_upload_sequence;
        g_host_uploads[g_host_upload_count % MaxHostUploads] = upload;
        ++g_host_upload_count;
        return upload.key;
    } catch (...) {
        // Observation must never affect the upload path.
        return {};
    }
}

bool FindHostUploadForBinding(u64 host_buffer, u64 host_offset, u64 size, u64 guest_addr,
                              UploadKey& key, u64& content_hash) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return false;

        std::scoped_lock lock{g_upload_mutex};
        const u32 count = std::min<u32>(g_host_upload_count, static_cast<u32>(MaxHostUploads));
        // Newest first, so a reused stream-buffer offset resolves to the upload that most recently
        // landed there rather than an obsolete one.
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = (g_host_upload_count - 1 - i) % MaxHostUploads;
            const auto& upload = g_host_uploads[slot];
            if (upload.key.sequence == 0)
                continue;
            // The host allocation is the identity. A guest range re-uploaded into a different
            // allocation must not resolve to the earlier one.
            if (upload.key.host_buffer != host_buffer)
                continue;
            // The production path uploads merged ranges, so a binding may sit anywhere inside an
            // upload rather than exactly at its start. Containment is the correct relation: the
            // binding's host offset must fall inside the upload, and the same delta must hold on
            // the guest side, which is what proves the two describe the same bytes.
            if (host_offset < upload.key.host_offset)
                continue;
            const u64 host_delta = host_offset - upload.key.host_offset;
            if (host_delta + size > upload.key.size)
                continue;
            if (guest_addr < upload.key.guest_addr)
                continue;
            if (guest_addr - upload.key.guest_addr != host_delta)
                continue;
            // The hash covers only the retained prefix, so a range beyond it cannot be claimed.
            if (host_delta + size > upload.retained_bytes)
                continue;
            key = upload.key;
            // The retained bytes start at the upload's own offset, so the caller's slice begins at
            // host_delta into them.
            key.host_offset = upload.key.host_offset + host_delta;
            key.guest_addr = upload.key.guest_addr + host_delta;
            key.size = size;
            content_hash = upload.content_hash;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool CopyUploadBytes(const UploadKey& key, u8* out, u64& copied) noexcept {
    try {
        copied = 0;
        if (!g_state.enabled.load(std::memory_order_relaxed) || out == nullptr)
            return false;
        if (key.sequence == 0)
            return false;

        std::scoped_lock lock{g_upload_mutex};
        const u32 count = std::min<u32>(g_host_upload_count, static_cast<u32>(MaxHostUploads));
        for (u32 i = 0; i < count; ++i) {
            const u32 slot = (g_host_upload_count - 1 - i) % MaxHostUploads;
            const auto& upload = g_host_uploads[slot];
            if (upload.key.sequence != key.sequence)
                continue;
            // A binding may reference a slice of the upload, so the retained bytes are indexed
            // from the upload's own start rather than from the binding's.
            if (key.host_offset < upload.key.host_offset)
                return false;
            const u64 delta = key.host_offset - upload.key.host_offset;
            if (delta + key.size > upload.retained_bytes)
                return false;
            std::memcpy(out, upload.retained.data() + delta, static_cast<size_t>(key.size));
            copied = key.size;
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RetainUploadBytes(const UploadKey& key, u32& pool_offset, u32& retained,
                        u64& content_hash) noexcept {
    try {
        pool_offset = 0;
        retained = 0;
        content_hash = 0;
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return false;
        const u32 want = static_cast<u32>(std::min<u64>(key.size, MaxBytesPerResource));
        if (want == 0)
            return false;
        const u32 offset = g_retained_used.fetch_add(want, std::memory_order_relaxed);
        if (static_cast<size_t>(offset) + want > g_retained_pool.size())
            return false;
        u64 copied = 0;
        if (!CopyUploadBytes(key, g_retained_pool.data() + offset, copied))
            return false;
        pool_offset = offset;
        retained = static_cast<u32>(copied);
        // Hash exactly the bytes retained. For a binding inside a merged upload this is a slice,
        // and the package's hash must describe the slice rather than the whole upload.
        content_hash = HashCapturedBytes(g_retained_pool.data() + offset, retained);
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

void BeginDrawGeometry() noexcept {
    g_pending_geometry.vertex_count = 0;
    g_pending_geometry.has_index = false;
    g_pending_geometry.index = {};
}

void NoteVertexBinding(const VertexBindingRecord& record) noexcept {
    if (!g_state.enabled.load(std::memory_order_relaxed))
        return;
    if (g_pending_geometry.vertex_count >= MaxBindingsPerDraw)
        return;
    g_pending_geometry.vertices[g_pending_geometry.vertex_count++] = record;
}

void NoteIndexBinding(const IndexBindingRecord& record) noexcept {
    if (!g_state.enabled.load(std::memory_order_relaxed))
        return;
    g_pending_geometry.has_index = true;
    g_pending_geometry.index = record;
}

void EndDrawGeometry(DrawSnapshot& snapshot) noexcept {
    snapshot.vertex_binding_count = g_pending_geometry.vertex_count;
    for (u32 i = 0; i < g_pending_geometry.vertex_count && i < MaxBindingsPerDraw; ++i) {
        snapshot.vertex_bindings[i] = g_pending_geometry.vertices[i];
    }
    snapshot.has_index_binding = g_pending_geometry.has_index;
    snapshot.index_binding = g_pending_geometry.index;
    BeginDrawGeometry();
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
        // Selection is by stable signature, not by provenance id, because ids shift between runs.
        // A candidate must be a complete indexed draw whose every vertex binding resolved to host
        // content, and it is scored on how much three-dimensional structure it carries rather than
        // on raw size: a screen-space quad has one stream and a two-component position, while a
        // scene mesh has several streams and real 3D formats.
        const char* override_env = std::getenv("SHADPS4_DERACINE_DRAW_PACKAGE_SELECT");
        const u64 override_vs = override_env != nullptr && *override_env != '\0'
                                    ? std::strtoull(override_env, nullptr, 0)
                                    : 0;

        u32 best_index = MaxCapturedDraws;
        u64 best_score = 0;
        for (u32 i = 0; i < MaxCapturedDraws && i < draws; ++i) {
            const auto& candidate = g_state.draws[i];
            if (candidate.provenance_id == 0 || candidate.vertex_binding_count == 0)
                continue;
            if (!candidate.has_index_binding)
                continue;
            if (override_vs != 0 && candidate.vs_hash != override_vs)
                continue;

            // Only consider draws whose geometry is fully resolved: a draw with an unresolved
            // vertex binding cannot produce a valid package, so it is not a candidate.
            bool complete = candidate.index_binding.retained_bytes != 0;
            u64 score = candidate.index_binding.retained_bytes;
            u32 three_d_streams = 0;
            for (u32 v = 0; v < candidate.vertex_binding_count && v < MaxBindingsPerDraw; ++v) {
                const auto& vertex = candidate.vertex_bindings[v];
                if (vertex.retained_bytes == 0) {
                    complete = false;
                    break;
                }
                score += vertex.retained_bytes;
                // A three-component float attribute is the shape a position or normal takes. A
                // two-component one is more often a UV.
                if (vertex.element_count >= 3) {
                    ++three_d_streams;
                }
            }
            if (!complete) {
                // Name the binding that failed, so the reason is actionable rather than a count.
                for (u32 v = 0; v < candidate.vertex_binding_count && v < MaxBindingsPerDraw;
                     ++v) {
                    const auto& vertex = candidate.vertex_bindings[v];
                    if (vertex.retained_bytes != 0)
                        continue;
                    LOG_INFO(Core_Linker,
                             "DERACINE_DRAW_PACKAGE_CANDIDATE provenance_id={} "
                             "rejected=incomplete binding={} status={} guest_size={} stride={} "
                             "upload={}",
                             candidate.provenance_id, vertex.binding, vertex.status,
                             vertex.guest_size, vertex.stride, vertex.upload_sequence);
                    break;
                }
                if (candidate.index_binding.retained_bytes == 0) {
                    LOG_INFO(Core_Linker,
                             "DERACINE_DRAW_PACKAGE_CANDIDATE provenance_id={} "
                             "rejected=incomplete index status={} guest_size={}",
                             candidate.provenance_id, candidate.index_binding.status,
                             candidate.index_binding.guest_size);
                }
                continue;
            }
            // Weight three-dimensional structure heavily, so a small scene mesh outranks a large
            // screen-space element.
            score += static_cast<u64>(three_d_streams) * 1024ull * 1024ull;
            if (score > best_score) {
                best_score = score;
                best_index = i;
            }
        }
        if (best_index == MaxCapturedDraws) {
            LOG_INFO(Core_Linker, "DERACINE_DRAW_PACKAGE_NONE no draw had captured geometry");
            return;
        }

        for (u32 i = best_index; i < best_index + 1; ++i) {
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

            // Geometry: one resource entry per distinct host allocation and range, shared between
            // bindings that genuinely refer to the same version.
            std::map<std::tuple<u64, u64, u64>, u32> geometry_by_content;
            auto add_geometry_resource = [&](u64 host_buffer, u64 host_offset, u64 guest_base,
                                             u64 guest_size, u64 upload_sequence, u64 content_hash,
                                             u32 retained_offset, u32 retained_bytes,
                                             u8 status) -> u32 {
                if (retained_bytes == 0)
                    return 0;
                const auto key = std::make_tuple(host_buffer, host_offset, upload_sequence);
                if (const auto it = geometry_by_content.find(key);
                    it != geometry_by_content.end()) {
                    return it->second;
                }
                DrawPackage::Resource resource{};
                resource.id = next_id++;
                resource.requested_base = guest_base;
                resource.requested_size = guest_size;
                resource.clamped_base = guest_base;
                resource.clamped_size = guest_size;
                resource.content_hash = content_hash;
                resource.status = status;
                resource.bytes.resize(retained_bytes);
                if (!ReadRetainedBytes(retained_offset, retained_bytes, resource.bytes.data())) {
                    resource.bytes.clear();
                    return 0;
                }
                retained += resource.bytes.size();
                geometry_by_content[key] = resource.id;
                package.resources.push_back(std::move(resource));
                return geometry_by_content[key];
            };

            for (u32 v = 0; v < draw.vertex_binding_count && v < MaxBindingsPerDraw; ++v) {
                const auto& vertex = draw.vertex_bindings[v];
                DrawPackage::VertexBinding out{};
                out.binding = vertex.binding;
                out.host_buffer = vertex.host_buffer;
                out.host_offset = vertex.host_offset;
                out.guest_base = vertex.guest_base;
                out.guest_size = vertex.guest_size;
                out.stride = vertex.stride;
                out.input_rate = vertex.input_rate;
                out.divisor = vertex.divisor;
                out.upload_sequence = vertex.upload_sequence;
                out.content_hash = vertex.content_hash;
                out.status = vertex.status;
                out.semantic = vertex.semantic;
                out.vk_format = vertex.vk_format;
                out.attr_offset = vertex.attr_offset;
                out.element_count = vertex.element_count;
                out.vs_hash = vertex.vs_hash;
                out.ps_hash = vertex.ps_hash;
                out.resource_ref = add_geometry_resource(
                    vertex.host_buffer, vertex.host_offset, vertex.guest_base, vertex.guest_size,
                    vertex.upload_sequence, vertex.content_hash, vertex.retained_offset,
                    vertex.retained_bytes, vertex.status);
                package.vertex_bindings.push_back(out);
            }

            if (draw.has_index_binding) {
                const auto& index = draw.index_binding;
                DrawPackage::IndexBinding out{};
                out.host_buffer = index.host_buffer;
                out.host_offset = index.host_offset;
                out.guest_base = index.guest_base;
                out.guest_size = index.guest_size;
                out.index_size = index.index_size;
                out.index_count = index.index_count;
                out.first_index = index.first_index;
                out.base_vertex = index.base_vertex;
                out.instance_count = index.instance_count;
                out.first_instance = index.first_instance;
                out.primitive_type = index.primitive_type;
                out.index_type = index.index_type;
                out.swap_mode = index.swap_mode;
                out.upload_sequence = index.upload_sequence;
                out.content_hash = index.content_hash;
                out.status = index.status;
                out.resource_ref = add_geometry_resource(
                    index.host_buffer, index.host_offset, index.guest_base, index.guest_size,
                    index.upload_sequence, index.content_hash, index.retained_offset,
                    index.retained_bytes, index.status);
                package.index_binding = out;
                package.has_index_binding = true;
            }

            // Interpret the geometry: pick a position candidate, measure its bounds, and count the
            // triangles. The position choice is evidence-based rather than assumed, and the
            // evidence is recorded so a reader can disagree with it.
            {
                auto& geo = package.geometry;
                geo.bounds_min[0] = geo.bounds_min[1] = geo.bounds_min[2] = 1e30f;
                geo.bounds_max[0] = geo.bounds_max[1] = geo.bounds_max[2] = -1e30f;

                // A position candidate is a three- or four-component float attribute whose values
                // span more than one unit on at least two axes. A UV or a normal does not.
                u32 best_binding = 0;
                u32 best_components = 0;
                float best_span = 0.0f;
                for (const auto& vertex : package.vertex_bindings) {
                    if (vertex.resource_ref == 0 || vertex.element_count < 2)
                        continue;
                    if (vertex.vk_format != 106 && vertex.vk_format != 109) {
                        // Only 32-bit float formats are interpreted; anything else would be a
                        // guess about numeric type.
                        continue;
                    }
                    const auto& resource = package.resources[vertex.resource_ref - 1];
                    if (resource.bytes.size() < vertex.stride || vertex.stride == 0)
                        continue;
                    const u32 count = static_cast<u32>(resource.bytes.size() / vertex.stride);
                    float lo[3] = {1e30f, 1e30f, 1e30f};
                    float hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (u32 v = 0; v < count; ++v) {
                        for (u32 c = 0; c < 3 && c < vertex.element_count; ++c) {
                            float value = 0.0f;
                            std::memcpy(&value,
                                        resource.bytes.data() + v * vertex.stride + c * 4,
                                        sizeof(value));
                            if (!std::isfinite(value)) {
                                ++geo.nonfinite_vertices;
                                continue;
                            }
                            lo[c] = std::min(lo[c], value);
                            hi[c] = std::max(hi[c], value);
                        }
                    }
                    // Rank by the second-largest axis span, so a flat quad does not win on one wide
                    // axis alone.
                    std::array<float, 3> spans{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
                    std::sort(spans.begin(), spans.end(), std::greater<float>());
                    const float score = spans[1];
                    if (score > best_span) {
                        best_span = score;
                        best_binding = vertex.binding;
                        best_components = vertex.element_count;
                        for (u32 c = 0; c < 3; ++c) {
                            geo.bounds_min[c] = lo[c];
                            geo.bounds_max[c] = hi[c];
                        }
                    }
                }
                geo.position_binding = best_binding;
                geo.position_components = best_components;
                geo.position_evidence =
                    "largest second-axis span among 32-bit float attributes";
                geo.is_three_dimensional = best_span > 1.0f;

                // Triangle statistics from the captured indices.
                if (package.has_index_binding && package.index_binding.resource_ref != 0) {
                    const auto& index_resource =
                        package.resources[package.index_binding.resource_ref - 1];
                    const u32 width = package.index_binding.index_size;
                    const u32 count = package.index_binding.index_count;
                    if ((width == 2 || width == 4) &&
                        index_resource.bytes.size() >= static_cast<size_t>(count) * width) {
                        std::vector<u32> indices(count);
                        for (u32 i = 0; i < count; ++i) {
                            if (width == 2) {
                                u16 v = 0;
                                std::memcpy(&v, index_resource.bytes.data() + i * 2, sizeof(v));
                                indices[i] = v;
                            } else {
                                std::memcpy(&indices[i], index_resource.bytes.data() + i * 4,
                                            sizeof(u32));
                            }
                        }
                        geo.triangle_count = count / 3;
                        std::set<u32> unique{indices.begin(), indices.end()};
                        geo.unique_referenced_vertices = static_cast<u32>(unique.size());
                        for (u32 t = 0; t + 2 < count; t += 3) {
                            if (indices[t] == indices[t + 1] || indices[t + 1] == indices[t + 2] ||
                                indices[t] == indices[t + 2]) {
                                ++geo.degenerate_triangles;
                            }
                        }
                    }
                }
            }

            // Readiness is derived per dimension, not asserted as one flag.
            package.readiness.shader_buffers_ready = !package.bindings.empty();
            for (const auto& binding : package.bindings) {
                if (binding.resource_ref == 0)
                    continue;
                const auto& resource = package.resources[binding.resource_ref - 1];
                if (resource.status != static_cast<u8>(CaptureStatus::HostContentVerified) &&
                    resource.status != static_cast<u8>(CaptureStatus::HostContentCaptured)) {
                    package.readiness.shader_buffers_ready = false;
                    break;
                }
            }

            package.readiness.geometry_ready = !package.vertex_bindings.empty();
            for (const auto& vertex : package.vertex_bindings) {
                if (vertex.resource_ref == 0) {
                    package.readiness.geometry_ready = false;
                    break;
                }
                const auto& resource = package.resources[vertex.resource_ref - 1];
                if (resource.status != static_cast<u8>(CaptureStatus::HostContentVerified) &&
                    resource.status != static_cast<u8>(CaptureStatus::HostContentCaptured)) {
                    package.readiness.geometry_ready = false;
                    break;
                }
            }
            if (package.readiness.geometry_ready && package.is_indexed) {
                if (!package.has_index_binding || package.index_binding.resource_ref == 0) {
                    package.readiness.geometry_ready = false;
                }
            }

            // Textures and pipeline state are not captured by this phase, so those dimensions stay
            // false and the limitation says why.
            package.readiness.textures_ready = false;
            package.readiness.pipeline_state_ready = false;
            package.readiness.diagnostic_geometry_replay_ready =
                package.readiness.geometry_ready;
            package.readiness.full_draw_replay_ready = false;
            if (package.readiness.diagnostic_geometry_replay_ready) {
                package.readiness.limitation =
                    "diagnostic geometry replay only: no textures, no original shaders, no "
                    "material state";
            } else {
                package.readiness.limitation = "geometry is incomplete or not host-verified";
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
