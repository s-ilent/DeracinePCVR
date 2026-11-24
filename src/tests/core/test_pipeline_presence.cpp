// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for pipeline-state presence semantics.
//
// The defect these exist to prevent: zero is a legitimate value for every pipeline enum this
// renderer applies. `VK_POLYGON_MODE_FILL` is 0, `VK_COMPARE_OP_NEVER` is 0,
// `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` is 0 and `VK_CULL_MODE_NONE` is 0. Testing a value against
// zero to decide whether it was captured substitutes the diagnostic default for exactly the fields
// the game set to their zero value.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "package_reader.h"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("rhi03_presence_" + std::to_string(::testing::UnitTest::GetInstance()
                                                        ->random_seed()) +
                 "_" + std::to_string(counter_++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    [[nodiscard]] const fs::path& path() const {
        return path_;
    }

private:
    fs::path path_;
    static inline int counter_ = 0;
};

void Write(const fs::path& path, const std::string& text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
}

/// A schema-4 package whose pipeline state carries the given presence block and values.
std::string PackageJson(const std::string& present, const std::string& values) {
    return std::string{R"({
  "schema_version": 4,
  "provenance_id": 1,
  "resources": [], "vertex_bindings": [], "textures": [], "samplers": [],
  "shader_stages": [], "shader_interfaces": [],
  "pipeline_state": { )"} +
           present + values + R"( },
  "readiness": {}
})";
}

package::Package Read(const TempDir& dir) {
    package::Package out{};
    std::string error;
    package::Read(dir.path().string(), out, error);
    return out;
}

TEST(PipelinePresence, AZeroPolygonModeIsPresentNotAbsent) {
    // VK_POLYGON_MODE_FILL is 0. A package that captured it must report it as present.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "polygon_mode": true }, )", R"("polygon_mode": 0)"));
    const auto package = Read(dir);
    EXPECT_TRUE(package.pipeline_state.present.polygon_mode);
    EXPECT_EQ(package.pipeline_state.polygon_mode, 0u);
}

TEST(PipelinePresence, AZeroDepthCompareIsPresentNotAbsent) {
    // VK_COMPARE_OP_NEVER is 0.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "depth_compare_op": true }, )", R"("depth_compare_op": 0)"));
    const auto package = Read(dir);
    EXPECT_TRUE(package.pipeline_state.present.depth_compare_op);
    EXPECT_EQ(package.pipeline_state.depth_compare_op, 0u);
}

TEST(PipelinePresence, AZeroCullModeIsPresentNotAbsent) {
    // VK_CULL_MODE_NONE is 0.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "cull_mode": true }, )", R"("cull_mode": 0)"));
    const auto package = Read(dir);
    EXPECT_TRUE(package.pipeline_state.present.cull_mode);
    EXPECT_EQ(package.pipeline_state.cull_mode, 0u);
}

TEST(PipelinePresence, AZeroTopologyIsPresentNotAbsent) {
    // VK_PRIMITIVE_TOPOLOGY_POINT_LIST is 0.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "topology": true }, )",
                      R"("has_topology": true, "topology": 0)"));
    const auto package = Read(dir);
    EXPECT_TRUE(package.pipeline_state.present.topology);
    EXPECT_EQ(package.pipeline_state.topology, 0u);
}

TEST(PipelinePresence, AMissingFieldIsNotPresent) {
    // The distinction the whole mechanism exists for: absent is not the same as zero.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "polygon_mode": false }, )", R"("polygon_mode": 0)"));
    const auto package = Read(dir);
    EXPECT_FALSE(package.pipeline_state.present.polygon_mode);
    EXPECT_EQ(package.pipeline_state.polygon_mode, 0u);
}

TEST(PipelinePresence, EveryFieldIsReadIndependently) {
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson(R"("present": { "topology": true, "cull_mode": false,
                                      "front_face": true, "polygon_mode": false,
                                      "depth_test": true, "depth_write": false,
                                      "depth_compare_op": true, "samples": false,
                                      "color_formats": true, "depth_format": false,
                                      "write_mask": true, "blend_enable": false,
                                      "viewport": true }, )",
                      R"("topology": 3, "cull_mode": 2, "front_face": 1, "polygon_mode": 0,
                         "depth_test": true, "depth_write": false, "depth_compare_op": 0,
                         "samples": 1, "color_formats": [37], "depth_format": 0,
                         "write_mask": [15], "blend_enable": [0], "viewport_width": 64.0)"));
    const auto package = Read(dir);
    const auto& present = package.pipeline_state.present;
    EXPECT_TRUE(present.topology);
    EXPECT_FALSE(present.cull_mode);
    EXPECT_TRUE(present.front_face);
    EXPECT_FALSE(present.polygon_mode);
    EXPECT_TRUE(present.depth_test);
    EXPECT_FALSE(present.depth_write);
    EXPECT_TRUE(present.depth_compare_op);
    EXPECT_FALSE(present.samples);
    EXPECT_TRUE(present.color_formats);
    EXPECT_FALSE(present.depth_format);
    EXPECT_TRUE(present.write_mask);
    EXPECT_FALSE(present.blend_enable);
    EXPECT_TRUE(present.viewport);
}

TEST(PipelinePresence, APackageWithoutTheBlockClaimsOnlyWhatItAlwaysCarried) {
    // A package that predates the presence block is not guessed at. The producer always wrote the
    // topology and the colour formats, so those are claimed; nothing else is.
    TempDir dir;
    Write(dir.path() / "package.json",
          PackageJson("", R"("has_topology": true, "topology": 3, "color_formats": [37])"));
    const auto package = Read(dir);
    EXPECT_TRUE(package.pipeline_state.present.topology);
    EXPECT_TRUE(package.pipeline_state.present.color_formats);
    EXPECT_FALSE(package.pipeline_state.present.polygon_mode);
    EXPECT_FALSE(package.pipeline_state.present.depth_compare_op);
    EXPECT_FALSE(package.pipeline_state.present.samples);
}

} // namespace
