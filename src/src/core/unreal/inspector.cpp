// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/inspector.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace Core::Unreal {
namespace {


constexpr u32 NameBlockBits = 16;
constexpr u32 NamePoolBlocks = 8192;
constexpr u32 NamesPerChunk = 16384;

// {objects, max, num, max_chunks, num_chunks} layouts the chunked object array has been seen with.
constexpr std::array<std::array<u64, 5>, 5> ChunkedLayouts{{
    {0x00, 0x10, 0x14, 0x18, 0x1C},
    {0x00, 0x0C, 0x08, 0x14, 0x10},
    {0x10, 0x00, 0x04, 0x08, 0x0C},
    {0x18, 0x10, 0x00, 0x14, 0x20},
    {0x18, 0x00, 0x14, 0x10, 0x04},
}};

std::string ToLower(std::string_view text) {
    std::string out{text};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Utf16ToUtf8(const std::u16string& text) {
    std::string out;
    for (const char16_t unit : text) {
        if (unit < 0x80) {
            out.push_back(static_cast<char>(unit));
        } else if (unit < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
            out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
            out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
        }
    }
    return out;
}

} // Anonymous namespace

class Inspector::Impl {
public:
    Impl(const GuestMemory& memory, ScanRanges ranges, DiscoveryOptions options)
        : memory{memory}, ranges{std::move(ranges.all)}, writable_ranges{std::move(ranges.writable)},
          options{options} {
        const auto by_address = [](const AddressRange& a, const AddressRange& b) {
            return a.start < b.start;
        };
        std::ranges::sort(this->ranges, by_address);
        std::ranges::sort(this->writable_ranges, by_address);
    }

    // -- generic access ---------------------------------------------------------------------

    bool InRange(VAddr address, u64 size = 1) const {
        for (const auto& range : ranges) {
            if (address >= range.start && address + size <= range.end) {
                return true;
            }
        }
        return false;
    }

    bool ReadU64(VAddr address, u64& out) const {
        return InRange(address, sizeof(u64)) && memory.Read(address, out);
    }

    bool ReadU32(VAddr address, u32& out) const {
        return InRange(address, sizeof(u32)) && memory.Read(address, out);
    }

    bool ReadS32(VAddr address, s32& out) const {
        return InRange(address, sizeof(s32)) && memory.Read(address, out);
    }

    bool ReadRaw(VAddr address, void* out, u64 size) const {
        return InRange(address, size) && memory.Read(address, out, size);
    }

    // -- name table ------------------------------------------------------------------------

    enum class NameKind { None, Pool, Chunked };

    struct NameTable {
        NameKind kind = NameKind::None;
        VAddr address = 0;
        u32 current_block = 0;
        u32 cursor = 0;
        u32 chunks = 0;
        u32 count = 0;
    } names;

    bool ResolveName(u32 index, std::string& out, u32 depth = 0) const {
        out.clear();
        if (depth > options.max_outline_depth) {
            return false;
        }
        if (names.kind == NameKind::Pool) {
            return ResolvePoolName(index, out, depth);
        }
        if (names.kind == NameKind::Chunked) {
            return ResolveChunkedName(index, out);
        }
        return false;
    }

    bool ResolvePoolName(u32 index, std::string& out, u32 depth) const {
        const u32 block = index >> NameBlockBits;
        if (block > names.current_block || block >= NamePoolBlocks) {
            return false;
        }
        u64 block_address = 0;
        if (!ReadU64(names.address + 0x10 + block * 8, block_address) || block_address == 0) {
            return false;
        }
        const VAddr entry = block_address + static_cast<u64>(index & 0xFFFF) * 2;
        u16 header = 0;
        if (!ReadRaw(entry, &header, sizeof(header))) {
            return false;
        }
        const u32 length = header >> 6;
        const bool wide = (header & 1) != 0;
        if (length == 0) {
            // Outline entry: {header, u32 index, u32 number}.
            u32 referred = 0;
            u32 number = 0;
            if (!ReadRaw(entry + 2, &referred, sizeof(referred)) ||
                !ReadRaw(entry + 6, &number, sizeof(number))) {
                return false;
            }
            if (!ResolveName(referred, out, depth + 1)) {
                return false;
            }
            if (number > 0) {
                out += "_" + std::to_string(number - 1);
            }
            return true;
        }
        if (length > options.max_name_length) {
            return false;
        }
        if (wide) {
            std::u16string text(length, u'\0');
            if (!ReadRaw(entry + 2, text.data(), length * 2)) {
                return false;
            }
            out = Utf16ToUtf8(text);
        } else {
            std::string text(length, '\0');
            if (!ReadRaw(entry + 2, text.data(), length)) {
                return false;
            }
            out = std::move(text);
        }
        return true;
    }

