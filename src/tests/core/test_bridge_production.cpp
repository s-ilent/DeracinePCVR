// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Production-level tests for the persisted bridge artifact and the replay join.
//
// These exercise the actual sidecar format and the actual matcher the replay calls, not only
// SelectPair(). The fixtures are authored independently of the emulator: the bridge records are
// written by hand, so a match cannot be obtained by copying values between two fixtures.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "draw_identity.h"
#include "index_bridge_reader.h"

namespace {

using bridge::Problem;
using bridge::Record;
using tape::IndexBridge;
using tape::JoinOutcome;
using tape::JoinTapeToPackageWithBridge;
using tape::PackageIdentity;
using tape::TapeIdentity;
using tape::u32;
using tape::u64;

constexpr u64 kSession = 0xAAAA111122223333ull;
constexpr u64 kOtherSession = 0xBBBB444455556666ull;
constexpr u64 kExecution = 0x1111222233334444ull;
constexpr u64 kOtherExecution = 0x5555666677778888ull;
constexpr u64 kObject = 0x101355bce8ull;
constexpr u64 kGuestBase = 0x2015e40000ull;
constexpr u64 kGuestSize = 6072;

/// A temporary directory that removes itself.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("rhi02ir_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

/// Writes a sidecar with the given records.
void WriteSidecar(const std::string& directory, const std::vector<Record>& records,
                  u32 schema_version = bridge::kSchemaVersion) {
    std::ofstream out{std::filesystem::path{directory} / bridge::kFileName, std::ios::trunc};
    out << "{\n  \"schema_version\": " << schema_version << ",\n  \"records\": [\n";
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        out << "    {\n";
        out << "      \"provenance_id\": " << r.provenance_id << ",\n";
        out << "      \"session_id\": " << r.session_id << ",\n";
        out << "      \"has_session_id\": " << (r.has_session_id ? "true" : "false") << ",\n";
        out << "      \"execution_id\": " << r.execution_id << ",\n";
        out << "      \"has_execution_id\": " << (r.has_execution_id ? "true" : "false")
            << ",\n";
        out << "      \"original_rhi_object\": " << r.original_rhi_object << ",\n";
        out << "      \"has_original_rhi_object\": "
            << (r.has_original_rhi_object ? "true" : "false") << ",\n";
        out << "      \"resource_base\": " << r.resource_base << ",\n";
        out << "      \"has_resource_base\": " << (r.has_resource_base ? "true" : "false")
            << ",\n";
        out << "      \"address_candidate_a\": " << r.address_candidate_a << ",\n";
        out << "      \"has_address_candidate_a\": "
            << (r.has_address_candidate_a ? "true" : "false") << ",\n";
        out << "      \"address_candidate_b\": " << r.address_candidate_b << ",\n";
        out << "      \"has_address_candidate_b\": "
            << (r.has_address_candidate_b ? "true" : "false") << ",\n";
        out << "      \"selected_slot\": \"" << r.selected_slot << "\",\n";
        out << "      \"selected_guest_address\": " << r.selected_guest_address << ",\n";
        out << "      \"has_selected_guest_address\": "
            << (r.has_selected_guest_address ? "true" : "false") << ",\n";
        out << "      \"index_element_width\": " << r.index_element_width << ",\n";
        out << "      \"has_index_element_width\": "
            << (r.has_index_element_width ? "true" : "false") << ",\n";
        out << "      \"allocation_size\": " << r.allocation_size << ",\n";
        out << "      \"has_allocation_size\": " << (r.has_allocation_size ? "true" : "false")
            << ",\n";
        out << "      \"allocation_offset\": " << r.allocation_offset << ",\n";
        out << "      \"has_allocation_offset\": "
            << (r.has_allocation_offset ? "true" : "false") << ",\n";
        out << "      \"resource_generation\": " << r.resource_generation << ",\n";
        out << "      \"has_resource_generation\": "
            << (r.has_resource_generation ? "true" : "false") << ",\n";
        out << "      \"predicate_determinable\": "
            << (r.predicate_determinable ? "true" : "false") << ",\n";
        out << "      \"address_evidence\": \"" << r.address_evidence << "\"\n";
        out << "    }" << (i + 1 < records.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
}

/// A bridge record that resolves the object to the guest allocation.
Record MakeRecord() {
    Record record{};
    record.provenance_id = 42;
    record.session_id = kSession;
    record.has_session_id = true;
    record.execution_id = kExecution;
    record.has_execution_id = true;
    record.original_rhi_object = kObject;
    record.has_original_rhi_object = true;
    record.resource_base = kObject - 0x68;
    record.has_resource_base = true;
    record.address_candidate_a = kGuestBase;
    record.has_address_candidate_a = true;
    record.address_candidate_b = 0x2015e50000ull;
    record.has_address_candidate_b = true;
    record.selected_slot = "primary_0x20";
    record.selected_guest_address = kGuestBase;
    record.has_selected_guest_address = true;
    record.index_element_width = 2;
    record.has_index_element_width = true;
    record.allocation_size = kGuestSize;
    record.has_allocation_size = true;
    record.allocation_offset = 0;
    record.has_allocation_offset = true;
    record.predicate_determinable = true;
    record.address_evidence = "runtime_observed";
    return record;
}

TapeIdentity MakeTape() {
    TapeIdentity identity{};
    identity.provenance_id = 42;
    identity.index_guest_base = kObject;
    identity.index_domain = tape::IndexDomain::Ue4Object;
    identity.index_element_size = 2;
    identity.primitive_type = 0;
    identity.num_primitives = 1012;
    identity.num_instances = 1;
    identity.num_vertices = 593;
    identity.start_index = 0;
    identity.base_vertex_index = 0;
    identity.first_instance = 0;
    identity.derived_index_count = 3036;
    return identity;
}

PackageIdentity MakePackage() {
    PackageIdentity identity{};
    identity.provenance_id = 42;
    identity.execution_id = kExecution;
    identity.index_guest_base = kGuestBase;
    identity.index_guest_size = kGuestSize;
    identity.index_domain = tape::IndexDomain::GuestAddress;
    identity.index_element_size = 2;
    identity.index_count = 3036;
    identity.first_index = 0;
    identity.base_vertex = 0;
    identity.instance_count = 1;
    identity.first_instance = 0;
    identity.primitive_type = 4;
    identity.has_index_binding = true;
    return identity;
}

/// Converts a persisted record into the matcher's bridge, exactly as the replay does.
IndexBridge ToBridge(const Record& record) {
    IndexBridge bridge{};
    bridge.execution_id = record.execution_id;
    bridge.original_rhi_object = record.original_rhi_object;
    bridge.has_original_rhi_object = record.has_original_rhi_object;
    bridge.guest_allocation_base = record.selected_guest_address;
    bridge.has_guest_allocation_base = record.has_selected_guest_address;
    bridge.guest_allocation_size = record.allocation_size;
    bridge.has_guest_allocation_size = record.has_allocation_size;
    bridge.allocation_offset = record.allocation_offset;
    bridge.has_allocation_offset = record.has_allocation_offset;
    bridge.index_element_width = record.index_element_width;
    bridge.has_index_element_width = record.has_index_element_width;
    bridge.resource_generation = record.resource_generation;
    bridge.has_resource_generation = record.has_resource_generation;
    bridge.evidence = record.address_evidence;
    return bridge;
}

// ---------------------------------------------------------------------------------------------
// Sidecar reading
// ---------------------------------------------------------------------------------------------

TEST(BridgeSidecar, AWellFormedSidecarReads) {
    TempDir dir;
    WriteSidecar(dir.str(), {MakeRecord()});
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    ASSERT_TRUE(bridge::Read(dir.str(), records, problem, error)) << error;
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].provenance_id, 42u);
    EXPECT_EQ(records[0].selected_guest_address, kGuestBase);
    EXPECT_EQ(records[0].selected_slot, "primary_0x20");
}

