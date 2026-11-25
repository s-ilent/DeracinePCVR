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
#include "common/sha256.h"

#include "core/capture_session.h"
#include "core/draw_package.h"
#include "core/reference_output.h"
#include "core/shader_binary_capture.h"
#include "core/shader_interface_capture.h"
#include "core/texture_capture.h"

namespace Core::ShaderParamCapture {

namespace {

/// Formats a SHA-256 digest as lowercase hex, which is the form the package stores.
[[nodiscard]] std::string HexDigest(const std::array<u8, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const u8 byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

/// Hashes a byte span and formats it as lowercase hex.
[[nodiscard]] std::string Sha256Hex(const std::vector<u8>& bytes) {
    sha256::SHA256 hasher;
    if (!bytes.empty()) {
        hasher.update(bytes.data(), bytes.size());
    }
    return HexDigest(hasher.final());
}

} // namespace

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

bool RetainHostBytes(const u8* host_bytes, u32 size, u32& pool_offset, u32& retained,
                     u64& content_hash) noexcept {
    try {
        pool_offset = 0;
        retained = 0;
        content_hash = 0;
        if (!g_state.enabled.load(std::memory_order_relaxed))
            return false;
        if (host_bytes == nullptr || size == 0)
            return false;
        const u32 want = static_cast<u32>(std::min<u64>(size, MaxBytesPerResource));
        if (want == 0)
            return false;
        const u32 offset = g_retained_used.fetch_add(want, std::memory_order_relaxed);
        if (static_cast<size_t>(offset) + want > g_retained_pool.size())
            return false;
        std::memcpy(g_retained_pool.data() + offset, host_bytes, want);
        pool_offset = offset;
        retained = want;
        // The hash covers exactly the bytes retained, so the package's hash and its bytes always
        // agree.
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
/// Converts an IEEE 754 binary16 value to float. A UV stored as 16-bit float is common in UE4
/// material streams, and reading it as anything else would be a guess.
float HalfToFloat(u16 half) noexcept {
    const u32 sign = (half >> 15) & 0x1u;
    const u32 exponent = (half >> 10) & 0x1Fu;
    const u32 mantissa = half & 0x3FFu;
    u32 bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            // Subnormal: normalize it.
            u32 e = 0;
            u32 m = mantissa;
            while ((m & 0x400u) == 0) {
                m <<= 1;
                ++e;
            }
            m &= 0x3FFu;
            bits = (sign << 31) | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exponent == 0x1F) {
        bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
    } else {
        bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

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
            // A draw whose texture content was actually captured is worth more than one whose
            // texture is only a binding, because only the former can be replayed textured. The
            // weight is large but not absolute: a draw with no geometry still cannot be replayed,
            // so the geometry requirement above remains the gate.
            //
            // The weight is applied only to a draw that also has three-dimensional geometry. A
            // screen-space quad with a captured texture would otherwise outrank a scene mesh, and
            // a textured replay of a quad proves nothing about the recovered material path.
            if (three_d_streams >= 2 && TextureCapture::HasContentFor(candidate.provenance_id)) {
                score += 4ull * 1024ull * 1024ull * 1024ull;
            }
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
            // The one capture session identity, so the package can be shown to come from the same
            // execution as the tape and the index bridge.
            package.session_id = Core::CaptureSession::SessionId();
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
            u64 blob_offset = 0;

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

                // Texture coordinates: a two-component float attribute whose values sit in or
                // near [0,1] on both axes, on a binding that is not the position. A normal or a
                // tangent is normalized but three- or four-component; a colour is eight-bit. The
                // range test is what separates a UV from those, and the evidence says so.
                auto& uv = package.texture_coordinate;
                uv.min_u = uv.min_v = 1e30f;
                uv.max_u = uv.max_v = -1e30f;
                u32 uv_binding = 0;
                float uv_score = -1.0f;
                for (const auto& vertex : package.vertex_bindings) {
                    if (vertex.resource_ref == 0)
                        continue;
                    // A UV is usually its own two-component attribute, but a material can pack it
                    // into a four-component stream alongside something else. Both are searched;
                    // the range test is what decides, not the component count.
                    if (vertex.element_count != 2 && vertex.element_count != 4)
                        continue;
                    // Two-component formats that can hold a UV: 32-bit float, 16-bit float, and
                    // 16-bit normalized. A title screen uses whichever the material asked for, so
                    // restricting to 32-bit float would miss most of them.
                    const bool is_f32 = vertex.vk_format == 103;
                    // R16G16_SFLOAT and R16G16B16A16_SFLOAT are both half-float streams; a
                    // material commonly packs UV0 and UV1 into the four-component one.
                    const bool is_f16 = vertex.vk_format == 83 || vertex.vk_format == 97;
                    const bool is_unorm16 = vertex.vk_format == 84;
                    if (!is_f32 && !is_f16 && !is_unorm16) {
                        continue;
                    }
                    if (vertex.binding == geo.position_binding)
                        continue;
                    const auto& resource = package.resources[vertex.resource_ref - 1];
                    if (vertex.stride == 0 || resource.bytes.size() < vertex.stride)
                        continue;
                    const u32 count = static_cast<u32>(resource.bytes.size() / vertex.stride);
                    const u32 components = vertex.element_count;
                    const u32 width = is_f32 ? 4u : 2u;
                    std::array<float, 4> lo{1e30f, 1e30f, 1e30f, 1e30f};
                    std::array<float, 4> hi{-1e30f, -1e30f, -1e30f, -1e30f};
                    for (u32 v = 0; v < count; ++v) {
                        for (u32 c = 0; c < components; ++c) {
                            float value = 0.0f;
                            if (is_f32) {
                                std::memcpy(&value,
                                            resource.bytes.data() + v * vertex.stride + c * 4,
                                            sizeof(value));
                            } else {
                                u16 raw = 0;
                                std::memcpy(&raw,
                                            resource.bytes.data() + v * vertex.stride + c * 2,
                                            sizeof(raw));
                                value = is_unorm16 ? static_cast<float>(raw) / 65535.0f
                                                   : HalfToFloat(raw);
                            }
                            if (!std::isfinite(value))
                                continue;
                            lo[c] = std::min(lo[c], value);
                            hi[c] = std::max(hi[c], value);
                        }
                    }
                    // Score how well a component pair matches normalized coordinates: both axes
                    // inside a small margin around [0,1], and not degenerate.
                    const auto in_unit = [](float a, float b) {
                        return a >= -0.05f && b <= 1.05f && (b - a) > 0.01f;
                    };
                    for (u32 pair = 0; pair + 1 < components; pair += 2) {
                        const float score =
                            in_unit(lo[pair], hi[pair]) && in_unit(lo[pair + 1], hi[pair + 1])
                                ? 1.0f
                                : 0.0f;
                        if (score > uv_score) {
                            uv_score = score;
                            uv_binding = vertex.binding;
                            uv.component_u = pair;
                            uv.component_v = pair + 1;
                            uv.min_u = lo[pair];
                            uv.max_u = hi[pair];
                            uv.min_v = lo[pair + 1];
                            uv.max_v = hi[pair + 1];
                        }
                    }
                }
                uv.binding = uv_binding;
                uv.looks_like_uv = uv_score > 0.0f;
                uv.evidence = uv.looks_like_uv
                                  ? "two-component float attribute with both axes inside [0,1]"
                                  : "no two-component float attribute had a normalized range";

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

            // Readiness is derived per dimension, not asserted as one flag. A non-empty binding
            // list is not proof that every required shader buffer is available: one captured
            // binding beside three missing ones would otherwise read as ready.
            //
            // A binding with no resource is a descriptor the draw used and the capture could not
            // retain, which is not readiness. A resource with no bytes is the same.
            package.readiness.shader_buffers_ready = !package.bindings.empty();
            for (const auto& binding : package.bindings) {
                if (binding.resource_ref == 0) {
                    package.readiness.shader_buffers_ready = false;
                    break;
                }
                if (binding.resource_ref > package.resources.size()) {
                    package.readiness.shader_buffers_ready = false;
                    break;
                }
                const auto& resource = package.resources[binding.resource_ref - 1];
                if (resource.bytes.empty()) {
                    package.readiness.shader_buffers_ready = false;
                    break;
                }
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

            // Textures: take the records the binding path observed for this draw, and give each
            // captured subresource its own blob range.
            {
                const auto images = TextureCapture::Images();
                const auto samplers = TextureCapture::Samplers();
                for (const auto& sampler : samplers) {
                    // Only the selected draw's observations belong in its package. A sampler from
                    // another draw may share the shader, the binding number and even the state, and
                    // admitting it would let an unrelated descriptor satisfy a declaration.
                    if (sampler.provenance_id != draw.provenance_id) {
                        continue;
                    }
                    DrawPackage::Sampler out{};
                    out.id = sampler.id;
                    // The binding identity travels with the state, so a validator can check that
                    // the sampler occupying a declared binding is the one the draw used rather than
                    // merely that some sampler exists.
                    out.vk_binding = sampler.vk_binding;
                    out.has_vk_binding = sampler.has_vk_binding;
                    out.logical_stage = sampler.logical_stage;
                    out.provenance_id = sampler.provenance_id;
                    out.guest_hash = sampler.guest_hash;
                    out.mag_filter = sampler.mag_filter;
                    out.min_filter = sampler.min_filter;
                    out.mip_filter = sampler.mip_filter;
                    out.address_u = sampler.address_u;
                    out.address_v = sampler.address_v;
                    out.address_w = sampler.address_w;
                    out.lod_bias = sampler.lod_bias;
                    out.min_lod = sampler.min_lod;
                    out.max_lod = sampler.max_lod;
                    out.anisotropy_enable = sampler.anisotropy_enable;
                    out.max_anisotropy = sampler.max_anisotropy;
                    out.compare_enable = sampler.compare_enable;
                    out.compare_op = sampler.compare_op;
                    out.border_color = sampler.border_color;
                    out.unnormalized_coordinates = sampler.unnormalized_coordinates;
                    package.samplers.push_back(out);
                }

                // The binding path records each descriptor once, and the snapshot records the
                // same descriptor again with its contents. Those are one texture, not two: the
                // late record is joined onto the binding record by the identity the two share,
                // rather than appended as a separate entry.
                u32 next_texture_id = 1;
                // The array element is part of the join identity. Two elements of one binding may
                // reference the same image and view, and joining them would collapse two descriptors
                // into one, leaving the second element unrepresented.
                std::map<std::tuple<u64, u64, u32, u32, u32, u32>, u32> by_binding;
                for (const auto& image : images) {
                    if (image.provenance_id != draw.provenance_id)
                        continue;

                    const auto join_key = std::make_tuple(image.stage_hash, image.image_uid,
                                                          image.vk_binding, image.array_element,
                                                          image.mip_first, image.layer_first);
                    if (const auto it = by_binding.find(join_key); it != by_binding.end()) {
                        // An existing entry for this binding: attach the contents to it.
                        auto& existing = package.textures[it->second - 1];
                        if (!image.bytes.empty() && image.row_pitch != 0) {
                            DrawPackage::TextureSubresource sub{};
                            sub.mip = image.mip_first;
                            sub.layer = image.layer_first;
                            sub.width = image.width;
                            sub.height = image.height;
                            sub.row_pitch = image.row_pitch;
                            sub.blob_offset = blob_offset;
                            sub.byte_length = image.bytes.size();
                            sub.content_hash = image.content_hash;
                            blob_offset += image.bytes.size();
                            retained += image.bytes.size();
                            package.texture_bytes.insert(package.texture_bytes.end(),
                                                         image.bytes.begin(), image.bytes.end());
                            existing.subresources.push_back(sub);
                            existing.status = image.status;
                            existing.transfer_id = image.transfer_id;
                            existing.block_columns = image.block_width;
                            existing.block_rows = image.block_height;
                            existing.block_texels_x = image.block_texels_x;
                            existing.block_texels_y = image.block_texels_y;
                            existing.bytes_per_block = image.bytes_per_block;
                            existing.limitation.clear();
                        }
                        continue;
                    }

                    DrawPackage::Texture out{};
                    out.id = next_texture_id++;
                    out.vk_binding = image.vk_binding;
                    // The element identity travels with the binding. A descriptor with
                    // `descriptorCount = N` is one binding with N elements, and the package must
                    // preserve which element each record is.
                    out.array_element = image.array_element;
                    out.array_count = image.array_count;
                    out.stage = image.stage;
                    out.stage_hash = image.stage_hash;
                    out.guest_address = image.guest_address;
                    out.image_id = image.image_id;
                    out.image_uid = image.image_uid;
                    out.image_format = image.image_format;
                    out.view_format = image.view_format;
                    out.aspect = image.aspect;
                    out.mip_first = image.mip_first;
                    out.mip_count = image.mip_count;
                    out.layer_first = image.layer_first;
                    out.layer_count = image.layer_count;
                    out.width = image.width;
                    out.height = image.height;
                    out.depth = image.depth;
                    out.samples = image.samples;
                    out.status = image.status;
                    out.transfer_id = image.transfer_id;
                    out.block_columns = image.block_width;
                    out.block_rows = image.block_height;
                    out.block_texels_x = image.block_texels_x;
                    out.block_texels_y = image.block_texels_y;
                    out.bytes_per_block = image.bytes_per_block;

                    // Join the sampler through the shader's own pairing. The image record carries
                    // the shader-level image index it was bound as, and a sampler whose
                    // `associated_image` names that index is the one the shader samples with.
                    // Without that evidence the association stays unresolved rather than being
                    // guessed from binding order.
                    for (const auto& sampler : samplers) {
                        if (sampler.associated_image == image.shader_image_index &&
                            sampler.stage_hash == image.stage_hash) {
                            out.sampler_id = sampler.id;
                            out.sampler_verified = true;
                            break;
                        }
                    }

                    if (!image.bytes.empty() && image.row_pitch != 0) {
                        DrawPackage::TextureSubresource sub{};
                        sub.mip = image.mip_first;
                        sub.layer = image.layer_first;
                        sub.width = image.width;
                        sub.height = image.height;
                        sub.row_pitch = image.row_pitch;
                        sub.blob_offset = blob_offset;
                        sub.byte_length = image.bytes.size();
                        sub.content_hash = image.content_hash;
                        blob_offset += image.bytes.size();
                        retained += image.bytes.size();
                        package.texture_bytes.insert(package.texture_bytes.end(),
                                                     image.bytes.begin(), image.bytes.end());
                        out.subresources.push_back(sub);
                    } else if (image.status ==
                               static_cast<u8>(TextureCapture::ContentStatus::Unsupported)) {
                        out.limitation = "the image cannot be copied safely";
                    } else {
                        out.limitation = "content was not captured";
                    }
                    by_binding[join_key] = out.id;
                    package.textures.push_back(std::move(out));
                }
            }

            package.readiness.texture_binding_ready = !package.textures.empty();
            // Content readiness means every required subresource of every bound texture was
            // captured, not merely that each texture has some bytes. A view that asks for four mip
            // levels and got one is not complete, and claiming otherwise would describe a partial
            // capture as a complete original resource.
            package.readiness.texture_contents_ready = !package.textures.empty();
            for (const auto& texture : package.textures) {
                if (texture.subresources.empty()) {
                    package.readiness.texture_contents_ready = false;
                    break;
                }
                // The required set is the view's mip and layer range. Every (mip, layer) pair in
                // it must appear exactly once in the captured set.
                const u32 required_mips = texture.mip_count != 0 ? texture.mip_count : 1;
                const u32 required_layers = texture.layer_count != 0 ? texture.layer_count : 1;
                u32 matched = 0;
                for (u32 m = 0; m < required_mips; ++m) {
                    for (u32 l = 0; l < required_layers; ++l) {
                        const u32 want_mip = texture.mip_first + m;
                        const u32 want_layer = texture.layer_first + l;
                        const bool present = std::ranges::any_of(
                            texture.subresources, [&](const DrawPackage::TextureSubresource& sub) {
                                return sub.mip == want_mip && sub.layer == want_layer;
                            });
                        if (present) {
                            ++matched;
                        }
                    }
                }
                if (matched != required_mips * required_layers) {
                    package.readiness.texture_contents_ready = false;
                    break;
                }
            }
            // A capture that hit the record budget is truncated, so it cannot claim completeness.
            if (TextureCapture::CapacityExhausted()) {
                package.readiness.texture_contents_ready = false;
            }
            package.readiness.sampler_ready = !package.samplers.empty();
            package.readiness.pipeline_state_ready = false;

            // The effective shader stages. The identity is (program hash, permutation index), which
            // is what the pipeline key carries in stage_hashes. A program hash alone is not unique:
            // one program compiles to several permutations, so a hash match alone would not
            // identify the module this draw executed.
            {
                const auto add_stage = [&](u64 permutation_hash, u32 logical_stage) {
                    Core::ShaderBinaryCapture::RetainedBinary binary{};
                    if (!Core::ShaderBinaryCapture::FindByPermutationHash(permutation_hash,
                                                                          binary)) {
                        return false;
                    }
                    DrawPackage::ShaderStage stage{};
                    stage.logical_stage = binary.identity.logical_stage;
                    stage.hardware_stage = binary.identity.hardware_stage;
                    stage.program_hash = binary.identity.program_hash;
                    stage.permutation_index = binary.identity.permutation_index;
                    stage.permutation_hash = binary.identity.permutation_hash;
                    stage.patched = binary.patched;
                    stage.entry_point = std::string{binary.entry_point};
                    // The effective module is the patch when one was applied, and the generated
                    // SPIR-V otherwise. Keeping the two hashes distinct is what stops a consumer
                    // from loading the unpatched module and believing it is what the draw ran.
                    const auto& effective =
                        binary.patched && !binary.patch_spirv.empty() ? binary.patch_spirv
                                                                      : binary.spirv;
                    const auto& digest = binary.patched && !binary.patch_spirv.empty()
                                             ? binary.effective_sha256
                                             : binary.generated_sha256;
                    stage.effective_sha256 = HexDigest(digest);
                    stage.generated_sha256 = HexDigest(binary.generated_sha256);
                    stage.effective_byte_length = effective.size() * sizeof(u32);
                    stage.gcn_byte_length = binary.gcn_code.size() * sizeof(u32);
                    stage.effective_bytes.resize(stage.effective_byte_length);
                    std::memcpy(stage.effective_bytes.data(), effective.data(),
                                stage.effective_bytes.size());
                    stage.gcn_bytes.resize(stage.gcn_byte_length);
                    if (!binary.gcn_code.empty()) {
                        std::memcpy(stage.gcn_bytes.data(), binary.gcn_code.data(),
                                    stage.gcn_bytes.size());
                    }
                    const char* stage_name = logical_stage == 3 ? "vs" : "ps";
                    stage.effective_file =
                        std::string{"shader_"} + stage_name + "_" +
                        std::to_string(stage.permutation_hash) + ".spv";
                    stage.gcn_file = std::string{"shader_"} + stage_name + "_" +
                                     std::to_string(stage.permutation_hash) + ".gcn";
                    package.shader_stages.push_back(std::move(stage));
                    return true;
                };
                const bool have_vs = add_stage(draw.vs_hash, 3);
                const bool have_ps = add_stage(draw.ps_hash, 0);
                package.readiness.shader_binaries_ready = have_vs && have_ps;

                // The descriptor interface of each effective stage, keyed by permutation hash.
                //
                // The declaration's own `binding` is the shader-local number, not the Vulkan one.
                // The effective number comes from the pipeline's resolved mapping, which is keyed by
                // the identity the draw carries. A descriptor whose mapping cannot be resolved is
                // exported with `binding_resolved = false` and the validator refuses it, rather than
                // silently presenting a local number as a global one.
                const auto add_interface = [&](u64 permutation_hash) {
                    Core::ShaderInterface::Interface captured{};
                    if (!Core::ShaderInterface::FindByPermutationHash(permutation_hash,
                                                                      captured)) {
                        return false;
                    }
                    DrawPackage::ShaderInterface out{};
                    out.program_hash = captured.program_hash;
                    out.permutation_index = captured.permutation_index;
                    out.permutation_hash = captured.permutation_hash;
                    out.hardware_stage = captured.hardware_stage;
                    out.logical_stage = captured.logical_stage;
                    out.patched = captured.patched;
                    out.push_constant_offset = captured.push_constant.offset;
                    out.push_constant_size = captured.push_constant.size;
                    out.push_constant_stage_flags = captured.push_constant.stage_flags;
                    out.unified_binding_count = captured.unified_binding_count;
                    out.pipeline_id = draw.pipeline_hash;
                    for (u32 d = 0; d < captured.descriptor_count; ++d) {
                        const auto& descriptor = captured.descriptors[d];
                        DrawPackage::ShaderDescriptor entry{};
                        entry.set = descriptor.set;
                        // Both identities are preserved: the shader's own declaration and the
                        // effective Vulkan binding the pipeline allocated.
                        entry.local_binding = descriptor.local_binding;
                        entry.binding = descriptor.binding;
                        entry.binding_resolved = false;
                        u32 resolved = 0;
                        if (Core::ShaderInterface::ResolveDescriptorBinding(
                                draw.pipeline_hash, captured.logical_stage,
                                descriptor.local_binding, resolved)) {
                            entry.binding = resolved;
                            entry.binding_resolved = true;
                        }
                        entry.descriptor_class = static_cast<u32>(descriptor.cls);
                        entry.count = descriptor.count;
                        entry.stage_flags = descriptor.stage_flags;
                        entry.guest_resource_index = descriptor.guest_resource_index;
                        entry.written = descriptor.written;
                        out.descriptors.push_back(entry);
                    }
                    package.shader_interfaces.push_back(std::move(out));
                    return true;
                };
                add_interface(draw.vs_hash);
                add_interface(draw.ps_hash);
            }

            // The graphics pipeline state the draw used.
            {
                const auto& in = draw.pipeline_state;
                auto& out = package.pipeline_state;
                // Presence is recorded explicitly. Zero is a legitimate value for every one of
                // these enums, so a consumer cannot decide presence from the value: it must be told.
                // The capture records these fields unconditionally from the live pipeline key, so
                // each is present when the draw was captured at all.
                out.present.topology = true;
                out.present.cull_mode = true;
                out.present.front_face = true;
                out.present.polygon_mode = true;
                out.present.depth_test = true;
                out.present.depth_write = true;
                out.present.depth_compare_op = true;
                out.present.samples = true;
                out.present.depth_format = true;
                out.present.viewport = true;
                out.has_topology = true;
                out.topology = in.topology;
                out.cull_mode = in.cull_mode;
                out.front_face = in.front_face;
                out.polygon_mode = in.polygon_mode;
                out.depth_test = in.depth_test;
                out.depth_write = in.depth_write;
                out.depth_compare_op = in.depth_compare_op;
                out.depth_format = in.depth_format;
                out.samples = in.samples;
                for (u32 i = 0; i < in.color_format_count && i < 8; ++i) {
                    out.color_formats.push_back(in.color_formats[i]);
                    out.blend_enable.push_back(in.blend_enable[i]);
                    out.write_mask.push_back(in.write_mask[i]);
                }
                // The array-backed fields are present only when the capture recorded an entry for
                // them. An empty array is absent, not a zero value.
                out.present.color_formats = !out.color_formats.empty();
                out.present.blend_enable = !out.blend_enable.empty();
                out.present.write_mask = !out.write_mask.empty();
                out.viewport_x = in.viewport_x;
                out.viewport_y = in.viewport_y;
                out.viewport_width = in.viewport_width;
                out.viewport_height = in.viewport_height;
                out.viewport_min_depth = in.viewport_min_depth;
                out.viewport_max_depth = in.viewport_max_depth;
                out.push_constant_offset = in.push_constant_offset;
                // The declared size must fit the captured words. Resizing to the declared size and
                // copying the smaller of the two leaves a zero-filled tail that was never captured
                // from the draw, and the replay would then accept fabricated parameters as genuine
                // captured state. An oversized range is reported rather than truncated.
                constexpr size_t kPushConstantCapacity = sizeof(in.push_constant_words);
                const bool push_constants_fit = in.push_constant_size <= kPushConstantCapacity;
                if (push_constants_fit) {
                    out.push_constant_size = in.push_constant_size;
                    out.push_constant_bytes.assign(
                        reinterpret_cast<const u8*>(in.push_constant_words),
                        reinterpret_cast<const u8*>(in.push_constant_words) +
                            in.push_constant_size);
                } else {
                    // The range is not representable. The size is recorded so a reader can see what
                    // was asked for, but no bytes are claimed: a partial block presented as complete
                    // would be worse than an absent one.
                    out.push_constant_size = 0;
                    out.push_constant_bytes.clear();
                    package.readiness.limitation =
                        "the push-constant range exceeds the capture capacity";
                }

                // Readiness is evidence-based, not value-based. Zero is a legitimate value for
                // every one of these enums, so presence comes from the explicit flags the capture
                // recorded rather than from testing a value against zero.
                package.readiness.pipeline_state_ready_original =
                    out.present.topology && out.present.color_formats && push_constants_fit;
            }

            // The reference output around the draw, when it was captured.
            {
                Core::ReferenceOutput::ReferenceImage pre{};
                Core::ReferenceOutput::ReferenceImage post{};
                bool has_pre = false;
                if (Core::ReferenceOutput::GetReference(draw.provenance_id, pre, post, has_pre)) {
                    auto& out = package.reference_output;
                    out.width = post.width;
                    out.height = post.height;
                    out.format = post.format;
                    out.bytes_per_pixel = post.bytes_per_pixel;
                    out.has_pre = has_pre;
                    out.pre_sha256 = Sha256Hex(pre.bytes);
                    out.post_sha256 = Sha256Hex(post.bytes);
                    out.pre_file = has_pre ? "reference_pre.bin" : "";
                    out.post_file = "reference_post.bin";
                    package.reference_pre_bytes = std::move(pre.bytes);
                    package.reference_post_bytes = std::move(post.bytes);
                    package.readiness.reference_output_ready =
                        !out.post_sha256.empty() && out.width != 0 && out.height != 0;
                }
            }

            // The shader bindings and parameters are ready when the binaries are and every binding
            // the draw recorded resolved to host content.
            package.readiness.shader_bindings_ready =
                package.readiness.shader_binaries_ready && !package.bindings.empty();
            package.readiness.shader_parameters_ready =
                package.readiness.shader_bindings_ready &&
                package.pipeline_state.push_constant_bytes.size() ==
                    package.pipeline_state.push_constant_size;
            package.readiness.diagnostic_geometry_replay_ready =
                package.readiness.geometry_ready;
            // A diagnostic textured replay needs one usable texture, not every bound texture. The
            // strict `texture_contents_ready` stays false while any binding is uncaptured, so the
            // two claims stay distinct: a diagnostic can sample what was captured, but the draw's
            // original material cannot be reproduced from an incomplete set.
            bool any_texture_content = false;
            for (const auto& texture : package.textures) {
                if (!texture.subresources.empty()) {
                    any_texture_content = true;
                    break;
                }
            }
            // A textured replay of a screen-space quad would prove nothing about the recovered
            // material path, so the geometry must also be three-dimensional.
            package.readiness.diagnostic_textured_replay_ready =
                package.readiness.diagnostic_geometry_replay_ready && any_texture_content &&
                package.readiness.sampler_ready && package.texture_coordinate.looks_like_uv &&
                package.geometry.is_three_dimensional;
            package.readiness.original_material_ready = false;

            if (package.readiness.diagnostic_textured_replay_ready) {
                package.readiness.limitation =
                    package.readiness.texture_contents_ready
                        ? "diagnostic textured replay only: no original shaders, no material state"
                        : "diagnostic textured replay from the captured texture only: another "
                          "binding on this draw was not captured";
            } else if (package.readiness.diagnostic_geometry_replay_ready) {
                package.readiness.limitation =
                    "diagnostic geometry replay only: no captured texture content";
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