    bool ResolveChunkedName(u32 index, std::string& out) const {
        if (index >= names.count) {
            return false;
        }
        u64 chunk = 0;
        if (!ReadU64(names.address + 8 * (index / NamesPerChunk), chunk) || chunk == 0) {
            return false;
        }
        u64 entry = 0;
        if (!ReadU64(chunk + 8 * (index % NamesPerChunk), entry) || entry == 0) {
            return false;
        }
        u32 header = 0;
        if (!ReadU32(entry, header)) {
            return false;
        }
        const bool wide = (header & 1) != 0;
        if (wide) {
            std::u16string text(options.max_name_length, u'\0');
            if (!ReadRaw(entry + 0x10, text.data(), text.size() * 2)) {
                return false;
            }
            if (const auto end = text.find(u'\0'); end != std::u16string::npos) {
                text.resize(end);
            }
            out = Utf16ToUtf8(text);
        } else {
            std::string text(options.max_name_length, '\0');
            if (!ReadRaw(entry + 0x10, text.data(), text.size())) {
                return false;
            }
            if (const auto end = text.find('\0'); end != std::string::npos) {
                text.resize(end);
            }
            out = std::move(text);
        }
        return true;
    }

    bool ValidatePool(VAddr address) {
        u32 current_block = 0;
        u32 cursor = 0;
        if (!ReadU32(address + 8, current_block) || !ReadU32(address + 12, cursor)) {
            return false;
        }
        if (current_block >= NamePoolBlocks - 1 || cursor == 0 || cursor > 0x40000) {
            return false;
        }
        for (u32 block = 0; block <= current_block; ++block) {
            u64 block_address = 0;
            if (!ReadU64(address + 0x10 + block * 8, block_address) ||
                !InRange(block_address, 2)) {
                return false;
            }
        }
        u64 unused = 0;
        if (!ReadU64(address + 0x10 + (current_block + 1) * 8, unused) || unused != 0) {
            return false;
        }
        names.kind = NameKind::Pool;
        names.address = address;
        names.current_block = current_block;
        names.cursor = cursor;
        return true;
    }

    // FNamePool block 0 starts with "None" then "ByteProperty", each with its length in the top
    // bits of a 16-bit header and a hash in the low six.
    static bool MatchesNamePrologue(const u8* data, u64 size) {
        constexpr char None[] = "None";
        constexpr char ByteProperty[] = "ByteProperty";
        constexpr u64 NoneLength = sizeof(None) - 1;
        constexpr u64 BytePropertyLength = sizeof(ByteProperty) - 1;
        constexpr u64 Total = 2 + NoneLength + 2 + BytePropertyLength;
        if (size < Total) {
            return false;
        }
        if (data[0] > 0x3F || data[1] != 0x01) {
            return false;
        }
        if (std::memcmp(data + 2, None, NoneLength) != 0) {
            return false;
        }
        if (data[2 + NoneLength] > 0x3F || data[2 + NoneLength + 1] != 0x03) {
            return false;
        }
        return std::memcmp(data + 2 + NoneLength + 2, ByteProperty, BytePropertyLength) == 0;
    }

    template <typename Visit>
    void ForEachWindow(const AddressRange& range, u64 window, Visit&& visit) const {
        for (u64 offset = 0; offset < range.end - range.start; offset += window) {
            const u64 size = std::min(window, range.end - range.start - offset);
            if (size < 8) {
                return;
            }
            scratch.resize(size);
            if (!memory.Read(range.start + offset, scratch.data(), size)) {
                continue;
            }
            if (!visit(range.start + offset, scratch.data(), size)) {
                return;
            }
        }
    }

