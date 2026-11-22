// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "common/types.h"

namespace Core::GnmShaderMetadata {

/// The trailing GnmShaderBinaryInfo ("OrbShdr") structure is 28 bytes.
inline constexpr size_t TrailerSize = 28;

/// Bounded bytes of the private raw region kept per shader, for the offline parser.
inline constexpr size_t MaxRawRegionBytes = 64 * 1024;

/// Bounded number of four-byte declarations recorded per shader. The GNM usage table is small; a
/// shader declaring more is reported as truncated rather than silently cut.
inline constexpr size_t MaxSlots = 64;

/// The seven-byte signature that opens the trailer.
inline constexpr std::array<u8, 7> OrbShdrSignature = {0x4f, 0x72, 0x62, 0x53, 0x68, 0x64, 0x72};

/// Trailer field offsets, from the published GnmShaderBinaryInfo layout and confirmed against the
/// original decoder's own reads (Atlas A-027).
inline constexpr size_t OffVersion = 7;
inline constexpr size_t OffPacked = 8;
inline constexpr size_t OffChunkBase = 12;
inline constexpr size_t OffSlotCount = 13;
inline constexpr size_t OffFlags = 14;
inline constexpr size_t OffHash = 16;
inline constexpr size_t OffCrc32 = 24;

/// One four-byte GnmInputUsageSlot, in the original byte order.
struct UsageSlot {
    u8 usage_type{};
    u8 api_slot{};
    u8 start_register{};
    u8 flags{};
};

/// The public PS4 GNM name for a usage type, or nullptr when the type has no public name. Never
/// invents a name: an unknown type is reported as unknown.
[[nodiscard]] constexpr const char* UsageTypeName(u8 usage_type) noexcept {
    switch (usage_type) {
    case 0x00:
        return "IMM_RESOURCE";
    case 0x01:
        return "IMM_SAMPLER";
    case 0x02:
        return "IMM_CONSTBUFFER";
    case 0x03:
        return "IMM_VERTEXBUFFER";
    case 0x04:
        return "IMM_RWRESOURCE";
    case 0x05:
        return "IMM_ALUFLOATCONST";
    case 0x06:
        return "IMM_ALUBOOL32CONST";
    case 0x07:
        return "IMM_GDSCOUNTERRANGE";
    case 0x08:
        return "IMM_GDSMEMORYRANGE";
    case 0x09:
        return "IMM_GWSBASE";
    case 0x0A:
        return "IMM_SRT";
    case 0x0D:
        return "IMM_LDSESGSSIZE";
    case 0x12:
        return "SUBPTR_FETCHSHADER";
    case 0x13:
        return "PTR_RESOURCETABLE";
    case 0x14:
        return "PTR_INTERNALRESOURCETABLE";
    case 0x15:
        return "PTR_SAMPLERTABLE";
    case 0x16:
        return "PTR_CONSTBUFFERTABLE";
    case 0x17:
        return "PTR_VERTEXBUFFERTABLE";
    case 0x18:
        return "PTR_SOBUFFERTABLE";
    case 0x19:
        return "PTR_RWRESOURCETABLE";
    case 0x1A:
        return "PTR_INTERNALGLOBALTABLE";
    case 0x1B:
        return "PTR_EXTENDEDUSERDATA";
    case 0x1C:
        return "PTR_INDIRECTRESOURCETABLE";
    case 0x1D:
        return "PTR_INDIRECTINTERNALRESOURCETABLE";
    case 0x1E:
        return "PTR_INDIRECTRWRESOURCETABLE";
    case 0x22:
        return "IMM_GDS_KICK_RING_OFFSET";
    case 0x23:
        return "IMM_VERTEX_RING_OFFSET";
    case 0x24:
        return "PTR_DISPATCH_DRAW";
    case 0x25:
        return "IMM_DISPATCH_DRAW_INSTANCES";
    default:
        return nullptr;
    }
}

/// Whether the usage type is one of the DispatchDraw declarations 0x22..0x25.
[[nodiscard]] constexpr bool IsDispatchDrawUsage(u8 usage_type) noexcept {
    return usage_type >= 0x22 && usage_type <= 0x25;
}

/// A parsed trailer plus its declaration table.
struct ParseResult {
    bool valid{};
    /// Static description of why parsing failed. Null when valid.
    const char* error{};

