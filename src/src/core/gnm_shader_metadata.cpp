// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/gnm_shader_metadata.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/memory.h"

namespace Core::GnmShaderMetadata {

namespace {

struct CollectorState {
    std::atomic<bool> enabled{};
    std::atomic<bool> raw_enabled{};
    std::atomic<bool> exported{};
    std::atomic<u32> dropped{};
    std::mutex mutex;
    std::vector<Record> records;
    /// Raw trailer-and-table bytes per record, parallel to records. Private: never committed.
    std::vector<std::vector<u8>> raw_regions;
};

CollectorState g_state{};

[[nodiscard]] bool ReadFlag(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

/// Reads guest memory only after the emulator's own mapping table says the whole range is mapped.
/// Returns false rather than reading a partially mapped range.
[[nodiscard]] bool ReadGuest(VAddr address, void* out, size_t size) noexcept {
    if (size == 0) {
        return true;
    }
    auto* memory = Memory::Instance();
    if (memory == nullptr || !memory->IsValidMapping(address, size)) {
        return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}

[[nodiscard]] const char* StageName(u32 stage) noexcept {
    switch (stage) {
    case 0:
        return "PS";
    case 1:
        return "VS";
    case 2:
        return "GS";
    case 3:
        return "ES";
    case 4:
        return "HS";
    case 5:
        return "LS";
    case 6:
        return "CS";
    default:
        return "UNKNOWN";
    }
}

void AppendQuoted(std::string& out, const char* text) {
    out += '"';
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
        }
        out += *p;
    }
    out += '"';
}

void AppendUsageName(std::string& out, u8 usage_type) {
    if (const char* name = UsageTypeName(usage_type); name != nullptr) {
        AppendQuoted(out, name);
        return;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "UNKNOWN_0x%02X", usage_type);
    AppendQuoted(out, buffer);
}

} // namespace

bool Enabled() noexcept {
    static const bool enabled = [] {
        g_state.enabled.store(ReadFlag(EnableEnvironmentVariable.data()),
                              std::memory_order_relaxed);
        g_state.raw_enabled.store(ReadFlag(RawEnvironmentVariable.data()),
                                  std::memory_order_relaxed);
        return g_state.enabled.load(std::memory_order_relaxed);
    }();
    return enabled;
}

void Observe(u32 stage, VAddr code_address, VAddr trailer_address) noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        if (code_address == 0 || trailer_address == 0 || trailer_address < code_address) {
            return;
        }

        // Read the trailer first: its own fields decide how much of the table to read, so the
        // table range is validated against the emulator's mapping table before it is touched.
        std::array<u8, TrailerSize> trailer_bytes{};
        if (!ReadGuest(trailer_address, trailer_bytes.data(), trailer_bytes.size())) {
            return;
        }
        if (std::memcmp(trailer_bytes.data(), OrbShdrSignature.data(), OrbShdrSignature.size()) !=
            0) {
            return;
        }

        const u8 chunk_base = trailer_bytes[OffChunkBase];
        const u8 slot_count = trailer_bytes[OffSlotCount];
        const size_t table_bytes = (static_cast<size_t>(chunk_base) + slot_count) * 4;
        if (table_bytes > trailer_address) {
            return;
        }
        const VAddr table_address = trailer_address - table_bytes;

        // One contiguous span covering the table and the trailer, so a single mapping check covers
        // everything the parser will read.
        const size_t span_bytes = table_bytes + TrailerSize;
        std::vector<u8> span(span_bytes);
        if (!ReadGuest(table_address, span.data(), span_bytes)) {
            return;
        }

        const ParseResult parsed = Parse(span, table_bytes);
        if (!parsed.valid) {
            return;
        }

        std::scoped_lock lock{g_state.mutex};
        // Once per distinct (hash, stage): a shader recompiled for a permutation is the same
        // metadata, and the title recompiles the same stage many times.
        for (const auto& existing : g_state.records) {
            if (existing.shader_hash == parsed.shader_hash && existing.stage == stage) {
                return;
            }
        }
        if (g_state.records.size() >= MaxShaders) {
            g_state.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        Record record{};
        record.shader_hash = parsed.shader_hash;
        record.stage = stage;
        record.code_address = code_address;
        record.trailer_address = trailer_address;
        record.trailer_offset_in_code = trailer_address - code_address;
        record.bininfo_length = parsed.length;
        record.program_size = record.trailer_offset_in_code + TrailerSize;
        record.trailer_within_bininfo_length = record.program_size <= parsed.length;
        record.parsed = parsed;

        g_state.records.push_back(record);
        if (g_state.raw_enabled.load(std::memory_order_relaxed)) {
            // The private raw region covers the whole program from its first instruction to the end
            // of the trailer, so the layout between the compiled span and the trailer is visible to
            // the offline parser. Bounded, and never committed.
            const size_t full_bytes = record.trailer_offset_in_code + TrailerSize;
            if (full_bytes <= MaxRawRegionBytes) {
                std::vector<u8> full(full_bytes);
                if (ReadGuest(code_address, full.data(), full_bytes)) {
                    g_state.raw_regions.push_back(std::move(full));
                } else {
                    g_state.raw_regions.push_back(std::move(span));
                }
            } else {
                g_state.raw_regions.push_back(std::move(span));
            }
        }
    } catch (...) {
        // Observation must never affect shader compilation.
    }
}

