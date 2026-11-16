// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/unreal/guest_memory.h"

namespace Core::Unreal {

// UE4 UObject field offsets. Every read validates the class and name they lead to, so a build with
// a different layout fails discovery instead of reporting wrong objects.
constexpr u64 ObjectFlagsOffset = 0x08;
constexpr u64 ObjectIndexOffset = 0x0C;
constexpr u64 ObjectClassOffset = 0x10;
constexpr u64 ObjectNameOffset = 0x18;
constexpr u64 ObjectOuterOffset = 0x20;

struct AddressRange {
    VAddr start = 0;
    VAddr end = 0;
};

// What a live UObject looks like through its own class/name/outer fields.
struct ObjectInfo {
    u32 index = 0;
    VAddr address = 0;
    VAddr class_address = 0;
    VAddr outer_address = 0;
    u32 flags = 0;
    std::string name;
    std::string class_name;

    bool operator==(const ObjectInfo&) const = default;
};

// Guest ranges to search: `all` for the name table (it can live anywhere), `writable` for the
// object array, which sits in the main image's data segment.
struct ScanRanges {
    std::vector<AddressRange> all;
    std::vector<AddressRange> writable;
};

struct DiscoveryOptions {
    // Bounds for scanning guest memory that belongs to the main image's writable segment.
    u64 max_scan_bytes = 1_GB;
    u32 max_objects = 1u << 20;
    u32 max_name_length = 256;
    u32 max_outline_depth = 4;
};

struct DiscoveryResult {
    bool names_found = false;
    bool objects_found = false;
    VAddr names_address = 0;
    VAddr objects_address = 0;
    u32 object_count = 0;
    u32 object_capacity = 0;
    u32 object_chunks = 0;
    u32 object_max_chunks = 0;
    u32 object_items_per_chunk = 0;
    u32 object_item_size = 0;
    u64 object_item_offset = 0;
    // Field offsets of the array layout that was recognised, hex formatted.
    std::string object_layout;
    std::string name_table_kind;
    std::string object_array_kind;
    std::string error;

    bool ok() const {
        return names_found && objects_found;
    }
};

// Read-only view of a running Unreal Engine 4 game: finds the name table and the object array from
// their structure (never from a hardcoded address), then exposes objects defensively, because the
// game frees and recycles them while this runs.
class Inspector {
public:
    Inspector(const GuestMemory& memory, ScanRanges scan_ranges, DiscoveryOptions options = {});
    ~Inspector();

    Inspector(const Inspector&) = delete;
    Inspector& operator=(const Inspector&) = delete;

    // Scans `scan_ranges` once. Repeatable: clears the previous result first.
    const DiscoveryResult& Discover();

    const DiscoveryResult& Result() const {
        return result;
    }

    // A live UObject, or false if the slot no longer holds the object it used to (recycled).
    bool GetObject(u32 index, ObjectInfo& out) const;

    // Case-insensitive substring match on the object name, and exact match on its class name.
    std::vector<ObjectInfo> SearchByName(std::string_view text, u32 limit) const;
    std::vector<ObjectInfo> SearchByClass(std::string_view class_name, u32 limit) const;

    // Reads a UObject pointer stored at `object + offset` and keeps it only when it points at a
    // plausible UObject. Used to walk Outer chains and component references.
    bool FollowReference(const ObjectInfo& object, u64 offset, ObjectInfo& out) const;

    // True when the object still sits in its original slot and its class and name still resolve.
    bool IsLive(u32 index, const ObjectInfo& object) const;

    u32 ObjectCount() const;
    std::optional<ObjectInfo> ObjectAt(u32 index) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl;
    DiscoveryResult result;
};

} // namespace Core::Unreal
