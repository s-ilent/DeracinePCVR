// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the same-draw identity join.
//
// The defect these exist to prevent: two independently valid captures being treated as one
// integrated draw because both are valid. Every test here either establishes the join from a real
// identity field or refuses it.

#include <algorithm>

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
    identity.index_domain = tape::IndexDomain::GuestAddress;
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

TEST(DrawIdentity, ADifferentIndexCountIsReportedAsInconsistentNotMismatched) {
    // A semantic mutation changes the draw arguments on purpose while keeping the resource. That is
    // a legitimate execution, not a mismatched capture, so the join still succeeds and the
    // disagreement is reported separately.
    auto package = MakePackage();
    package.index_count = 1500;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_FALSE(result.derived_consistent);
    EXPECT_NE(std::find(result.derived_disagreements.begin(), result.derived_disagreements.end(),
                        "index_count"),
              result.derived_disagreements.end());
}

TEST(DrawIdentity, ADifferentStartIndexIsReportedAsInconsistent) {
    auto package = MakePackage();
    package.first_index = 168;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_FALSE(result.derived_consistent);
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

TEST(DrawIdentity, ADifferentBaseVertexIsReportedAsInconsistent) {
    auto tape = MakeTape();
    tape.base_vertex_index = -7;
    auto package = MakePackage();
    package.base_vertex = 0;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_FALSE(result.derived_consistent);
}

TEST(DrawIdentity, ADifferentInstanceCountIsReportedAsInconsistent) {
    auto package = MakePackage();
    package.instance_count = 2;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_FALSE(result.derived_consistent);
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

TEST(DrawIdentity, AReusedIndexBufferWithADifferentDrawIsReportedAsInconsistent) {
    // The same index buffer can serve several draws. The resource identity agrees, so the join
    // succeeds, but the draw arguments do not, and that is reported. A reader can then tell a
    // mutated or reused-buffer draw from an unmutated one without the join being refused.
    auto package = MakePackage();
    package.index_count = 1500;
    package.first_index = 336;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    EXPECT_FALSE(result.derived_consistent);
    EXPECT_GE(result.derived_disagreements.size(), 2u);
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

TEST(DrawIdentity, AProvenanceMismatchIsDecisiveEvenWhenEverythingElseAgrees) {
    // Provenance is the same record id from the same execution. When it disagrees, no amount of
    // agreement elsewhere repairs it: the two captures observed different draws.
    auto package = MakePackage();
    package.provenance_id = 99;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
}

TEST(DrawIdentity, AnIncomparableFieldNeverClaimsAgreement) {
    // A field the package does not carry cannot be compared. Reporting it as agreeing would be a
    // fabricated match, which is the failure mode this whole module exists to prevent.
    auto package = MakePackage();
    package.has_index_binding = false;
    package.index_count = 0;
    package.first_index = 0;
    package.base_vertex = 0;
    package.instance_count = 0;
    package.first_instance = 0;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    for (const auto& field : result.fields) {
        if (!field.comparable) {
            EXPECT_FALSE(field.agrees) << field.name << " claimed agreement without comparison";
        }
    }
}

TEST(DrawIdentity, AZeroIndexResourceOnBothSidesIsNotAnIdentity) {
    // Two captures that both failed to record the index resource must not be joined on the
    // strength of two absences. Provenance is not an index identity: it says the two records came
    // from the same execution, not that they describe the same resource. The join therefore
    // reports insufficient evidence rather than treating two zeros as agreement.
    auto tape = MakeTape();
    tape.index_guest_base = 0;
    tape.index_guest_size = 0;
    auto package = MakePackage();
    package.index_guest_base = 0;
    package.index_guest_size = 0;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence);
    for (const auto& field : result.fields) {
        if (field.name == "index_guest_base") {
            EXPECT_FALSE(field.comparable);
            EXPECT_FALSE(field.agrees);
        }
    }
}

TEST(DrawIdentity, ADifferentElementWidthIsDecisive) {
    // The width is a property of the resource, so a disagreement means one side is describing a
    // different buffer even when the addresses happen to match.
    auto package = MakePackage();
    package.index_element_size = 4;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Mismatched);
    EXPECT_NE(result.reason.find("element width"), std::string::npos);
}

TEST(DrawIdentity, TheConsistencyReportNamesEveryDisagreement) {
    // A reader needs to know which fields disagreed, not merely that something did.
    auto package = MakePackage();
    package.index_count = 1500;
    package.first_index = 336;
    package.instance_count = 3;
    const auto result = JoinTapeToPackage(MakeTape(), package);
    EXPECT_EQ(result.outcome, JoinOutcome::Joined);
    EXPECT_FALSE(result.derived_consistent);
    EXPECT_GE(result.derived_disagreements.size(), 3u);
}

TEST(DrawIdentity, ACrossDomainIndexComparisonIsRefused) {
    // The tape records the RHI invocation's index-buffer argument, which is an original UE4 object
    // pointer. The package records the guest GPU address. Those are different namespaces, so their
    // numeric equality is not evidence of anything. This is the case a genuine capture will hit.
    auto tape = MakeTape();
    tape.index_domain = tape::IndexDomain::Ue4Object;
    auto package = MakePackage();
    package.index_domain = tape::IndexDomain::GuestAddress;
    // The numbers are made equal on purpose: a naive comparison would join them.
    tape.index_guest_base = package.index_guest_base;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence);
    EXPECT_NE(result.reason.find("different namespaces"), std::string::npos);
}

TEST(DrawIdentity, TheDomainsAreReportedEvenWhenTheyAgree) {
    const auto result = JoinTapeToPackage(MakeTape(), MakePackage());
    ASSERT_EQ(result.outcome, JoinOutcome::Joined) << result.reason;
    bool found = false;
    for (const auto& field : result.fields) {
        if (field.name == "index_domain") {
            found = true;
            EXPECT_TRUE(field.comparable);
            EXPECT_TRUE(field.agrees);
            EXPECT_EQ(field.tape_value, "guest_address");
        }
    }
    EXPECT_TRUE(found);
}

TEST(DrawIdentity, AnUnknownDomainCannotEstablishTheJoin) {
    // A capture that did not record which namespace its index value belongs to cannot be joined on
    // it. Reporting the absence is correct; guessing the domain would be a fabricated match.
    auto tape = MakeTape();
    tape.index_domain = tape::IndexDomain::Unknown;
    const auto result = JoinTapeToPackage(tape, MakePackage());
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence);
}

TEST(DrawIdentity, AHostBufferDomainIsNotAGuestAddress) {
    auto tape = MakeTape();
    tape.index_domain = tape::IndexDomain::HostBuffer;
    auto package = MakePackage();
    package.index_domain = tape::IndexDomain::GuestAddress;
    const auto result = JoinTapeToPackage(tape, package);
    EXPECT_EQ(result.outcome, JoinOutcome::InsufficientEvidence);
}

} // namespace
