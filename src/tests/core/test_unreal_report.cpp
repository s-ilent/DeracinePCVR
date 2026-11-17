// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/unreal/inspector.h"
#include "core/unreal/report.h"
#include "tests/core/unreal_fixture.h"

namespace Core::Unreal {
namespace {

namespace fs = std::filesystem;

ScanRanges RangesOf(const Core::Unreal::Test::Image& image) {
    ScanRanges ranges;
    for (const auto& region : image.Regions()) {
        ranges.all.push_back({region.start, region.start + region.size});
        if (region.name == "executable" && (region.prot & 2) != 0) {
            ranges.writable.push_back({region.start, region.start + region.size});
        }
    }
    return ranges;
}

SnapshotMetadata Metadata() {
    SnapshotMetadata metadata;
    metadata.title_id = "CUSA11919";
    metadata.title_name = "Deracine";
    metadata.app_version = "01.00";
    metadata.executable_path = "D:/Temp/Deracine/1.0/Image0/eboot.bin";
    metadata.emulator_commit = "2cbfc56";
    metadata.time = "2026-11-17 02:40:00";
    return metadata;
}

class ReportFromFixture : public ::testing::Test {
protected:
    void SetUp() override {
        image = std::make_unique<::Core::Unreal::Test::Image>();
        inspector = std::make_unique<Inspector>(*image, RangesOf(*image));
        inspector->Discover();
        ASSERT_TRUE(inspector->Result().ok()) << inspector->Result().error;
    }

    std::unique_ptr<::Core::Unreal::Test::Image> image;
    std::unique_ptr<Inspector> inspector;
};

TEST_F(ReportFromFixture, DescribesDiscoveryAndMeasuredLayout) {
    const auto report = BuildDiagnosticReport(*inspector, Metadata());

    EXPECT_EQ(report["report"], "shadPS4-unreal-inspector/1");
    EXPECT_EQ(report["title"]["id"], "CUSA11919");
    ASSERT_TRUE(report["discovery"]["ok"].get<bool>());

    const auto& names = report["discovery"]["name_table"];
    EXPECT_EQ(names["kind"], "FNamePool");
    EXPECT_EQ(names["address"], "0x0000000800001000");

    const auto& objects = report["discovery"]["object_array"];
    EXPECT_EQ(objects["kind"], "FChunkedFixedUObjectArray");
    EXPECT_EQ(objects["layout"], "0x00,0x10,0x14,0x18,0x1c");
    EXPECT_EQ(objects["item_size"], 16);
    EXPECT_EQ(objects["item_offset"], 0);
    EXPECT_EQ(objects["count"], Core::Unreal::Test::Image::ObjectCount);
    EXPECT_EQ(objects["capacity"], Core::Unreal::Test::Image::MaxObjects);
    EXPECT_EQ(objects["chunks"], 1);
}

TEST_F(ReportFromFixture, CountsLiveAndStaleSlots) {
    const auto report = BuildDiagnosticReport(*inspector, Metadata());
    EXPECT_EQ(report["objects"]["scanned"], Core::Unreal::Test::Image::ObjectCount);
    EXPECT_EQ(report["objects"]["live"], Core::Unreal::Test::Image::ObjectCount);
    EXPECT_EQ(report["objects"]["stale_slots"], 0);
    EXPECT_EQ(report["objects"]["array_count"], Core::Unreal::Test::Image::ObjectCount);
}

TEST_F(ReportFromFixture, ListsClassesAndHandCandidates) {
    const auto report = BuildDiagnosticReport(*inspector, Metadata());

    ASSERT_FALSE(report["classes"].empty());
    const auto& top = report["classes"][0];
    EXPECT_EQ(top["class"], "Class");
    EXPECT_EQ(top["count"], Core::Unreal::Test::Image::ObjectCount - 2);

    std::vector<std::string> candidate_names;
    for (const auto& candidate : report["candidates"]) {
        candidate_names.push_back(candidate["name"].get<std::string>());
    }
    EXPECT_NE(std::find(candidate_names.begin(), candidate_names.end(), "HandMeshLeft"),
              candidate_names.end());
    EXPECT_NE(std::find(candidate_names.begin(), candidate_names.end(), "HandMeshRight"),
              candidate_names.end());
    EXPECT_NE(std::find(candidate_names.begin(), candidate_names.end(), "HandAnimBlueprint"),
              candidate_names.end());

    for (const auto& candidate : report["candidates"]) {
        if (candidate["name"] == "HandMeshLeft") {
            EXPECT_EQ(candidate["class"], "SkeletalMeshComponent");
            EXPECT_EQ(candidate["address"], "0x0000000800100500");
        }
    }
}

TEST_F(ReportFromFixture, WritesIndentedJson) {
    const fs::path path = fs::path(::testing::TempDir()) / "shadps4-unreal-report" / "report.json";
    std::string error;
    ASSERT_TRUE(WriteDiagnosticReport(path, *inspector, Metadata(), {}, &error)) << error;
    ASSERT_TRUE(fs::exists(path));

    std::ifstream file{path};
    nlohmann::json report;
    file >> report;
    EXPECT_EQ(report["report"], "shadPS4-unreal-inspector/1");
    EXPECT_TRUE(report["discovery"]["ok"].get<bool>());
}

TEST(UnrealReport, RefusesToDescribeAFailedDiscovery) {
    std::vector<u8> zeros(0x1000, 0);
    class Buffer final : public GuestMemory {
    public:
        explicit Buffer(std::vector<u8> bytes) : bytes{std::move(bytes)} {}
        bool Read(VAddr address, void* out, u64 size) const override {
            if (address < 0x100000 || address + size > 0x100000 + bytes.size()) {
                return false;
            }
            std::memcpy(out, bytes.data() + (address - 0x100000), size);
            return true;
        }

    private:
        std::vector<u8> bytes;
    } buffer{zeros};

    const ScanRanges ranges{{{{0x100000, 0x101000}}}, {{{0x100000, 0x101000}}}};
    Inspector inspector{buffer, ranges};
    inspector.Discover();

    const auto report = BuildDiagnosticReport(inspector, Metadata());
    EXPECT_FALSE(report["discovery"]["ok"].get<bool>());
    EXPECT_FALSE(report["discovery"]["error"].get<std::string>().empty());
    EXPECT_TRUE(report["classes"].empty());
}

} // Anonymous namespace
} // namespace Core::Unreal
