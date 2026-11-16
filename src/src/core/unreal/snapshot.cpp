// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/snapshot.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Core::Unreal {
namespace {

constexpr u32 Ps4ProtRead = 1;

nlohmann::json BuildIndex(const SnapshotResult& result, const SnapshotMetadata& metadata,
                          const SnapshotOptions& options, const std::vector<SnapshotHole>& holes) {
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& region : result.index) {
        regions.push_back({{"name", region.name},
                           {"start", region.start},
                           {"end", region.end},
                           {"prot", region.prot},
                           {"offset", region.offset}});
    }
    nlohmann::json hole_list = nlohmann::json::array();
    for (const auto& hole : holes) {
        hole_list.push_back({hole.address, hole.size});
    }
    return nlohmann::json{{"host", "shadPS4"},
                          {"name", metadata.title_name},
                          {"path", metadata.executable_path},
                          {"title_id", metadata.title_id},
                          {"version", metadata.app_version},
                          {"time", metadata.time},
                          {"emulator_commit", metadata.emulator_commit},
                          {"chunk_size", options.chunk_size},
                          {"regions", regions},
                          {"holes", hole_list}};
}

} // Anonymous namespace

SnapshotResult WriteSnapshot(const std::filesystem::path& basename,
                             const std::vector<SnapshotRegion>& regions, const SnapshotReader& read,
                             const SnapshotMetadata& metadata, const SnapshotOptions& options) {
    SnapshotResult result;
    if (!read) {
        result.error = "no reader";
        return result;
    }
    if (options.chunk_size == 0) {
        result.error = "chunk size must be larger than zero";
        return result;
    }
    if (regions.empty()) {
        result.error = "no readable regions to snapshot";
        return result;
    }

    std::error_code ec;
    const auto parent = basename.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    const std::string bin_path = basename.string() + ".bin";
    const std::string json_path = basename.string() + ".json";

    std::ofstream bin{bin_path, std::ios::binary | std::ios::trunc};
    if (!bin) {
        result.error = "cannot open " + bin_path;
        return result;
    }

    std::vector<SnapshotHole> holes;
    std::vector<std::byte> chunk(static_cast<size_t>(options.chunk_size));
    std::vector<std::byte> zero(static_cast<size_t>(options.chunk_size), std::byte{0});

    const auto append = [&result](const SnapshotRegion& region, VAddr start, u64 size) {
        if (!result.index.empty()) {
            auto& last = result.index.back();
            if (last.end == start && last.name == region.name && last.prot == region.prot) {
                last.end = start + size;
                return;
            }
        }
        result.index.push_back({region.name, start, start + size, region.prot,
                                result.bytes_written});
    };

    bool limit_reached = false;
    for (const auto& region : regions) {
        if (limit_reached) {
            break;
        }
        if (region.size == 0) {
            continue;
        }
        if ((region.prot & Ps4ProtRead) == 0) {
            continue;
        }
        if (region.size > options.max_region_size) {
            ++result.regions_too_large;
            continue;
        }
        for (u64 done = 0; done < region.size; done += options.chunk_size) {
            const u64 size = std::min(options.chunk_size, region.size - done);
            const VAddr address = region.start + done;
            if (!read(address, chunk.data(), size)) {
                holes.push_back({address, size});
                continue;
            }
            if (options.skip_zero_chunks &&
                std::equal(chunk.begin(), chunk.begin() + size, zero.begin())) {
                ++result.chunks_zero_skipped;
                continue;
            }
            if (options.max_total_size != 0 &&
                result.bytes_written + size > options.max_total_size) {
                limit_reached = true;
                result.error = "snapshot size limit reached";
                break;
            }
            append(region, address, size);
            bin.write(reinterpret_cast<const char*>(chunk.data()),
                      static_cast<std::streamsize>(size));
            if (!bin) {
                result.error = "failed to write " + bin_path;
                return result;
            }
            result.bytes_written += size;
            ++result.chunks_stored;
        }
    }

    bin.close();
    if (!bin) {
        result.error = "failed to close " + bin_path;
        return result;
    }

    const auto index = BuildIndex(result, metadata, options, holes);
    std::ofstream json{json_path, std::ios::trunc};
    if (!json) {
        result.error = "cannot open " + json_path;
        return result;
    }
    json << index.dump(1) << '\n';
    if (!json) {
        result.error = "failed to write " + json_path;
        return result;
    }

    result.regions = result.index.size();
    result.holes = holes.size();
    result.ok = result.error.empty();
    return result;
}

} // namespace Core::Unreal
