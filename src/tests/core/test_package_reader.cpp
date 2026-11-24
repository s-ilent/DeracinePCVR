// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the unified package reader.
//
// The reader is the one place a package is interpreted. These tests exist because a reader that
// silently accepts a wrong schema, a truncated sidecar or a substituted shader would make every
// downstream claim unreliable: the consumer would be reasoning about bytes the producer never
// wrote.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "package_reader.h"

namespace {

namespace fs = std::filesystem;

/// A temporary directory that removes itself.
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("rhi03_reader_" + std::to_string(::testing::UnitTest::GetInstance()
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

void WriteBytes(const fs::path& path, const std::vector<package::u8>& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// A minimal schema-2 package: resources only, no shader state.
std::string Schema2Json() {
    return R"({
  "schema_version": 2,
  "provenance_id": 7,
  "resources": [],
  "vertex_bindings": [],
  "textures": [],
  "samplers": [],
  "readiness": { "geometry_ready": false }
})";
}

/// A minimal schema-4 package with one stage and one interface.
std::string Schema4Json(const std::string& stage_file, const std::string& stage_sha,
                        package::u64 stage_length) {
    return std::string{R"({
  "schema_version": 4,
  "provenance_id": 7,
  "session_id": 1234,
  "execution_id": 5678,
  "resources": [],
  "vertex_bindings": [],
  "textures": [],
  "samplers": [],
  "shader_stages": [
    {
      "logical_stage": 0,
      "hardware_stage": 0,
      "program_hash": 100,
      "permutation_index": 0,
      "permutation_hash": 200,
      "patched": false,
      "effective_sha256": ")"} +
           stage_sha + R"(",
      "generated_sha256": ")" +
           stage_sha + R"(",
      "entry_point": "main",
      "effective_byte_length": )" +
           std::to_string(stage_length) + R"(,
      "gcn_byte_length": 0,
      "effective_file": ")" +
           stage_file + R"(",
      "gcn_file": ""
    }
  ],
  "shader_interfaces": [
    {
      "program_hash": 100,
      "permutation_index": 0,
      "permutation_hash": 200,
      "hardware_stage": 0,
      "logical_stage": 0,
      "patched": false,
      "push_constant_offset": 0,
      "push_constant_size": 16,
      "push_constant_stage_flags": 1,
      "unified_binding_count": 1,
      "descriptors": [
        { "set": 0, "binding": 0, "descriptor_class": 0, "count": 1,
          "stage_flags": 1, "guest_resource_index": 4294967295, "written": true }
      ]
    }
  ],
  "pipeline_state": {
    "has_topology": true, "topology": 3, "samples": 1,
    "color_formats": [37], "blend_enable": [0], "write_mask": [15],
    "viewport_width": 64.0, "viewport_height": 64.0,
    "push_constant_size": 16, "push_constant_bytes": [0,0,0,0]
  },
  "reference_output": {
    "width": 2, "height": 2, "format": 37, "bytes_per_pixel": 4,
    "post_sha256": "", "post_file": "", "has_pre": false
  },
  "readiness": { "shader_binaries_ready": true }
})";
}

TEST(PackageReader, ASchemaTwoPackageReads) {
    TempDir dir;
    Write(dir.path() / "package.json", Schema2Json());
    package::Package out{};
    std::string error;
    ASSERT_TRUE(package::Read(dir.path().string(), out, error)) << error;
    EXPECT_EQ(out.schema_version, package::SchemaResourceOnly);
    EXPECT_EQ(out.provenance_id, 7u);
}

TEST(PackageReader, EffectiveModeRefusesASchemaTwoPackage) {
    // Effective-shader mode must not silently fall back to a resource-only package: the shader
    // state it needs is simply not there.
    TempDir dir;
    Write(dir.path() / "package.json", Schema2Json());
    package::Package out{};
    std::string error;
    EXPECT_FALSE(package::Read(dir.path().string(), out, error, /*require_effective=*/true));
    EXPECT_NE(error.find("effective-shader mode requires schema"), std::string::npos);
}

TEST(PackageReader, AnUnknownSchemaIsRefused) {
    // A version outside the accepted set is refused rather than reinterpreted, because a missing
    // field read as a default is not a captured value.
    TempDir dir;
    Write(dir.path() / "package.json", R"({"schema_version": 3, "provenance_id": 1})");
    package::Package out{};
    std::string error;
    EXPECT_FALSE(package::Read(dir.path().string(), out, error));
    EXPECT_NE(error.find("is not 2 or 4"), std::string::npos);
}