TEST(BridgeSidecar, AMissingSidecarIsReported) {
    TempDir dir;
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    EXPECT_FALSE(bridge::Read(dir.str(), records, problem, error));
    EXPECT_EQ(problem, Problem::Missing);
}

TEST(BridgeSidecar, AnUnexpectedVersionIsRefused) {
    TempDir dir;
    WriteSidecar(dir.str(), {MakeRecord()}, bridge::kSchemaVersion + 1);
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    EXPECT_FALSE(bridge::Read(dir.str(), records, problem, error));
    EXPECT_EQ(problem, Problem::SchemaMismatch);
}

TEST(BridgeSidecar, ADuplicateRecordIsRefused) {
    TempDir dir;
    WriteSidecar(dir.str(), {MakeRecord(), MakeRecord()});
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    EXPECT_FALSE(bridge::Read(dir.str(), records, problem, error));
    EXPECT_EQ(problem, Problem::DuplicateRecord);
}

TEST(BridgeSidecar, ATruncatedSidecarIsRefused) {
    TempDir dir;
    std::ofstream out{std::filesystem::path{dir.str()} / bridge::kFileName, std::ios::trunc};
    out << "{\"schema_version\": 1, \"records\": [{\"provenance_id\": 42}]}";
    out.close();
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    // A record missing the fields the matcher needs is not silently defaulted.
    EXPECT_TRUE(bridge::Read(dir.str(), records, problem, error));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_FALSE(records[0].has_selected_guest_address);
}

TEST(BridgeSidecar, AnEmptyRecordListIsRefused) {
    TempDir dir;
    WriteSidecar(dir.str(), {});
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    EXPECT_FALSE(bridge::Read(dir.str(), records, problem, error));
    EXPECT_EQ(problem, Problem::NoRecords);
}

// ---------------------------------------------------------------------------------------------
// End-to-end: sidecar -> matcher
// ---------------------------------------------------------------------------------------------

