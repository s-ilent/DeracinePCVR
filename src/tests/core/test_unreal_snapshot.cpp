// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

#include "core/unreal/snapshot.h"
#include "tests/core/unreal_fixture.h"

namespace Core::Unreal {
namespace {

namespace fs = std::filesystem;

std::vector<SnapshotRegion> FixtureRegions(const Test::Image& image) {
    std::vector<SnapshotRegion> regions;
    for (const auto& region : image.Regions()) {
        regions.push_back({region.start, region.size, region.prot, region.name});
    }
    return regions;
}

SnapshotMetadata Metadata() {
    SnapshotMetadata metadata;
    metadata.title_id = "CUSA11919";
    metadata.title_name = "Deracine";
    metadata.app_version = "01.00";
    metadata.executable_path = "D:/Temp/Deracine/1.0/Image0/eboot.bin";
    metadata.emulator_commit = "e21a9e2";
    metadata.time = "2026-11-17 01:00:00";
    return metadata;
}

fs::path WorkingPath(const std::string& name) {
    const auto dir = fs::path(::testing::TempDir()) / "shadps4-unreal-snapshot" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir / "dump";
}

nlohmann::json ReadIndex(const fs::path& basename) {
    std::ifstream file{basename.string() + ".json"};
    nlohmann::json index;
    file >> index;
    return index;
}

u64 FileSize(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? 0 : size;
}

// Reads 8 bytes straight out of the stored dump through the index, the way the offline tools do.
bool ReadFromDump(const fs::path& basename, const nlohmann::json& index, VAddr address, void* out,
                  u64 size) {
    for (const auto& region : index["regions"]) {
        const auto start = region["start"].get<VAddr>();
        const auto end = region["end"].get<VAddr>();
        if (address >= start && address + size <= end) {
            std::ifstream file{basename.string() + ".bin", std::ios::binary};
            file.seekg(static_cast<std::streamoff>(region["offset"].get<u64>() + address - start));
            file.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
            return static_cast<u64>(file.gcount()) == size;
        }
    }
    return false;
}

TEST(SnapshotFormat, WritesBinAndIndexInDumperLayout) {
    const Core::Unreal::Test::Image image;
    const auto basename = WorkingPath("layout");

    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) { return image.Read(address, out, size); },
        Metadata());

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(FileSize(basename.string() + ".bin"), result.bytes_written);
    EXPECT_GT(result.bytes_written, 0u);
    EXPECT_EQ(result.holes, 0u);

    const auto index = ReadIndex(basename);
    EXPECT_EQ(index["title_id"], "CUSA11919");
    EXPECT_EQ(index["name"], "Deracine");
    EXPECT_EQ(index["version"], "01.00");
    EXPECT_EQ(index["emulator_commit"], "e21a9e2");
    EXPECT_EQ(index["host"], "shadPS4");

    ASSERT_FALSE(index["regions"].empty());
    EXPECT_EQ(index["regions"].size(), result.index.size());
    EXPECT_TRUE(index["holes"].empty());

    // Offsets must be allocated densely from zero and stay inside the file.
    u64 expected_offset = 0;
    for (const auto& region : index["regions"]) {
        EXPECT_EQ(region["offset"].get<u64>(), expected_offset);
        expected_offset += region["end"].get<u64>() - region["start"].get<VAddr>();
        EXPECT_LE(expected_offset, result.bytes_written);
        EXPECT_GT(region["end"].get<VAddr>(), region["start"].get<VAddr>());
    }

    // The name pool bytes must survive at their virtual address.
    const VAddr pool_address = image.NamePoolAddress();
    std::vector<u8> expected(16);
    ASSERT_TRUE(image.Read(pool_address, expected.data(), expected.size()));
    std::vector<u8> stored(16);
    ASSERT_TRUE(ReadFromDump(basename, index, pool_address, stored.data(), stored.size()));
    EXPECT_EQ(stored, expected);
}

TEST(SnapshotFormat, SkipsAllZeroChunks) {
    const Core::Unreal::Test::Image image;
    const auto basename = WorkingPath("zero");

    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) { return image.Read(address, out, size); },
        Metadata());

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_GT(result.chunks_zero_skipped, 0u);
    const auto index = ReadIndex(basename);
    for (const auto& region : index["regions"]) {
        EXPECT_NE(region["name"].get<std::string>(), "anon");
    }
}

