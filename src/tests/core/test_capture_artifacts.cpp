// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the structural capture-artifact readers.
//
// The defect these exist to prevent: the finalizer searched the serialized package for the first
// "guest_base" and took it. The package writes vertex_bindings before index_binding, and both carry
// that field name, so the search returned a vertex-buffer address. That is a silent wrong answer.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/capture_artifacts.h"

namespace {

using Core::CaptureArtifacts::BridgeRecord;
using Core::CaptureArtifacts::FieldProblem;
using Core::CaptureArtifacts::PackageMetadata;
using Core::CaptureArtifacts::ReadBridgeRecords;
using Core::CaptureArtifacts::ReadPackageMetadata;
using Core::CaptureArtifacts::SelectBridgeRecord;
using Core::CaptureArtifacts::u64;

constexpr u64 kSession = 0x1111222233334444ull;
constexpr u64 kProvenance = 40;
// The decimal literals in the fixtures below are these values. They are written in decimal in the
// JSON so the fixture reads as a real serialized package rather than a hand-formatted one.
constexpr u64 kVertexBase = 0x20118D0000ull;   // 137733406720
constexpr u64 kIndexBase = 0x20118E0000ull;    // 137733472256

/// A temporary directory that removes itself.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("rhi02ir3_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::string Write(const std::string& name, const std::string& body) const {
        const auto path = path_ / name;
        std::ofstream out{path, std::ios::trunc};
        out << body;
        return path.string();
    }

private:
    std::filesystem::path path_;
};

/// A package with a vertex binding whose guest_base differs from the index binding.
std::string PackageWithDistinctAddresses() {
    return R"({"schema_version": 3, "provenance_id": 40, "session_id": 1229801703532086340,
"vertex_bindings": [{"binding": 0, "guest_base": 137733406720, "guest_size": 36},
                    {"binding": 1, "guest_base": 137733406800, "guest_size": 36}],
"index_binding": {"guest_base": 137733472256, "guest_size": 12, "index_size": 2,
                  "index_count": 6}})";
}

// ---------------------------------------------------------------------------------------------
// The index address is read from index_binding, not from the first guest_base in the file
// ---------------------------------------------------------------------------------------------

TEST(CaptureArtifacts, TheIndexAddressIsNotTheFirstVertexAddress) {
    TempDir dir;
    const auto path = dir.Write("package.json", PackageWithDistinctAddresses());
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadPackageMetadata(path, package, problem, error)) << error;

    EXPECT_EQ(package.index.guest_base, kIndexBase);
    EXPECT_NE(package.index.guest_base, kVertexBase);
    // The vertex address is available for the negative control, but is never used for identity.
    EXPECT_EQ(package.first_vertex_guest_base, kVertexBase);
}

TEST(CaptureArtifacts, MultipleVertexBindingsDoNotConfuseTheIndexAddress) {
    TempDir dir;
    const auto path = dir.Write("package.json", PackageWithDistinctAddresses());
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadPackageMetadata(path, package, problem, error)) << error;
    EXPECT_EQ(package.index.guest_base, kIndexBase);
}

TEST(CaptureArtifacts, AMissingIndexBindingIsReported) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40,
                                   "session_id": 1, "vertex_bindings": [{"guest_base": 5}]})");
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadPackageMetadata(path, package, problem, error)) << error;
    EXPECT_FALSE(package.index.present);
    EXPECT_FALSE(package.index.has_guest_base);
}

TEST(CaptureArtifacts, AZeroIndexAddressIsPresentButZero) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40, "session_id": 1,
                                   "index_binding": {"guest_base": 0}})") ;
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadPackageMetadata(path, package, problem, error)) << error;
    EXPECT_TRUE(package.index.present);
    EXPECT_TRUE(package.index.has_guest_base);
    EXPECT_EQ(package.index.guest_base, 0u);
}

