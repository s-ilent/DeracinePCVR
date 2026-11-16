// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Unreal {

// One readable guest mapping as the snapshot index describes it. `prot` uses the PS4 bit layout
// the offline tools expect: 1 read, 2 write, 4 execute.
struct SnapshotRegion {
    VAddr start = 0;
    u64 size = 0;
    u32 prot = 0;
    std::string name;
};

struct SnapshotMetadata {
    std::string title_id;
    std::string title_name;
    std::string app_version;
    std::string executable_path;
    std::string emulator_commit;
    std::string time;
};

struct SnapshotOptions {
    // The offline tooling reads the dump in 1 MiB chunks and drops all-zero ones, which is what
    // keeps a multi-gigabyte reservation from becoming a multi-gigabyte file.
    u64 chunk_size = 1_MB;
    // Reserved-but-unmapped regions can be huge; a region above this is skipped and reported.
    u64 max_region_size = 16_GB;
    // Optional cap on stored bytes, 0 = unlimited.
    u64 max_total_size = 0;
    bool skip_zero_chunks = true;
    // Regions are named "executable" only for the main module; other names pass through.
    std::string executable_name = "executable";
};

struct SnapshotRegionEntry {
    std::string name;
    VAddr start = 0;
    VAddr end = 0;
    u32 prot = 0;
    u64 offset = 0;
};

struct SnapshotHole {
    VAddr address = 0;
    u64 size = 0;
};

struct SnapshotResult {
    bool ok = false;
    std::string error;
    u64 bytes_written = 0;
    u64 regions = 0;
    u64 holes = 0;
    u64 chunks_stored = 0;
    u64 chunks_zero_skipped = 0;
    u64 regions_too_large = 0;
    std::vector<SnapshotRegionEntry> index;
};

using SnapshotReader = std::function<bool(VAddr address, void* out, u64 size)>;

// Reads the emulator's currently mapped guest ranges, in address order, naming the main module's
// mappings "executable" the way the offline tools expect.
std::vector<SnapshotRegion> EnumerateLiveRegions(const std::string& executable_vma_name,
                                                 const std::string& executable_name = "executable");

// Reads live guest memory, failing rather than faulting on an unmapped range.
bool ReadLiveRegion(VAddr address, void* out, u64 size);

// Writes <basename>.bin and <basename>.json in the ps4-unreal-dumper layout: a flat file of
// stored chunks plus an index of {name,start,end,prot,offset} regions and unreadable holes.
// Nothing is written when `regions` is empty or the bin cannot be opened.
SnapshotResult WriteSnapshot(const std::filesystem::path& basename,
                             const std::vector<SnapshotRegion>& regions, const SnapshotReader& read,
                             const SnapshotMetadata& metadata, const SnapshotOptions& options = {});

} // namespace Core::Unreal