    u8 version{};
    u32 length{};
    u8 shader_type{};
    u8 chunk_usage_base_offset_in_dw{};
    u8 num_input_usage_slots{};
    u8 flags{};
    bool is_srt{};
    bool is_extended_usage_info{};
    u64 shader_hash{};
    u32 crc32{};

    /// Offset of the declaration table within the supplied span, and its byte length.
    size_t table_offset{};
    size_t table_bytes{};

    std::array<UsageSlot, MaxSlots> slots{};
    u8 slot_count{};
    /// Declarations present in the table but beyond MaxSlots.
    u8 truncated_slots{};
};

namespace Detail {

[[nodiscard]] inline u32 ReadU32(const u8* p) noexcept {
    u32 v{};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] inline u64 ReadU64(const u8* p) noexcept {
    u64 v{};
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace Detail

/// Parses the trailer at `trailer_offset` and the declaration table that precedes it.
///
/// The table is located at `trailer_offset - 4 * (chunk_usage_base_offset_in_dw +
/// num_input_usage_slots)`, which is the original decoder's own arithmetic (Atlas A-027). A span
/// that does not contain the whole table is rejected rather than partially parsed.
///
/// Pure: reads only the supplied span, so it can be exercised without an emulator, a GPU or a
/// guest memory manager.
[[nodiscard]] inline ParseResult Parse(std::span<const u8> blob, size_t trailer_offset) noexcept {
    ParseResult result{};

    if (trailer_offset > blob.size() || blob.size() - trailer_offset < TrailerSize) {
        result.error = "trailer outside the supplied span";
        return result;
    }
    const u8* trailer = blob.data() + trailer_offset;
    if (std::memcmp(trailer, OrbShdrSignature.data(), OrbShdrSignature.size()) != 0) {
        result.error = "OrbShdr signature absent";
        return result;
    }

    result.version = trailer[OffVersion];
    const u32 packed = Detail::ReadU32(trailer + OffPacked);
    result.length = packed >> 8;
    result.shader_type = static_cast<u8>((packed >> 2) & 0xf);
    result.chunk_usage_base_offset_in_dw = trailer[OffChunkBase];
    result.num_input_usage_slots = trailer[OffSlotCount];
    result.flags = trailer[OffFlags];
    result.is_srt = (result.flags & 1) != 0;
    result.is_extended_usage_info = (result.flags & 4) != 0;
    result.shader_hash = Detail::ReadU64(trailer + OffHash);
    result.crc32 = Detail::ReadU32(trailer + OffCrc32);

    // The original decoder's own arithmetic: the table sits before the trailer, and its length is
    // the chunk base plus the slot count, in DWORDs. Fail closed rather than partially parse.
    const size_t table_dwords =
        static_cast<size_t>(result.chunk_usage_base_offset_in_dw) + result.num_input_usage_slots;
    const size_t table_bytes = table_dwords * 4;
    if (table_bytes > trailer_offset) {
        result.error = "declaration table precedes the supplied span";
        return result;
    }
    result.table_offset = trailer_offset - table_bytes;
    result.table_bytes = table_bytes;

    const u8* table = blob.data() + result.table_offset;
    const size_t count = result.num_input_usage_slots;
    result.slot_count = static_cast<u8>(std::min<size_t>(count, MaxSlots));
    result.truncated_slots = static_cast<u8>(count - result.slot_count);
    for (size_t i = 0; i < result.slot_count; ++i) {
        const u8* rec = table + i * 4;
        result.slots[i] = UsageSlot{rec[0], rec[1], rec[2], rec[3]};
    }

    result.valid = true;
    return result;
}

} // namespace Core::GnmShaderMetadata