TEST(CaptureArtifacts, AMalformedIndexBindingIsRefused) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40, "session_id": 1,
                                   "index_binding": {"guest_base": "not a number"}})") ;
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    EXPECT_FALSE(ReadPackageMetadata(path, package, problem, error));
    EXPECT_EQ(problem, FieldProblem::WrongType);
}

TEST(CaptureArtifacts, ANegativeIndexAddressIsRefused) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40, "session_id": 1,
                                   "index_binding": {"guest_base": -1}})") ;
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    EXPECT_FALSE(ReadPackageMetadata(path, package, problem, error));
    EXPECT_EQ(problem, FieldProblem::MalformedNumber);
}

TEST(CaptureArtifacts, AnOverflowingIndexAddressIsRefused) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40, "session_id": 1,
                                   "index_binding": {"guest_base": 99999999999999999999999}})") ;
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    EXPECT_FALSE(ReadPackageMetadata(path, package, problem, error));
}

TEST(CaptureArtifacts, ADuplicateIdentityKeyIsRefused) {
    TempDir dir;
    const auto path = dir.Write("package.json",
                                R"({"schema_version": 3, "provenance_id": 40, "provenance_id": 41,
                                   "session_id": 1, "index_binding": {"guest_base": 5}})") ;
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    EXPECT_FALSE(ReadPackageMetadata(path, package, problem, error));
    EXPECT_EQ(problem, FieldProblem::DuplicateKey);
}

TEST(CaptureArtifacts, ATruncatedPackageIsRefused) {
    TempDir dir;
    const auto path = dir.Write("package.json", R"({"schema_version": 3, "provenance_id")");
    PackageMetadata package{};
    FieldProblem problem{};
    std::string error;
    EXPECT_FALSE(ReadPackageMetadata(path, package, problem, error));
    EXPECT_EQ(problem, FieldProblem::Unreadable);
}

// ---------------------------------------------------------------------------------------------
// Bridge record selection
// ---------------------------------------------------------------------------------------------

std::string BridgeWithRecords() {
    return R"({"schema_version": 1, "records": [
{"provenance_id": 58, "session_id": 1229801703532086340, "selected_guest_address": 137733406720,
 "original_rhi_object": 4311744232, "predicate_determinable": true},
{"provenance_id": 40, "session_id": 1229801703532086340, "selected_guest_address": 137733472256,
 "original_rhi_object": 4311744000, "predicate_determinable": true},
{"provenance_id": 77, "session_id": 1229801703532086340, "selected_guest_address": 137733406800,
 "original_rhi_object": 4311744100, "predicate_determinable": true}]})";
}

TEST(CaptureArtifacts, TheSelectedBridgeRecordIsNotTheFirst) {
    TempDir dir;
    const auto path = dir.Write("index_bridge.json", BridgeWithRecords());
    std::vector<BridgeRecord> records;
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadBridgeRecords(path, records, problem, error)) << error;
    ASSERT_EQ(records.size(), 3u);

    BridgeRecord selected{};
    bool ambiguous = false;
    ASSERT_TRUE(SelectBridgeRecord(records, kSession, kProvenance, selected, ambiguous, error))
        << error;
    EXPECT_FALSE(ambiguous);
    EXPECT_EQ(selected.provenance_id, kProvenance);
    EXPECT_EQ(selected.selected_guest_address, kIndexBase);
    // The first record belongs to a different draw and must not have been chosen.
    EXPECT_NE(selected.provenance_id, records[0].provenance_id);
}

TEST(CaptureArtifacts, DuplicateMatchingBridgeRecordsAreAmbiguous) {
    TempDir dir;
    const auto path = dir.Write("index_bridge.json",
                                R"({"schema_version": 1, "records": [
{"provenance_id": 40, "session_id": 1, "selected_guest_address": 5},
{"provenance_id": 40, "session_id": 1, "selected_guest_address": 6}]})") ;
    std::vector<BridgeRecord> records;
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadBridgeRecords(path, records, problem, error)) << error;
    BridgeRecord selected{};
    bool ambiguous = false;
    EXPECT_FALSE(SelectBridgeRecord(records, 1, 40, selected, ambiguous, error));
    EXPECT_TRUE(ambiguous);
}

