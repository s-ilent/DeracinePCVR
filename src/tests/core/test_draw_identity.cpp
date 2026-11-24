// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the same-draw identity join.
//
// The defect these exist to prevent: two independently valid captures being treated as one
// integrated draw because both are valid. Every test here either establishes the join from a real
// identity field or refuses it.

#include <gtest/gtest.h>

#include "draw_identity.h"

namespace {

using tape::JoinOutcome;
using tape::JoinTapeToPackage;
using tape::PackageIdentity;
using tape::TapeIdentity;
using tape::u32;

/// The observed classroom draw, as both captures describe it.
TapeIdentity MakeTape() {
    TapeIdentity identity{};
    identity.provenance_id = 42;
    identity.index_guest_base = 0x2015e40000ull;
    identity.index_guest_size = 6072;
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
    identity.index_guest_base = 0x2015e40000ull;
    identity.index_guest_size = 6072;
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

TEST(DrawIdentity, TheSameDrawJoins) {
    const auto result = JoinTapeToPackage(MakeTape(), MakePackage());
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
}

TEST(DrawIdentity, ADifferentProvenanceIsRejected) {
    // The strongest identity: the same record id from the same execution. A disagreement means the
    // captures are about different draws, and no other field can repair that.
    auto package = MakePackage();
    package.provenance_id = 43;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
    EXPECT_NE(result.reason.find("different draws"), std::string::npos);
}

TEST(DrawIdentity, ADifferentIndexBufferIsRejected) {
    // Two draws can be otherwise identical and still consume different index buffers. This is the
    // case a size-only comparison would miss.
    auto package = MakePackage();
    package.index_guest_base = 0x2015e80000ull;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
    EXPECT_NE(result.reason.find("different index buffers"), std::string::npos);
}

TEST(DrawIdentity, ADifferentIndexExtentIsRejected) {
    auto package = MakePackage();
    package.index_guest_size = 4096;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, ADifferentElementWidthIsRejected) {
    // The width is a property of the resource, so a disagreement means one side is describing a
    // different buffer even when the addresses happen to match.
    auto package = MakePackage();
    package.index_element_size = 4;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
    EXPECT_NE(result.reason.find("element width"), std::string::npos);
}

TEST(DrawIdentity, ADifferentIndexCountIsRejected) {
    auto package = MakePackage();
    package.index_count = 1500;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, ADifferentStartIndexIsRejected) {
    auto package = MakePackage();
    package.first_index = 168;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, ANegativeBaseVertexIsComparedAsSigned) {
    // A negative base vertex cast through u32 would become a huge positive number and appear to
    // disagree with itself. It is compared as signed on both sides.
    auto tape = MakeTape();
    tape.base_vertex_index = -7;
    auto package = MakePackage();
    package.base_vertex = static_cast<u32>(-7);
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
}

TEST(DrawIdentity, ADifferentBaseVertexIsRejected) {
    auto tape = MakeTape();
    tape.base_vertex_index = -7;
    auto package = MakePackage();
    package.base_vertex = 0;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, ADifferentInstanceCountIsRejected) {
    auto package = MakePackage();
    package.instance_count = 2;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, AgreementOnDerivedFieldsAloneIsNotEnough) {
    // Every derived value agrees, but the index buffer is unknown on both sides. Two different
    // draws can share every derived value, so this must not be reported as a join.
    auto tape = MakeTape();
    tape.index_guest_base = 0;
    tape.index_guest_size = 0;
    auto package = MakePackage();
    package.index_guest_base = 0;
    package.index_guest_size = 0;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence);
    EXPECT_NE(result.reason.find("no identity field"), std::string::npos);
}

TEST(DrawIdentity, AMissingProvenanceDoesNotFailTheJoin) {
    // A capture that did not record a provenance id can still be joined on the index resource,
    // which is a real identity field. The absent field is reported as not comparable rather than
    // as agreeing.
    auto tape = MakeTape();
    tape.provenance_id = 0;
    auto package = MakePackage();
    package.provenance_id = 0;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    bool found = false;
    for (const auto& field : result.fields) {
        if (field.name == "provenance_id") {
            found = true;
            EXPECT_FALSE(field.comparable);
            EXPECT_FALSE(field.agrees);
        }
    }
    EXPECT_TRUE(found);
}

TEST(DrawIdentity, EveryFieldIsReported) {
    // The report is what a reader uses to judge the join, so every field examined must appear.
    const auto result = JoinTapeToPackage(MakeTape(), MakePackage());
    const std::vector<std::string_view> expected{
        "provenance_id", "index_guest_base", "index_guest_size", "index_element_size",
        "index_count",   "first_index",       "base_vertex",      "instance_count",
        "first_instance"};
    for (const auto name : expected) {
        bool found = false;
        for (const auto& field : result.fields) {
            if (field.name == name) {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "missing field " << name;
    }
}

TEST(DrawIdentity, AReusedIndexBufferWithADifferentDrawIsRejected) {
    // The same index buffer can serve several draws. The index resource agrees, but the draw
    // arguments do not, so the join must fail rather than accept the shared buffer as identity.
    auto package = MakePackage();
    package.index_count = 1500;
    package.first_index = 336;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, AMismatchedCommandListIsRejected) {
    // The command list is not carried by the package, so it cannot be compared. The join still
    // succeeds on the index resource, and the absent field is reported as not comparable rather
    // than silently treated as agreeing.
    const auto result = JoinTapeToPackage(MakeTape(), MakePackage());
    ASSERT_EQ(result.outcome, JoinOutcome::Joined);
    for (const auto& field : result.fields) {
        if (!field.comparable) {
            EXPECT_FALSE(field.agrees) << field.name << " claimed agreement without comparison";
        }
    }
}

} // namespace