    bool FindNamePool() {
        for (const auto& range : ranges) {
            if (range.end <= range.start) {
                continue;
            }
            bool stop = false;
            ForEachWindow(range, 4_MB,
                          [&](VAddr base, const u8* data, u64 size) {
                              for (u64 p = 0; p + 2 + 4 + 2 + 12 <= size;) {
                                  const u8* found = static_cast<const u8*>(std::memchr(
                                      data + p, 0x01, static_cast<size_t>(size - p)));
                                  if (found == nullptr) {
                                      break;
                                  }
                                  const u64 at = static_cast<u64>(found - data);
                                  if (at == 0 || at + 2 + 4 + 2 + 12 > size ||
                                      !MatchesNamePrologue(data + at - 1, size - (at - 1))) {
                                      p = at + 1;
                                      continue;
                                  }
                                  const VAddr prologue = base + at - 1;
                                  if (FindPoolFromPrologue(prologue)) {
                                      stop = true;
                                      return false;
                                  }
                                  p = at + 1;
                              }
                              return !stop;
                          });
            if (names.kind != NameKind::None) {
                return true;
            }
        }
        return false;
    }

    // The prologue is referenced by the block pointer table, at pool + 0x10 for block 0.
    bool FindPoolFromPrologue(VAddr prologue) {
        std::array<u8, 8> key{};
        std::memcpy(key.data(), &prologue, sizeof(prologue));
        bool found = false;
        for (const auto& range : ranges) {
            if (found || range.end <= range.start) {
                continue;
            }
            ForEachWindow(range, 4_MB,
                          [&](VAddr base, const u8* data, u64 size) {
                              u64 offset = 0;
                              while (offset + sizeof(key) <= size) {
                                  const void* hit = std::memchr(data + offset, key[0],
                                                                static_cast<size_t>(size - offset));
                                  if (hit == nullptr) {
                                      return true;
                                  }
                                  const auto at = static_cast<u64>(static_cast<const u8*>(hit) -
                                                                   data);
                                  if (at + sizeof(key) <= size &&
                                      std::memcmp(data + at, key.data(), key.size()) == 0) {
                                      const VAddr where = base + at;
                                      if (where >= 0x10 && ValidatePool(where - 0x10)) {
                                          found = true;
                                          return false;
                                      }
                                  }
                                  offset = at + 1;
                              }
                              return true;
                          });
        }
        return found;
    }

    bool FindChunkedNameArray() {
        // Entry 0 is "None": {u32 index, pad, u64 hash next, chars}. The array holds chunk pointers
        // and its counters sit at +0x400.
        constexpr char NoneText[] = "None";
        for (const auto& range : ranges) {
            bool stop = false;
            ForEachWindow(
                range, 4_MB,
                [&](VAddr base, const u8* data, u64 size) {
                    for (u64 p = 0; p + 0x18 < size;) {
                        const void* hit = std::memchr(data + p, 'N', static_cast<size_t>(size - p));
                        if (hit == nullptr) {
                            break;
                        }
                        const u64 at = static_cast<u64>(static_cast<const u8*>(hit) - data);
                        if (at >= 0x10 && at + 4 < size &&
                            std::memcmp(data + at, NoneText, 4) == 0 &&
                            (data[at + 4] == 0 || at + 4 == size)) {
                            const VAddr text = base + at;
                            if (FindChunkedArrayFromText(text)) {
                                stop = true;
                                return false;
                            }
                        }
                        p = at + 1;
                    }
                    return !stop;
                });
            if (names.kind != NameKind::None) {
                return true;
            }
        }
        return false;
    }

    bool FindChunkedArrayFromText(VAddr text) {
        const VAddr entry = text - 0x10;
        bool found = false;
        for (const auto& range : ranges) {
            if (found || range.end <= range.start) {
                continue;
            }
            std::array<u8, 8> key{};
            std::memcpy(key.data(), &entry, sizeof(entry));
            ForEachWindow(range, 4_MB,
                          [&](VAddr base, const u8* data, u64 size) {
                              u64 offset = 0;
                              while (offset + sizeof(key) <= size && !found) {
                                  const void* hit = std::memchr(data + offset, key[0],
                                                                static_cast<size_t>(size - offset));
                                  if (hit == nullptr) {
                                      break;
                                  }
                                  const auto at = static_cast<u64>(static_cast<const u8*>(hit) -
                                                                   data);
                                  if (at + sizeof(key) <= size &&
                                      std::memcmp(data + at, key.data(), key.size()) == 0) {
                                      const VAddr chunk0 = base + at;
                                      if (ValidateChunkedNameArray(chunk0 - 0x18)) {
                                          found = true;
                                          break;
                                      }
                                  }
                                  offset = at + 1;
                              }
                              return !found;
                          });
        }
        return found;
    }