TEST(SnapshotFormat, RecordsUnreadableChunksAsHoles) {
    const Core::Unreal::Test::Image image;
    const auto basename = WorkingPath("holes");
    const VAddr failed_from = image.Regions().front().start + image.Regions().front().size / 2;

    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) {
            if (address >= failed_from) {
                return false;
            }
            return image.Read(address, out, size);
        },
        Metadata());

    ASSERT_TRUE(result.ok) << result.error;
    // Both the second chunk of the executable mapping and the whole second mapping fail.
    EXPECT_EQ(result.holes, 2u);
    const auto index = ReadIndex(basename);
    ASSERT_EQ(index["holes"].size(), 2u);
    // Holes are recorded at the chunk that could not be read, not at the first bad byte.\n    EXPECT_EQ(index["holes"][0][0].get<VAddr>(), image.Regions().front().start + 0x100000);
    EXPECT_EQ(FileSize(basename.string() + ".bin"), result.bytes_written);
    EXPECT_LT(result.bytes_written, image.Size());
}

TEST(SnapshotFormat, MergesAdjacentChunksIntoSingleRegion) {
    std::vector<u8> data(0x4000);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<u8>(i * 31 + 7);
    }
    const std::vector<SnapshotRegion> regions{{0x200000, data.size(), 3, "executable"}};

    SnapshotOptions options;
    options.chunk_size = 0x1000;
    const auto basename = WorkingPath("merge");
    const auto result =
        WriteSnapshot(basename, regions,
                      [&](VAddr address, void* out, u64 size) {
                          std::memcpy(out, data.data() + (address - 0x200000), size);
                          return true;
                      },
                      Metadata(), options);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.regions, 1u);
    const auto index = ReadIndex(basename);
    ASSERT_EQ(index["regions"].size(), 1u);
    EXPECT_EQ(index["regions"][0]["start"].get<VAddr>(), 0x200000u);
    EXPECT_EQ(index["regions"][0]["end"].get<VAddr>(), 0x204000u);
    EXPECT_EQ(result.bytes_written, data.size());
}

TEST(SnapshotFormat, SkipsRegionsAboveTheSizeLimit) {
    const Core::Unreal::Test::Image image;
    const auto basename = WorkingPath("oversize");

    SnapshotOptions options;
    options.max_region_size = 0x800;
    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) { return image.Read(address, out, size); },
        Metadata(), options);

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.regions_too_large, 2u);
    EXPECT_EQ(result.bytes_written, 0u);
    EXPECT_TRUE(ReadIndex(basename)["regions"].empty());
}

TEST(SnapshotFormat, StopsAtTheTotalSizeLimit) {
    const Core::Unreal::Test::Image image;
    const auto basename = WorkingPath("limit");

    SnapshotOptions options;
    options.max_total_size = 1_MB;
    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) { return image.Read(address, out, size); },
        Metadata(), options);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "snapshot size limit reached");
    EXPECT_EQ(result.bytes_written, 1_MB);
    EXPECT_EQ(FileSize(basename.string() + ".bin"), 1_MB);
}

TEST(SnapshotFormat, RejectsMissingRegionsOrReader) {
    const auto basename = WorkingPath("invalid");
    const auto result = WriteSnapshot(basename, {}, SnapshotReader{}, Metadata());
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

// Writes the synthetic image out for the offline dumper tools, which are run against this file by
// hand (tools/ps4-unreal-dumper). The path is relative to the test working directory.
TEST(SnapshotFormat, WritesFixtureForOfflineTools) {
    const Core::Unreal::Test::Image image;
    const fs::path basename = fs::path("unreal-fixture") / "dump";

    const auto result = WriteSnapshot(
        basename, FixtureRegions(image),
        [&](VAddr address, void* out, u64 size) { return image.Read(address, out, size); },
        Metadata());

    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(fs::exists(basename.string() + ".bin"));
    ASSERT_TRUE(fs::exists(basename.string() + ".json"));
    const auto index = ReadIndex(basename);
    EXPECT_FALSE(index["regions"].empty());
    EXPECT_EQ(index["title_id"], "CUSA11919");
}

} // Anonymous namespace
} // namespace Core::Unreal