TEST(CaptureArtifacts, ARecordFromAnotherSessionIsNotSelected) {
    TempDir dir;
    const auto path = dir.Write("index_bridge.json",
                                R"({"schema_version": 1, "records": [
{"provenance_id": 40, "session_id": 999, "selected_guest_address": 5}]})") ;
    std::vector<BridgeRecord> records;
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadBridgeRecords(path, records, problem, error)) << error;
    BridgeRecord selected{};
    bool ambiguous = false;
    EXPECT_FALSE(SelectBridgeRecord(records, kSession, 40, selected, ambiguous, error));
    EXPECT_FALSE(ambiguous);
}

TEST(CaptureArtifacts, AMissingBridgeRecordIsReported) {
    TempDir dir;
    const auto path = dir.Write("index_bridge.json",
                                R"({"schema_version": 1, "records": [
{"provenance_id": 58, "session_id": 1, "selected_guest_address": 5}]})") ;
    std::vector<BridgeRecord> records;
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadBridgeRecords(path, records, problem, error)) << error;
    BridgeRecord selected{};
    bool ambiguous = false;
    EXPECT_FALSE(SelectBridgeRecord(records, 1, 40, selected, ambiguous, error));
    EXPECT_FALSE(ambiguous);
}

TEST(CaptureArtifacts, AnEmptyBridgeIsReported) {
    TempDir dir;
    const auto path = dir.Write("index_bridge.json", R"({"schema_version": 1, "records": []})");
    std::vector<BridgeRecord> records;
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadBridgeRecords(path, records, problem, error)) << error;
    EXPECT_TRUE(records.empty());
}

// ---------------------------------------------------------------------------------------------
// Negative control: the old global search selects the wrong value
// ---------------------------------------------------------------------------------------------

/// The previous finalizer's algorithm: find the first "guest_base" in the file and read its number.
///
/// This is reproduced here only to demonstrate that it returns the vertex address. It is not used
/// by production code.
u64 LegacyFirstGuestBase(const std::string& text) {
    const std::string needle = "\"guest_base\":";
    const auto at = text.find(needle);
    if (at == std::string::npos) {
        return 0;
    }
    std::size_t i = at + needle.size();
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
    }
    u64 value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + static_cast<u64>(text[i] - '0');
        ++i;
    }
    return value;
}

TEST(CaptureArtifacts, TheLegacyGlobalSearchSelectsTheVertexAddress) {
    // The negative control. The old parser returns the vertex address, which is why the structural
    // reader exists. If this test ever fails, the fixture no longer reproduces the original defect
    // and the regression coverage has been lost.
    const std::string package = PackageWithDistinctAddresses();
    const u64 legacy = LegacyFirstGuestBase(package);
    EXPECT_EQ(legacy, kVertexBase);
    EXPECT_NE(legacy, kIndexBase);

    // The structural reader returns the index address from the same document.
    TempDir dir;
    const auto path = dir.Write("package.json", package);
    PackageMetadata metadata{};
    FieldProblem problem{};
    std::string error;
    ASSERT_TRUE(ReadPackageMetadata(path, metadata, problem, error)) << error;
    EXPECT_EQ(metadata.index.guest_base, kIndexBase);
    EXPECT_NE(metadata.index.guest_base, legacy);
}

TEST(CaptureArtifacts, EveryProblemHasAStableName) {
    for (const auto problem :
         {FieldProblem::None, FieldProblem::Unreadable, FieldProblem::MissingObject,
          FieldProblem::MissingField, FieldProblem::WrongType, FieldProblem::MalformedNumber,
          FieldProblem::DuplicateKey, FieldProblem::SchemaMismatch}) {
        EXPECT_NE(Core::CaptureArtifacts::FieldProblemName(problem), "unknown");
    }
}

} // namespace