    bool ValidateChunkedNameArray(VAddr address) {
        u32 count = 0;
        u32 chunks = 0;
        if (!ReadU32(address + 0x400, count) || !ReadU32(address + 0x404, chunks)) {
            return false;
        }
        if (chunks == 0 || chunks > 128 || count == 0 ||
            count > chunks * NamesPerChunk) {
            return false;
        }
        for (u32 chunk = 0; chunk < chunks; ++chunk) {
            u64 chunk_address = 0;
            if (!ReadU64(address + 8 * chunk, chunk_address) || !InRange(chunk_address, 8)) {
                return false;
            }
        }
        u64 unused = 0;
        if (!ReadU64(address + 8 * chunks, unused) || unused != 0) {
            return false;
        }
        names.kind = NameKind::Chunked;
        names.address = address;
        names.count = count;
        names.chunks = chunks;
        return true;
    }

    // -- object array ----------------------------------------------------------------------

    enum class ArrayKind { None, Chunked, Flat };

    struct ObjectArray {
        ArrayKind kind = ArrayKind::None;
        VAddr address = 0;
        VAddr items = 0;
        u32 count = 0;
        u32 capacity = 0;
        u32 chunks = 0;
        u32 max_chunks = 0;
        u32 item_size = 16;
        u64 item_offset = 0;
        u32 per_chunk = 0;
        std::array<u64, 5> layout{};
    } objects;

    bool ReadObjectPointer(u32 index, VAddr& out) const {
        out = 0;
        if (index >= objects.count) {
            return false;
        }
        if (objects.kind == ArrayKind::Flat) {
            u64 item = 0;
            return ReadU64(objects.items + index * objects.item_size + objects.item_offset, item) &&
                   item != 0 && (out = item);
        }
        if (objects.per_chunk == 0) {
            return false;
        }
        u64 chunk = 0;
        if (!ReadU64(objects.items + 8 * (index / objects.per_chunk), chunk) || chunk == 0) {
            return false;
        }
        u64 item = 0;
        if (!ReadU64(chunk + (index % objects.per_chunk) * objects.item_size + objects.item_offset,
                     item)) {
            return false;
        }
        out = item;
        return item != 0;
    }

    bool MeasureItem(VAddr chunk0, u32& item_size, u64& item_offset) const {
        u32 best_hits = 0;
        u32 best_size = 16;
        u64 best_offset = 0;
        for (const u32 stride : {16u, 20u, 24u, 32u, 40u}) {
            for (u64 offset = 0; offset + 8 <= stride; offset += 8) {
                u32 hits = 0;
                for (u32 k = 1; k < 40; ++k) {
                    u64 candidate = 0;
                    if (!ReadU64(chunk0 + k * stride + offset, candidate) || !InRange(candidate, 0x28)) {
                        continue;
                    }
                    s32 object_index = 0;
                    if (ReadS32(candidate + ObjectIndexOffset, object_index) &&
                        object_index == static_cast<s32>(k)) {
                        ++hits;
                    }
                }
                if (hits > best_hits) {
                    best_hits = hits;
                    best_size = stride;
                    best_offset = offset;
                }
            }
        }
        item_size = best_size;
        item_offset = best_offset;
        return best_hits > 0;
    }

    bool ValidateChunkedArray(VAddr address, const std::array<u64, 5>& layout) {
        u64 items = 0;
        u32 capacity = 0;
        u32 count = 0;
        u32 max_chunks = 0;
        u32 chunks = 0;
        if (!ReadU64(address + layout[0], items) || !ReadU32(address + layout[1], capacity) ||
            !ReadU32(address + layout[2], count) || !ReadU32(address + layout[3], max_chunks) ||
            !ReadU32(address + layout[4], chunks)) {
            return false;
        }
        if (!InRange(items, 8) || chunks == 0 || chunks > 0x14 || max_chunks < 6 ||
            max_chunks > 0x5FF || count <= 0x800 || capacity <= 0x10000) {
            return false;
        }
        if (count > capacity || chunks > max_chunks || capacity % 0x10 != 0) {
            return false;
        }
        const u32 per_chunk = capacity / max_chunks;
        if (per_chunk % 0x10 != 0 || per_chunk < 0x8000 || per_chunk > 0x80000) {
            return false;
        }
        if (count / per_chunk + 1 != chunks || capacity / per_chunk != max_chunks) {
            return false;
        }
        if (count > options.max_objects) {
            return false;
        }
        for (u32 chunk = 0; chunk < chunks; ++chunk) {
            u64 chunk_address = 0;
            if (!ReadU64(items + 8 * chunk, chunk_address) || !InRange(chunk_address, 8)) {
                return false;
            }
        }
        u64 chunk0 = 0;
        if (!ReadU64(items, chunk0)) {
            return false;
        }
        u32 item_size = 16;
        u64 item_offset = 0;
        if (!MeasureItem(chunk0, item_size, item_offset)) {
            return false;
        }
        objects.kind = ArrayKind::Chunked;
        objects.address = address;
        objects.items = items;
        objects.count = count;
        objects.capacity = capacity;
        objects.chunks = chunks;
        objects.max_chunks = max_chunks;
        objects.per_chunk = per_chunk;
        objects.item_size = item_size;
        objects.item_offset = item_offset;
        objects.layout = layout;
        return true;
    }