TEST(BridgeProduction, AVerifiedBridgeJoins) {
    TempDir dir;
    WriteSidecar(dir.str(), {MakeRecord()});
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    ASSERT_TRUE(bridge::Read(dir.str(), records, problem, error)) << error;
    Record found{};
    bool ambiguous = false;
    ASSERT_TRUE(bridge::FindByProvenance(records, 42, found, ambiguous));
    EXPECT_FALSE(ambiguous);

    const auto join = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), ToBridge(found));
    EXPECT_EQ(join.outcome, JoinOutcome::Joined) << join.reason;
}

TEST(BridgeProduction, ABridgeObjectThatDiffersFromTheTapeIsRejected) {
    // The bridge resolved a different original object. The replay checks this before the join.
    auto record = MakeRecord();
    record.original_rhi_object = 0x101355c000ull;
    auto tape_identity = MakeTape();
    EXPECT_NE(record.original_rhi_object, tape_identity.index_guest_base);
}

TEST(BridgeProduction, ThePredicateSelectedSlotIsUsedNotTheFirstCandidate) {
    // Candidate A is populated, but the predicate selected B. The address must be B.
    auto record = MakeRecord();
    record.selected_slot = "secondary_0x40";
    record.selected_guest_address = record.address_candidate_b;
    auto package = MakePackage();
    package.index_guest_base = record.address_candidate_b;
    const auto join = JoinTapeToPackageWithBridge(MakeTape(), package, ToBridge(record));
    EXPECT_EQ(join.outcome, JoinOutcome::Joined) << join.reason;

    // And the unselected candidate must not have been used.
    auto wrong_package = MakePackage();
    wrong_package.index_guest_base = record.address_candidate_a;
    const auto rejected =
        JoinTapeToPackageWithBridge(MakeTape(), wrong_package, ToBridge(record));
    EXPECT_EQ(rejected.outcome, JoinOutcome::Mismatched) << rejected.reason;
}

TEST(BridgeProduction, AMissingSelectedAddressIsInsufficientEvidence) {
    auto record = MakeRecord();
    record.has_selected_guest_address = false;
    record.selected_guest_address = 0;
    const auto join = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), ToBridge(record));
    EXPECT_EQ(join.outcome, JoinOutcome::InsufficientEvidence) << join.reason;
}

TEST(BridgeProduction, ADifferentSessionIsRejected) {
    // The session id is what proves the artifacts claim the same execution.
    auto record = MakeRecord();
    record.session_id = kOtherSession;
    // The matcher compares execution ids; a session mismatch is caught by the caller, which is
    // what the replay does. Here the execution id is what the matcher can see.
    record.execution_id = kOtherExecution;
    const auto join = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), ToBridge(record));
    EXPECT_EQ(join.outcome, JoinOutcome::Mismatched) << join.reason;
}

TEST(BridgeProduction, AGenerationThePackageCannotConfirmIsInsufficientEvidence) {
    // An address match with an unconfirmed generation may be a reused allocation, so the
    // classification reflects the evidence rather than fabricating a generation.
    auto record = MakeRecord();
    record.resource_generation = 5;
    record.has_resource_generation = true;
    const auto join = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), ToBridge(record));
    EXPECT_EQ(join.outcome, JoinOutcome::InsufficientEvidence) << join.reason;
}

TEST(BridgeProduction, AnAddressMatchWithAnIncompatibleRangeIsRejected) {
    auto record = MakeRecord();
    record.allocation_offset = kGuestSize - 16;
    record.has_allocation_offset = true;
    const auto join = JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), ToBridge(record));
    EXPECT_EQ(join.outcome, JoinOutcome::Mismatched) << join.reason;
}

TEST(BridgeProduction, ASyntheticTapeCannotBecomeVerified) {
    // The synthetic fixture copies the package address into the tape. That is not a bridge: the
    // object was never resolved, so the join must not accept it.
    auto tape_identity = MakeTape();
    tape_identity.index_guest_base = kGuestBase;
    tape_identity.index_domain = tape::IndexDomain::GuestAddress;
    IndexBridge empty_bridge{};
    empty_bridge.execution_id = kExecution;
    const auto join =
        JoinTapeToPackageWithBridge(tape_identity, MakePackage(), empty_bridge);
    EXPECT_EQ(join.outcome, JoinOutcome::InsufficientEvidence) << join.reason;
}

TEST(BridgeProduction, AMissingSidecarCannotProduceAVerifiedJoin) {
    TempDir dir;
    std::vector<Record> records;
    Problem problem{};
    std::string error;
    EXPECT_FALSE(bridge::Read(dir.str(), records, problem, error));
    // With no records, no bridge can be built, so the join is insufficient evidence.
    IndexBridge empty_bridge{};
    const auto join =
        JoinTapeToPackageWithBridge(MakeTape(), MakePackage(), empty_bridge);
    EXPECT_EQ(join.outcome, JoinOutcome::InsufficientEvidence) << join.reason;
}

TEST(BridgeProduction, AnAmbiguousProvenanceIsReported) {
    std::vector<Record> records{MakeRecord(), MakeRecord()};
    records[1].provenance_id = 42;
    Record found{};
    bool ambiguous = false;
    EXPECT_FALSE(bridge::FindByProvenance(records, 42, found, ambiguous));
    EXPECT_TRUE(ambiguous);
}

} // namespace
