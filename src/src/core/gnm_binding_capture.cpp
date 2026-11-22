// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/gnm_binding_capture.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "common/arch.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/cpu_patches.h"
#include "core/gnm_shader_metadata_parse.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/memory.h"
#include "core/loader/elf.h"
#include "core/module.h"

namespace Core::GnmBindingCapture {

namespace {

constexpr std::string_view ModuleName = "eboot.bin";
constexpr std::string_view TitleId = "CUSA11919";

/// The approved original Déraciné 1.00 SELF. The observation is refused on any other image, so a
/// record can never be attributed to a build the Atlas did not verify.
constexpr u64 ApprovedImageSize = 81506767;
constexpr std::string_view ApprovedImageSha =
    "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0";

/// The writer prologue, verified against the original image before the patch is installed.
constexpr std::array<u8, 16> WriterPrologue = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};

/// The grouped binder prologue.
constexpr std::array<u8, 16> BinderPrologue = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                               0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};

struct CaptureState {
    std::atomic<bool> enabled{};
    std::atomic<bool> exported{};
    std::atomic<u32> dropped{};
    std::atomic<u32> writer_calls{};
    std::atomic<u32> binder_calls{};
    std::atomic<u32> unmapped_slot{};
    std::atomic<u32> no_map_object{};
    std::atomic<u32> no_shader_hash{};
    std::atomic<u64> next_sequence{};
    std::atomic<u32> diag_program_read_failed{};
    std::atomic<u32> diag_program_null{};
    std::atomic<u32> diag_no_token{};
    std::atomic<u32> diag_no_signature{};
    std::atomic<u32> diag_last_head0{};
    std::atomic<u32> diag_dump_count{};
    std::atomic<u64> diag_last_code{};
    std::atomic<u32> diag_last_size{};
    struct DiagDump {
        VAddr program_data;
        VAddr shader_resource;
        u32 words[16];
    };
    std::vector<DiagDump> diag_dumps;
    std::mutex mutex;
    std::vector<BindingRecord> records;
    std::vector<BinderRecord> binder_records;
    std::atomic<u32> binder_no_pending{};
    std::atomic<u32> upload_calls{};
    VAddr last_upload_object{};
    u32 last_upload_size{};
    bool last_upload_valid{};
    MemoryManager* memory{};
    VAddr module_base{};
    GuestEntryObserverHandle writer_handle{};
    GuestEntryObserverHandle binder_handle{};
    GuestEntryObserverHandle upload_handle{};
};

CaptureState g_state{};

[[nodiscard]] bool IsOptedIn(const char* value) noexcept {
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] u64 CurrentGuestThreadId() noexcept {
    const auto* thread = Libraries::Kernel::g_curthread;
    if (thread == nullptr) {
        return 0;
    }
    return static_cast<u64>(thread->tid.load(std::memory_order_relaxed));
}

[[nodiscard]] u64 CurrentNativeThreadId() noexcept {
#ifdef _WIN32
    return static_cast<u64>(GetCurrentThreadId());
#else
    return 0;
#endif
}

[[nodiscard]] bool ReadGuest(VAddr address, void* out, size_t size) noexcept {
    if (address == 0 || size == 0) {
        return false;
    }
    auto* memory = g_state.memory;
    if (memory == nullptr || !memory->IsValidMapping(address, size)) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}

template <typename T>
[[nodiscard]] bool ReadGuestValue(VAddr address, T& out) noexcept {
    return ReadGuest(address, &out, sizeof(T));
}