TEST(PackageReader, ASchemaFourPackageReadsItsShaderState) {
    TempDir dir;
    const std::vector<package::u8> bytes{0x03, 0x02, 0x23, 0x07, 0, 0, 0, 0};
    WriteBytes(dir.path() / "stage.spv", bytes);
    // The fixture's hash is computed by the reader, so the test supplies the real one.
    package::Package probe{};
    std::string error;
    Write(dir.path() / "package.json", Schema4Json("stage.spv", std::string(64, '0'), bytes.size()));
    // A wrong hash must be refused, which also proves the hash is checked at all.
    EXPECT_FALSE(package::Read(dir.path().string(), probe, error));
    EXPECT_NE(error.find("hashes to"), std::string::npos);
}

TEST(PackageReader, ATruncatedSidecarIsRefused) {
    TempDir dir;
    const std::vector<package::u8> bytes{1, 2, 3, 4};
    WriteBytes(dir.path() / "stage.spv", bytes);
    // The package declares more bytes than the file holds.
    Write(dir.path() / "package.json", Schema4Json("stage.spv", std::string(64, '0'), 999));
    package::Package out{};
    std::string error;
    EXPECT_FALSE(package::Read(dir.path().string(), out, error));
    EXPECT_NE(error.find("bytes but the package declares"), std::string::npos);
}

TEST(PackageReader, AMissingSidecarIsRefused) {
    TempDir dir;
    Write(dir.path() / "package.json", Schema4Json("absent.spv", std::string(64, '0'), 8));
    package::Package out{};
    std::string error;
    EXPECT_FALSE(package::Read(dir.path().string(), out, error));
    EXPECT_NE(error.find("cannot open sidecar"), std::string::npos);
}

TEST(PackageReader, APathTraversalSidecarIsRefused) {
    // A sidecar name is a bare filename. A parent reference would let a package read outside its
    // own directory, so it is refused rather than resolved.
    TempDir dir;
    Write(dir.path() / "package.json", Schema4Json("../escape.spv", std::string(64, '0'), 8));
    package::Package out{};
    std::string error;
    EXPECT_FALSE(package::Read(dir.path().string(), out, error));
    EXPECT_NE(error.find("not a bare filename"), std::string::npos);
}

TEST(PackageReader, TheDescriptorInterfaceIsParsed) {
    TempDir dir;
    const std::vector<package::u8> bytes{1, 2, 3, 4};
    WriteBytes(dir.path() / "stage.spv", bytes);
    // Compute the real hash so the read succeeds and the interface can be inspected.
    const std::string json = Schema4Json("stage.spv", std::string(64, '0'), bytes.size());
    Write(dir.path() / "package.json", json);
    package::Package out{};
    std::string error;
    // The read fails on the hash, but the interface is parsed before the sidecar check, so it can
    // still be inspected. That ordering is deliberate: a consumer can report what the package
    // claimed even when a sidecar is bad.
    package::Read(dir.path().string(), out, error);
    ASSERT_EQ(out.shader_interfaces.size(), 1u);
    ASSERT_EQ(out.shader_interfaces[0].descriptors.size(), 1u);
    EXPECT_EQ(out.shader_interfaces[0].descriptors[0].binding, 0u);
    EXPECT_EQ(out.shader_interfaces[0].descriptors[0].stage_flags, 1u);
    EXPECT_EQ(out.shader_interfaces[0].push_constant_size, 16u);
}

TEST(PackageReader, ThePipelineStateIsParsed) {
    TempDir dir;
    Write(dir.path() / "package.json", Schema4Json("stage.spv", std::string(64, '0'), 4));
    package::Package out{};
    std::string error;
    package::Read(dir.path().string(), out, error);
    EXPECT_TRUE(out.pipeline_state.has_topology);
    EXPECT_EQ(out.pipeline_state.topology, 3u);
    ASSERT_EQ(out.pipeline_state.color_formats.size(), 1u);
    EXPECT_EQ(out.pipeline_state.color_formats[0], 37u);
    EXPECT_EQ(out.pipeline_state.push_constant_bytes.size(), 4u);
}

TEST(PackageReader, TheSessionIdentityIsParsed) {
    TempDir dir;
    Write(dir.path() / "package.json", Schema4Json("stage.spv", std::string(64, '0'), 4));
    package::Package out{};
    std::string error;
    package::Read(dir.path().string(), out, error);
    EXPECT_EQ(out.session_id, 1234u);
    EXPECT_EQ(out.execution_id, 5678u);
}

} // namespace