    bool ValidateFlatArray(VAddr address) {
        u64 items = 0;
        u32 capacity = 0;
        u32 count = 0;
        if (!ReadU64(address, items) || !ReadU32(address + 8, capacity) ||
            !ReadU32(address + 12, count)) {
            return false;
        }
        if (!InRange(items, 16) || count <= 0x800 || capacity < count || capacity > 0x800000) {
            return false;
        }
        if (count > options.max_objects) {
            return false;
        }
        u32 item_size = 16;
        u64 item_offset = 0;
        if (!MeasureItem(items, item_size, item_offset)) {
            return false;
        }
        objects.kind = ArrayKind::Flat;
        objects.address = address;
        objects.items = items;
        objects.count = count;
        objects.capacity = capacity;
        objects.chunks = 1;
        objects.max_chunks = 1;
        objects.per_chunk = capacity;
        objects.item_size = item_size;
        objects.item_offset = item_offset;
        return true;
    }

    bool FindObjectArray() {
        for (const auto& range : writable_ranges) {
            if (range.end <= range.start) {
                continue;
            }
            const u64 total = range.end - range.start;
            const u64 limit = std::min(total, options.max_scan_bytes);
            for (u64 offset = 0; offset + 0x40 <= limit; offset += 4) {
                const VAddr address = range.start + offset;
                for (const auto& layout : ChunkedLayouts) {
                    if (ValidateChunkedArray(address, layout)) {
                        return true;
                    }
                }
                if (ValidateFlatArray(address)) {
                    return true;
                }
            }
            if (objects.kind != ArrayKind::None) {
                return true;
            }
        }
        return objects.kind != ArrayKind::None;
    }

    // -- objects ---------------------------------------------------------------------------

    bool ReadObject(VAddr address, ObjectInfo& out) const {
        out = {};
        if (!InRange(address, 0x28)) {
            return false;
        }
        u32 index = 0;
        u64 klass = 0;
        u32 name_index = 0;
        u32 number = 0;
        u64 outer = 0;
        u32 flags = 0;
        if (!ReadRaw(address + ObjectIndexOffset, &index, sizeof(index)) ||
            !ReadU64(address + ObjectClassOffset, klass) || !InRange(klass, 0x28) ||
            !ReadRaw(address + ObjectNameOffset, &name_index, sizeof(name_index)) ||
            !ReadRaw(address + ObjectNameOffset + 4, &number, sizeof(number)) ||
            !ReadU64(address + ObjectOuterOffset, outer) ||
            !ReadRaw(address + ObjectFlagsOffset, &flags, sizeof(flags))) {
            return false;
        }
        std::string name;
        if (!ResolveName(name_index, name)) {
            return false;
        }
        if (number > 0) {
            name += "_" + std::to_string(number - 1);
        }
        u32 class_name_index = 0;
        if (!ReadU32(klass + ObjectNameOffset, class_name_index)) {
            return false;
        }
        std::string class_name;
        if (!ResolveName(class_name_index, class_name)) {
            return false;
        }
        out.index = index;
        out.address = address;
        out.class_address = klass;
        out.outer_address = outer;
        out.flags = flags;
        out.name = std::move(name);
        out.class_name = std::move(class_name);
        return true;
    }