size_t RecordCount() noexcept {
    std::scoped_lock lock{g_state.mutex};
    return g_state.records.size();
}

std::span<const Record> Records() noexcept {
    // The export path is the only concurrent reader and it runs at shutdown.
    return std::span<const Record>{g_state.records.data(), g_state.records.size()};
}

void ResetForTesting() noexcept {
    std::scoped_lock lock{g_state.mutex};
    g_state.records.clear();
    g_state.raw_regions.clear();
    g_state.dropped.store(0, std::memory_order_relaxed);
    g_state.exported.store(false, std::memory_order_relaxed);
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

        std::array<u32, 256> usage_histogram{};
        std::array<u32, 256> vs_histogram{};
        std::array<u32, 256> ps_histogram{};
        u32 total_declarations = 0;
        u32 dispatch_draw_declarations = 0;
        u32 trailers_outside_code_span = 0;
        u32 srt_shaders = 0;
        u32 extended_usage_shaders = 0;
        u32 truncated_shaders = 0;

        std::string json;
        json.reserve(4096 + g_state.records.size() * 512);
        json += "{\n  \"schema\": \"deracine.gnm_shader_metadata/1\",\n";
        json += "  \"shaders\": [\n";

        for (size_t i = 0; i < g_state.records.size(); ++i) {
            const Record& record = g_state.records[i];
            const ParseResult& parsed = record.parsed;

            if (!record.trailer_within_bininfo_length) {
                ++trailers_outside_code_span;
            }
            if (parsed.is_srt) {
                ++srt_shaders;
            }
            if (parsed.is_extended_usage_info) {
                ++extended_usage_shaders;
            }
            if (parsed.truncated_slots != 0) {
                ++truncated_shaders;
            }

            // Sized for the whole record header with room to spare: a truncated snprintf here
            // would emit invalid JSON rather than fail loudly.
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer),
                          "    {\"shader_hash\": \"%016llx\", \"stage\": \"%s\", "
                          "\"stage_index\": %u, \"code_address\": \"0x%llx\", "
                          "\"trailer_address\": \"0x%llx\", \"trailer_offset_in_code\": %llu, "
                          "\"bininfo_length\": %u, \"program_size\": %llu, "
                          "\"trailer_within_bininfo_length\": %s, "
                          "\"version\": %u, \"shader_type\": %u, "
                          "\"chunk_usage_base_offset_in_dw\": %u, "
                          "\"num_input_usage_slots\": %u, \"flags\": %u, \"is_srt\": %s, "
                          "\"is_extended_usage_info\": %s, \"crc32\": \"0x%08x\", "
                          "\"table_offset_in_span\": %zu, \"table_bytes\": %zu, "
                          "\"truncated_slots\": %u, \"usage_slots\": [",
                          static_cast<unsigned long long>(record.shader_hash),
                          StageName(record.stage), record.stage,
                          static_cast<unsigned long long>(record.code_address),
                          static_cast<unsigned long long>(record.trailer_address),
                          static_cast<unsigned long long>(record.trailer_offset_in_code),
                          record.bininfo_length,
                          static_cast<unsigned long long>(record.program_size),
                          record.trailer_within_bininfo_length ? "true" : "false", parsed.version,
                          parsed.shader_type, parsed.chunk_usage_base_offset_in_dw,
                          parsed.num_input_usage_slots, parsed.flags,
                          parsed.is_srt ? "true" : "false",
                          parsed.is_extended_usage_info ? "true" : "false", parsed.crc32,
                          parsed.table_offset, parsed.table_bytes, parsed.truncated_slots);
            json += buffer;

            for (size_t s = 0; s < parsed.slot_count; ++s) {
                const UsageSlot& slot = parsed.slots[s];
                ++usage_histogram[slot.usage_type];
                if (record.stage == 1) {
                    ++vs_histogram[slot.usage_type];
                } else if (record.stage == 0) {
                    ++ps_histogram[slot.usage_type];
                }
                ++total_declarations;
                if (IsDispatchDrawUsage(slot.usage_type)) {
                    ++dispatch_draw_declarations;
                }

                json += s == 0 ? "\n" : ",\n";
                std::snprintf(buffer, sizeof(buffer),
                              "      {\"usage_type\": %u, \"name\": ", slot.usage_type);
                json += buffer;
                AppendUsageName(json, slot.usage_type);
                std::snprintf(buffer, sizeof(buffer),
                              ", \"api_slot\": %u, \"start_register\": %u, \"flags\": %u}",
                              slot.api_slot, slot.start_register, slot.flags);
                json += buffer;
            }
            json += parsed.slot_count == 0 ? "]}" : "\n    ]}";
            json += i + 1 == g_state.records.size() ? "\n" : ",\n";
        }

        json += "  ],\n";

        char summary[1024];
        std::snprintf(summary, sizeof(summary),
                      "  \"summary\": {\"shaders\": %zu, \"dropped\": %u, "
                      "\"declarations\": %u, \"dispatch_draw_declarations\": %u, "
                      "\"trailers_outside_code_span\": %u, \"srt_shaders\": %u, "
                      "\"extended_usage_shaders\": %u, \"truncated_shaders\": %u},\n",
                      g_state.records.size(), g_state.dropped.load(std::memory_order_relaxed),
                      total_declarations, dispatch_draw_declarations, trailers_outside_code_span,
                      srt_shaders, extended_usage_shaders, truncated_shaders);
        json += summary;

        const auto append_histogram = [&](const char* label, const std::array<u32, 256>& histogram,
                                          bool trailing_comma) {
            json += "  \"";
            json += label;
            json += "\": {";
            bool first = true;
            for (u32 type = 0; type < histogram.size(); ++type) {
                if (histogram[type] == 0) {
                    continue;
                }
                if (!first) {
                    json += ", ";
                }
                AppendUsageName(json, static_cast<u8>(type));
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), ": %u", histogram[type]);
                json += buffer;
                first = false;
            }
            json += trailing_comma ? "},\n" : "}\n";
        };
        append_histogram("usage_type_histogram", usage_histogram, true);
        append_histogram("usage_type_histogram_vs", vs_histogram, true);
        append_histogram("usage_type_histogram_ps", ps_histogram, true);

        json += "  \"notes\": [\n";
        json += "    \"Metadata only. No shader code, no guest memory dump, no GPU readback.\",\n";
        json += "    \"bininfo_length is the trailer's own 24-bit field, not the compiled span.\",\n";
        json += "    \"usage_type names are PS4 GNM; UNKNOWN_0xNN is preserved, never guessed.\"\n";
        json += "  ]\n}\n";

        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "gnm_shader";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "gnm_shader_metadata.json";
        if (FILE* file = std::fopen(path.string().c_str(), "wb"); file != nullptr) {
            std::fwrite(json.data(), 1, json.size(), file);
            std::fclose(file);
            LOG_INFO(Core,
                     "DERACINE_GNM_SHADER_SUMMARY shaders={} declarations={} dispatch_draw={} "
                     "outside_span={} path={}",
                     g_state.records.size(), total_declarations, dispatch_draw_declarations,
                     trailers_outside_code_span, path.string());
        } else {
            LOG_ERROR(Core, "DERACINE_GNM_SHADER: could not write {}", path.string());
        }

        // The raw region holds original shader bytes. It is written only when explicitly asked
        // for, to a separate file, and it must never be committed.
        if (g_state.raw_enabled.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < g_state.records.size() && i < g_state.raw_regions.size(); ++i) {
                const Record& record = g_state.records[i];
                const auto& region = g_state.raw_regions[i];
                char name[128];
                std::snprintf(name, sizeof(name), "%016llx_%s.bin",
                              static_cast<unsigned long long>(record.shader_hash),
                              StageName(record.stage));
                const auto raw_path = dir / "raw" / name;
                std::filesystem::create_directories(raw_path.parent_path(), ec);
                if (FILE* file = std::fopen(raw_path.string().c_str(), "wb"); file != nullptr) {
                    std::fwrite(region.data(), 1, region.size(), file);
                    std::fclose(file);
                }
            }
            LOG_INFO(Core, "DERACINE_GNM_SHADER: wrote {} private raw regions to {}",
                     g_state.raw_regions.size(), (dir / "raw").string());
        }
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::GnmShaderMetadata