/// Resolves the shader hash behind a stage map object.
///
/// The map object is the shader resource's metadata at +0xB0 (Atlas A-025), so the resource is
/// recovered by subtracting that offset. The resource holds the program data at +0x1D8 (SC-02),
/// and the program's own first word carries the trailer offset (A-028R: 248/248 agreement).
[[nodiscard]] bool ResolveShader(VAddr map_object, VAddr& shader_resource, VAddr& program_data,
                                 VAddr& trailer, u64& shader_hash) noexcept {
    shader_resource = 0;
    program_data = 0;
    trailer = 0;
    shader_hash = 0;

    if (map_object < ShaderResourceMetadataOffset) {
        return false;
    }
    shader_resource = map_object - ShaderResourceMetadataOffset;

    if (!ReadGuestValue(shader_resource + ShaderResourceProgramData, program_data)) {
        g_state.diag_program_read_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (program_data == 0) {
        g_state.diag_program_null.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // The shader resource's program-data pointer addresses the original *stage record*, not the
    // code. Its layout is the one the original decoder reads (Atlas A-027): word 0 carries the
    // 23-bit stage size, word 2 the low half of the program address. The trailer sits at
    // program + size - 0x1C.
    u32 stage_record[4]{};
    if (!ReadGuest(program_data, stage_record, sizeof(stage_record))) {
        return false;
    }
    const u32 size_field = stage_record[0] & 0x7FFFFF;
    const VAddr code_address = static_cast<VAddr>(stage_record[2]) << 8;
    if (size_field < GnmShaderMetadata::TrailerSize || code_address == 0) {
        g_state.diag_no_token.fetch_add(1, std::memory_order_relaxed);
        g_state.diag_last_head0.store(stage_record[0], std::memory_order_relaxed);
        return false;
    }
    trailer = code_address + size_field - GnmShaderMetadata::TrailerSize;
    g_state.diag_last_code.store(code_address, std::memory_order_relaxed);
    g_state.diag_last_size.store(size_field, std::memory_order_relaxed);

    std::array<u8, GnmShaderMetadata::TrailerSize> trailer_bytes{};
    if (!ReadGuest(trailer, trailer_bytes.data(), trailer_bytes.size())) {
        return false;
    }
    if (std::memcmp(trailer_bytes.data(), GnmShaderMetadata::OrbShdrSignature.data(),
                    GnmShaderMetadata::OrbShdrSignature.size()) != 0) {
        g_state.diag_no_signature.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::memcpy(&shader_hash, trailer_bytes.data() + GnmShaderMetadata::OffHash, sizeof(shader_hash));
    return true;
}

/// Parses the shader's declaration table and finds the declaration that names this writer call.
[[nodiscard]] bool MatchDeclaration(VAddr trailer, u8 usage_type, u8 api_slot,
                                    GnmShaderMetadata::UsageSlot& out) noexcept {
    std::array<u8, GnmShaderMetadata::TrailerSize> trailer_bytes{};
    if (!ReadGuest(trailer, trailer_bytes.data(), trailer_bytes.size())) {
        return false;
    }
    const u8 chunk_base = trailer_bytes[GnmShaderMetadata::OffChunkBase];
    const u8 slot_count = trailer_bytes[GnmShaderMetadata::OffSlotCount];
    const size_t table_bytes = (static_cast<size_t>(chunk_base) + slot_count) * 4;
    if (table_bytes > trailer) {
        return false;
    }
    const VAddr table_address = trailer - table_bytes;

    std::vector<u8> span(table_bytes + GnmShaderMetadata::TrailerSize);
    if (!ReadGuest(table_address, span.data(), span.size())) {
        return false;
    }
    const auto parsed = GnmShaderMetadata::Parse(span, table_bytes);
    if (!parsed.valid) {
        return false;
    }
    for (size_t i = 0; i < parsed.slot_count; ++i) {
        if (parsed.slots[i].usage_type == usage_type && parsed.slots[i].api_slot == api_slot) {
            out = parsed.slots[i];
            return true;
        }
    }
    return false;
}

/// The typed-writer observer. Runs at the original writer's entry, before it copies anything.
PS4_SYSV_ABI void WriterObserver(const GuestEntryRegisterFrame* frame, void*) noexcept {
    try {
        if (frame == nullptr) {
            return;
        }
        g_state.writer_calls.fetch_add(1, std::memory_order_relaxed);

        // Writer ABI, from the original prologue: rdi=writer, esi=stage, edx=slot, ecx=count,
        // r8=source. The count is the number of descriptors this call writes.
        const VAddr writer = frame->rdi;
        const u32 stage = static_cast<u32>(frame->rsi);
        const u32 slot = static_cast<u32>(frame->rdx);
        const u32 count = static_cast<u32>(frame->rcx);
        const VAddr source = frame->r8;

        if (writer == 0 || stage > 6 || count == 0) {
            return;
        }

        // The stage's effective map object, exactly as the writer reads it.
        VAddr map_object{};
        if (!ReadGuestValue(writer + WriterStageMapBase + 8 * stage, map_object) ||
            map_object == 0) {
            g_state.no_map_object.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The typed map entry for this slot. 0xFFFF means the shader does not declare it.
        u16 map_word{};
        if (!ReadGuestValue(map_object + MapOffsetConstBuffer + 2 * slot, map_word)) {
            return;
        }
        if (map_word == 0xFFFF) {
            g_state.unmapped_slot.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        BindingRecord record{};
        record.sequence = g_state.next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        record.native_thread_id = CurrentNativeThreadId();
        record.guest_thread_id = CurrentGuestThreadId();
        record.writer = writer;
        record.stage = stage;
        record.api_slot = slot;
        record.count = count;
        record.source = source;
        record.map_object = map_object;
        record.decoded_map_word = map_word;
        record.map_word_present = true;
        record.direct = (map_word & 0x8000) != 0;
        record.mapped_index = map_word & 0x3FFF;

        // Descriptor bytes by value, at the writer boundary, before the writer copies them.
        const u32 descriptor_bytes = std::min<u32>(count * 16, 32);
        if (ReadGuest(source, record.descriptor.data(), descriptor_bytes)) {
            record.descriptor_bytes = descriptor_bytes;
        }

        // Where the writer will put it, when it takes the direct path.
        if (record.direct) {
            ReadGuestValue(writer + WriterPacketCursor, record.packet_cursor);
        }

        // The upload object this descriptor came from, when the deferred-upload caller ran.
        {
            std::scoped_lock lock{g_state.mutex};
            record.upload_object = g_state.last_upload_object;
            record.upload_size = g_state.last_upload_size;
            record.upload_valid = g_state.last_upload_valid;
        }

        // The shader behind this map object.
        VAddr shader_resource{};
        VAddr program_data{};
        VAddr trailer{};
        u64 shader_hash{};
        if (ResolveShader(map_object, shader_resource, program_data, trailer, shader_hash)) {
            record.shader_resource = shader_resource;
            record.shader_resource_plausible = true;
            record.program_data = program_data;
            record.trailer = trailer;
            record.shader_hash = shader_hash;
            record.shader_hash_valid = true;

            GnmShaderMetadata::UsageSlot declaration{};
            if (MatchDeclaration(trailer, 0x02, static_cast<u8>(slot), declaration)) {
                record.declaration_matched = true;
                record.declaration_usage_type = declaration.usage_type;
                record.declaration_api_slot = declaration.api_slot;
                record.declaration_start_register = declaration.start_register;
                record.declaration_flags = declaration.flags;
            }
        } else {
            g_state.no_shader_hash.fetch_add(1, std::memory_order_relaxed);
        }

        std::scoped_lock lock{g_state.mutex};
        if (g_state.records.size() >= MaxRecords) {
            g_state.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_state.records.push_back(record);
    } catch (...) {
        // Observation must never affect guest execution.
    }
}

/// The deferred-upload caller observer. It records the upload object whose contents the provider
/// will copy into the stack buffer the writer then consumes, so the descriptor has a named source.
PS4_SYSV_ABI void UploadCallerObserver(const GuestEntryRegisterFrame* frame, void*) noexcept {
    try {
        if (frame == nullptr) {
            return;
        }
        // Caller ABI: rdi=context. The upload object is at context+0xAE0 for the first slot.
        const VAddr context = frame->rdi;
        if (context == 0) {
            return;
        }
        VAddr upload_object{};
        if (!ReadGuestValue(context + 0xAE0, upload_object) || upload_object == 0) {
            return;
        }
        u8 valid{};
        u32 size{};
        ReadGuestValue(upload_object + UploadObjectValid, valid);
        ReadGuestValue(upload_object + UploadObjectSize, size);

        std::scoped_lock lock{g_state.mutex};
        g_state.last_upload_object = upload_object;
        g_state.last_upload_size = size;
        g_state.last_upload_valid = valid != 0;
        g_state.upload_calls.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
    }
}

/// The grouped-binder observer, so a writer call can be attributed to a binder invocation.
PS4_SYSV_ABI void BinderObserver(const GuestEntryRegisterFrame* frame, void*) noexcept {
    try {
        if (frame == nullptr) {
            return;
        }
        g_state.binder_calls.fetch_add(1, std::memory_order_relaxed);

        // Binder ABI, from the original prologue: rdi=context, rsi=shader_resource.
        const VAddr context = frame->rdi;
        const VAddr shader_resource = frame->rsi;
        if (context == 0 || shader_resource == 0) {
            return;
        }

        // The binder's own gate: pending = context_dirty & resource_group_mask. A zero result
        // means no group is dirty and no writer call follows, which is why not every declaration
        // produces a descriptor write.
        u32 dirty_mask{};
        u32 group_mask{};
        if (!ReadGuestValue(context + BinderDirtyMask, dirty_mask) ||
            !ReadGuestValue(shader_resource + BinderGroupMask, group_mask)) {
            return;
        }
        const u32 pending = dirty_mask & group_mask;
        if (pending == 0) {
            g_state.binder_no_pending.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The lowest set bit selects the group, exactly as the original blsi does.
        const u32 lowest = pending & (~pending + 1);
        u32 group = 0;
        for (u32 bit = lowest; bit > 1; bit >>= 1) {
            ++group;
        }

        // The packed binding list for this group, and the entry it selects.
        VAddr list{};
        u32 list_index{};
        u32 packed_entry{};
        VAddr cached_group{};
        VAddr object_array{};
        if (!ReadGuestValue(shader_resource + BinderResourceList, list) || list == 0) {
            return;
        }
        if (!ReadGuestValue(list + 4 * group, list_index)) {
            return;
        }
        if (!ReadGuestValue(list + 4 * list_index, packed_entry)) {
            return;
        }
        if (!ReadGuestValue(context + BinderGroupCache + 8 * group, cached_group) ||
            cached_group == 0) {
            return;
        }
        ReadGuestValue(cached_group + BinderObjectArray, object_array);

        // Atlas A-031's decode of the packed entry.
        const u8 api_slot = static_cast<u8>(packed_entry & 0xFF);
        const u16 object_index = static_cast<u16>((packed_entry >> 8) & 0xFFFF);
        const u8 entry_group = static_cast<u8>(packed_entry >> 24);

        VAddr selected_object{};
        if (object_array != 0) {
            ReadGuestValue(object_array + 8 * object_index, selected_object);
        }
        u64 object_vptr{};
        if (selected_object != 0) {
            ReadGuestValue(selected_object, object_vptr);
        }

        std::scoped_lock lock{g_state.mutex};
        if (g_state.binder_records.size() >= MaxRecords) {
            return;
        }
        BinderRecord record{};
        record.sequence = g_state.next_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        record.native_thread_id = CurrentNativeThreadId();
        record.guest_thread_id = CurrentGuestThreadId();
        record.context = context;
        record.shader_resource = shader_resource;
        record.dirty_mask = dirty_mask;
        record.group_mask = group_mask;
        record.pending = pending;
        record.group = group;
        record.list = list;
        record.list_index = list_index;
        record.packed_entry = packed_entry;
        record.packed_api_slot = api_slot;
        record.packed_object_index = object_index;
        record.packed_group = entry_group;
        record.cached_group = cached_group;
        record.object_array = object_array;
        record.selected_object = selected_object;
        record.object_vptr = object_vptr;
        g_state.binder_records.push_back(record);
    } catch (...) {
    }
}

} // namespace

size_t RecordCount() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return g_state.records.size();
}

std::span<const BindingRecord> Records() noexcept {
    return std::span<const BindingRecord>{g_state.records.data(), g_state.records.size()};
}

std::span<const BinderRecord> BinderRecords() noexcept {
    return std::span<const BinderRecord>{g_state.binder_records.data(),
                                         g_state.binder_records.size()};
}

void ResetForTesting() noexcept {
    std::scoped_lock lock{g_state.mutex};
    g_state.records.clear();
    g_state.binder_records.clear();
    g_state.dropped.store(0, std::memory_order_relaxed);
    g_state.exported.store(false, std::memory_order_relaxed);
}

void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version) {
    if (!IsOptedIn(std::getenv(EnableEnvironmentVariable.data()))) {
        return;
    }
    g_state.enabled.store(true, std::memory_order_relaxed);

#if !defined(ARCH_X86_64) || defined(SHADPS4_ENABLE_FEX_GUEST_CPU)
    LOG_WARNING(Core_Linker,
                "GNM binding capture refused: this build uses FEX/translated or non-native "
                "execution");
    g_state.enabled.store(false, std::memory_order_relaxed);
    return;
#else
    if (module.memory == nullptr || module.name != ModuleName || title_id != TitleId) {
        LOG_WARNING(Core_Linker,
                    "GNM binding capture refused: expected native CUSA11919 eboot.bin, got "
                    "title='{}' module='{}'",
                    title_id, module.name);
        g_state.enabled.store(false, std::memory_order_relaxed);
        return;
    }
    if (module.base_virtual_addr == 0) {
        LOG_WARNING(Core_Linker, "GNM binding capture refused: invalid module base");
        g_state.enabled.store(false, std::memory_order_relaxed);
        return;
    }

    g_state.memory = module.memory;
    g_state.module_base = module.base_virtual_addr;

    const auto install = [&](u32 rva, std::span<const u8> prologue, GuestEntryObserver observer,
                             GuestEntryObserverHandle& handle, const char* label) {
        const VAddr target = module.base_virtual_addr + rva;
        void* start{};
        void* end{};
        u32 protection{};
        if (module.memory->QueryProtection(target, &start, &end, &protection) != 0 ||
            reinterpret_cast<VAddr>(start) > target ||
            reinterpret_cast<VAddr>(end) < target + prologue.size() ||
            (protection & static_cast<u32>(MemoryProt::CpuRead)) == 0 ||
            (protection & static_cast<u32>(MemoryProt::CpuExec)) == 0) {
            LOG_WARNING(Core_Linker, "GNM binding capture refused: {} {:#x} is not in a readable "
                                     "executable VMA",
                        label, target);
            return false;
        }
        if (std::memcmp(reinterpret_cast<const void*>(target), prologue.data(),
                        prologue.size()) != 0) {
            LOG_WARNING(Core_Linker,
                        "GNM binding capture refused: {} prologue mismatch at {:#x}", label, target);
            return false;
        }
        if (!InstallGuestEntryObserver(*module.memory, target, observer, nullptr, handle)) {
            LOG_WARNING(Core_Linker, "GNM binding capture refused: {} install failed", label);
            return false;
        }
        LOG_INFO(Core_Linker, "GNM binding capture: {} observed at {:#x}", label, target);
        return true;
    };

    const bool writer_ok = install(CbufWriterRva, WriterPrologue, WriterObserver,
                                   g_state.writer_handle, "cbuf16 writer");
    if (!writer_ok) {
        g_state.enabled.store(false, std::memory_order_relaxed);
        return;
    }
    // The binder is optional: the writer chain is the proof, and a binder that fails to install
    // must not take the writer down with it.
    install(GroupedBinderRva, BinderPrologue, BinderObserver, g_state.binder_handle,
            "grouped binder");
    install(DeferredUploadCallerRva, BinderPrologue, UploadCallerObserver,
            g_state.upload_handle, "deferred upload caller");
#endif
}

void Export() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        if (g_state.exported.exchange(true, std::memory_order_relaxed)) {
            return;
        }

        std::scoped_lock lock{g_state.mutex};

        std::string json;
        json.reserve(4096 + g_state.records.size() * 512);
        json += "{\n  \"schema\": \"deracine.gnm_binding_chain/1\",\n";
        json += "  \"records\": [\n";

        for (size_t i = 0; i < g_state.records.size(); ++i) {
            const BindingRecord& r = g_state.records[i];
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer),
                          "    {\"sequence\": %llu, \"native_thread\": %llu, "
                          "\"guest_thread\": %llu, \"writer\": \"0x%llx\", \"stage\": %u, "
                          "\"api_slot\": %u, \"count\": %u, \"source\": \"0x%llx\", "
                          "\"map_object\": \"0x%llx\", \"decoded_map_word\": \"0x%04x\", "
                          "\"direct\": %s, \"mapped_index\": %u, "
                          "\"shader_resource\": \"0x%llx\", \"program_data\": \"0x%llx\", "
                          "\"trailer\": \"0x%llx\", \"shader_hash\": \"%016llx\", "
                          "\"shader_hash_valid\": %s, \"descriptor_bytes\": %u, "
                          "\"packet_cursor\": \"0x%llx\", \"upload_object\": \"0x%llx\", "
                          "\"upload_size\": %u, \"upload_valid\": %s, "
                          "\"declaration_matched\": %s, "
                          "\"declaration\": {\"usage_type\": %u, \"api_slot\": %u, "
                          "\"start_register\": %u, \"flags\": %u}, \"descriptor\": \"",
                          static_cast<unsigned long long>(r.sequence),
                          static_cast<unsigned long long>(r.native_thread_id),
                          static_cast<unsigned long long>(r.guest_thread_id),
                          static_cast<unsigned long long>(r.writer), r.stage, r.api_slot, r.count,
                          static_cast<unsigned long long>(r.source),
                          static_cast<unsigned long long>(r.map_object), r.decoded_map_word,
                          r.direct ? "true" : "false", r.mapped_index,
                          static_cast<unsigned long long>(r.shader_resource),
                          static_cast<unsigned long long>(r.program_data),
                          static_cast<unsigned long long>(r.trailer),
                          static_cast<unsigned long long>(r.shader_hash),
                          r.shader_hash_valid ? "true" : "false", r.descriptor_bytes,
                          static_cast<unsigned long long>(r.packet_cursor),
                          static_cast<unsigned long long>(r.upload_object), r.upload_size,
                          r.upload_valid ? "true" : "false",
                          r.declaration_matched ? "true" : "false", r.declaration_usage_type,
                          r.declaration_api_slot, r.declaration_start_register,
                          r.declaration_flags);
            json += buffer;
            for (u32 b = 0; b < r.descriptor_bytes; ++b) {
                char hex[4];
                std::snprintf(hex, sizeof(hex), "%02x", r.descriptor[b]);
                json += hex;
            }
            json += "\"}";
            json += i + 1 == g_state.records.size() ? "\n" : ",\n";
        }

        json += "  ],\n";
        char summary[512];
        std::snprintf(summary, sizeof(summary),
                      "  \"summary\": {\"records\": %zu, \"dropped\": %u, "
                      "\"writer_calls\": %u, \"binder_calls\": %u, \"unmapped_slot\": %u, "
                      "\"no_map_object\": %u, \"no_shader_hash\": %u, "
                      "\"diag_program_read_failed\": %u, \"diag_program_null\": %u, "
                      "\"diag_no_token\": %u, \"diag_no_signature\": %u, "
                      "\"diag_last_head0\": \"0x%08x\", \"diag_last_code\": \"0x%llx\", "
                      "\"diag_last_size\": %u, \"binder_no_pending\": %u},\n",
                      g_state.records.size(), g_state.dropped.load(std::memory_order_relaxed),
                      g_state.writer_calls.load(std::memory_order_relaxed),
                      g_state.binder_calls.load(std::memory_order_relaxed),
                      g_state.unmapped_slot.load(std::memory_order_relaxed),
                      g_state.no_map_object.load(std::memory_order_relaxed),
                      g_state.no_shader_hash.load(std::memory_order_relaxed),
                      g_state.diag_program_read_failed.load(std::memory_order_relaxed),
                      g_state.diag_program_null.load(std::memory_order_relaxed),
                      g_state.diag_no_token.load(std::memory_order_relaxed),
                      g_state.diag_no_signature.load(std::memory_order_relaxed),
                      g_state.diag_last_head0.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(g_state.diag_last_code.load()),
                      g_state.diag_last_size.load(),
                      g_state.binder_no_pending.load(std::memory_order_relaxed));
        json += summary;
        json += "  \"binder_records\": [\n";
        for (size_t i = 0; i < g_state.binder_records.size(); ++i) {
            const BinderRecord& b = g_state.binder_records[i];
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer),
                          "    {\"sequence\": %llu, \"native_thread\": %llu, "
                          "\"guest_thread\": %llu, \"context\": \"0x%llx\", "
                          "\"shader_resource\": \"0x%llx\", \"dirty_mask\": \"0x%08x\", "
                          "\"group_mask\": \"0x%08x\", \"pending\": \"0x%08x\", "
                          "\"group\": %u, \"list\": \"0x%llx\", \"list_index\": %u, "
                          "\"packed_entry\": \"0x%08x\", \"packed_api_slot\": %u, "
                          "\"packed_object_index\": %u, \"packed_group\": %u, "
                          "\"cached_group\": \"0x%llx\", \"object_array\": \"0x%llx\", "
                          "\"selected_object\": \"0x%llx\", \"object_vptr\": \"0x%llx\"}",
                          static_cast<unsigned long long>(b.sequence),
                          static_cast<unsigned long long>(b.native_thread_id),
                          static_cast<unsigned long long>(b.guest_thread_id),
                          static_cast<unsigned long long>(b.context),
                          static_cast<unsigned long long>(b.shader_resource), b.dirty_mask,
                          b.group_mask, b.pending, b.group,
                          static_cast<unsigned long long>(b.list), b.list_index, b.packed_entry,
                          b.packed_api_slot, b.packed_object_index, b.packed_group,
                          static_cast<unsigned long long>(b.cached_group),
                          static_cast<unsigned long long>(b.object_array),
                          static_cast<unsigned long long>(b.selected_object),
                          static_cast<unsigned long long>(b.object_vptr));
            json += buffer;
            json += i + 1 == g_state.binder_records.size() ? "\n" : ",\n";
        }
        json += "  ],\n";
        json += "  \"diag_dumps\": [\n";
        for (size_t i = 0; i < g_state.diag_dumps.size(); ++i) {
            const auto& d = g_state.diag_dumps[i];
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer),
                          "    {\"program_data\": \"0x%llx\", \"shader_resource\": \"0x%llx\", "
                          "\"words\": [",
                          static_cast<unsigned long long>(d.program_data),
                          static_cast<unsigned long long>(d.shader_resource));
            json += buffer;
            for (int w = 0; w < 16; ++w) {
                std::snprintf(buffer, sizeof(buffer), "%s\"0x%08x\"", w == 0 ? "" : ", ",
                              d.words[w]);
                json += buffer;
            }
            json += "]}";
            json += i + 1 == g_state.diag_dumps.size() ? "\n" : ",\n";
        }
        json += "  ],\n";
        json += "  \"notes\": [\n";
        json += "    \"Descriptor bytes are copied by value at the original writer boundary.\",\n";
        json += "    \"shader_hash is derived from the map object's owning shader resource.\",\n";
        json += "    \"No PM4 or Vulkan descriptor is used as original guest semantics.\"\n";
        json += "  ]\n}\n";

        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "gnm_binding";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "gnm_binding_chain.json";
        if (FILE* file = std::fopen(path.string().c_str(), "wb"); file != nullptr) {
            std::fwrite(json.data(), 1, json.size(), file);
            std::fclose(file);
            LOG_INFO(Core_Linker,
                     "DERACINE_GNM_BINDING_SUMMARY records={} writer_calls={} binder_calls={} "
                     "unmapped={} no_map={} no_hash={} path={}",
                     g_state.records.size(), g_state.writer_calls.load(),
                     g_state.binder_calls.load(), g_state.unmapped_slot.load(),
                     g_state.no_map_object.load(), g_state.no_shader_hash.load(), path.string());
        } else {
            LOG_ERROR(Core_Linker, "DERACINE_GNM_BINDING: could not write {}", path.string());
        }
    } catch (...) {
    }
}

} // namespace Core::GnmBindingCapture