    const GuestMemory& memory;
    std::vector<AddressRange> ranges;
    std::vector<AddressRange> writable_ranges;
    DiscoveryOptions options;
    mutable std::vector<u8> scratch;
};

Inspector::Inspector(const GuestMemory& memory, ScanRanges scan_ranges, DiscoveryOptions options)
    : impl{std::make_unique<Impl>(memory, std::move(scan_ranges), options)} {}

Inspector::~Inspector() = default;

const DiscoveryResult& Inspector::Discover() {
    result = {};
    impl->names = {};
    impl->objects = {};


    if (impl->FindNamePool()) {
        result.name_table_kind = "FNamePool";
    } else if (impl->FindChunkedNameArray()) {
        result.name_table_kind = "TNameEntryArray";
    }
    result.names_found = impl->names.kind != Impl::NameKind::None;
    result.names_address = impl->names.address;
    if (!result.names_found) {
        result.error = "name table not found";
        return result;
    }

    if (!impl->FindObjectArray()) {
        result.error = "object array not found";
        return result;
    }
    result.objects_found = true;
    result.objects_address = impl->objects.address;
    result.object_count = impl->objects.count;
    result.object_capacity = impl->objects.capacity;
    result.object_chunks = impl->objects.chunks;
    result.object_max_chunks = impl->objects.max_chunks;
    result.object_items_per_chunk = impl->objects.per_chunk;
    result.object_item_size = impl->objects.item_size;
    result.object_item_offset = impl->objects.item_offset;
    if (impl->objects.kind == Impl::ArrayKind::Flat) {
        result.object_layout = "0x00,0x08,0x0c";
    } else {
        char text[96];
        const auto& layout = impl->objects.layout;
        std::snprintf(text, sizeof(text), "0x%02x,0x%02x,0x%02x,0x%02x,0x%02x",
                      static_cast<unsigned>(layout[0]), static_cast<unsigned>(layout[1]),
                      static_cast<unsigned>(layout[2]), static_cast<unsigned>(layout[3]),
                      static_cast<unsigned>(layout[4]));
        result.object_layout = text;
    }
    result.object_array_kind =
        impl->objects.kind == Impl::ArrayKind::Flat ? "FFixedUObjectArray" : "FChunkedFixedUObjectArray";
    return result;
}

bool Inspector::GetObject(u32 index, ObjectInfo& out) const {
    VAddr address = 0;
    if (!impl->ReadObjectPointer(index, address)) {
        return false;
    }
    if (!impl->ReadObject(address, out)) {
        return false;
    }
    // A recycled slot holds a different object, usually with another index.
    return out.index == index;
}

std::vector<ObjectInfo> Inspector::SearchByName(std::string_view text, u32 limit) const {
    std::vector<ObjectInfo> found;
    const auto needle = ToLower(text);
    for (u32 index = 0; index < ObjectCount() && found.size() < limit; ++index) {
        ObjectInfo info;
        if (!GetObject(index, info)) {
            continue;
        }
        if (ToLower(info.name).find(needle) != std::string::npos) {
            found.push_back(std::move(info));
        }
    }
    return found;
}

std::vector<ObjectInfo> Inspector::SearchByClass(std::string_view class_name, u32 limit) const {
    std::vector<ObjectInfo> found;
    const auto needle = ToLower(class_name);
    for (u32 index = 0; index < ObjectCount() && found.size() < limit; ++index) {
        ObjectInfo info;
        if (!GetObject(index, info)) {
            continue;
        }
        if (ToLower(info.class_name) == needle) {
            found.push_back(std::move(info));
        }
    }
    return found;
}

bool Inspector::FollowReference(const ObjectInfo& object, u64 offset, ObjectInfo& out) const {
    u64 target = 0;
    if (!impl->ReadU64(object.address + offset, target) || target == 0) {
        return false;
    }
    return impl->ReadObject(target, out);
}

bool Inspector::IsLive(u32 index, const ObjectInfo& object) const {
    VAddr address = 0;
    if (!impl->ReadObjectPointer(index, address) || address != object.address) {
        return false;
    }
    ObjectInfo current;
    return impl->ReadObject(address, current) && current.index == index &&
           current.class_address == object.class_address;
}

u32 Inspector::ObjectCount() const {
    return impl->objects.count;
}

std::optional<ObjectInfo> Inspector::ObjectAt(u32 index) const {
    ObjectInfo info;
    if (!GetObject(index, info)) {
        return std::nullopt;
    }
    return info;
}

} // namespace Core::Unreal
