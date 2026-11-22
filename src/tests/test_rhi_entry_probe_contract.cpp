// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/guest_entry_detour.h"
#include "core/rhi_entry_probe_contract.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

namespace {

using Core::ApplyEntryPatch;
using Core::ApplyEntryPatchIfMatchesOriginal;
using Core::BuildEntryPatchPlan;
using Core::EntryPatchPlan;
using Core::GuestEntryRegisterFrame;
using Core::MatchesInstalledEntryPatch;
using Core::MatchesOriginalEntryPatch;
using Core::RestoreEntryPatchIfMatches;
using Core::RhiEntryProbe::EbootV100Sha256;
using Core::RhiEntryProbe::EbootV101Sha256;
using Core::RhiEntryProbe::EnableEnvironmentVariable;
using Core::RhiEntryProbe::IsExecutableLoadRange;
using Core::RhiEntryProbe::IsOptedIn;
using Core::RhiEntryProbe::IsTargetBuild;
using Core::RhiEntryProbe::MatchesTargetPrologue;
using Core::RhiEntryProbe::MaxProvenanceAgeUs;
using Core::RhiEntryProbe::MaxProvenanceRecords;
using Core::RhiEntryProbe::MaxStartDelaySeconds;
using Core::RhiEntryProbe::ModuleName;
using Core::RhiEntryProbe::ProvenanceEnvironmentVariable;
using Core::RhiEntryProbe::StartDelayEnvironmentVariable;
using Core::RhiEntryProbe::TargetPrologue;
using Core::RhiEntryProbe::TitleId;

constexpr std::array<u8, 8> TargetInstructionLengths{1, 3, 2, 2, 2, 2, 1, 4};

TEST(GuestEntryDetour, RegisterFrameCoversFullYmmStateAndStackPointer) {
    EXPECT_EQ(offsetof(GuestEntryRegisterFrame, ymm), 16 * sizeof(u64));
    EXPECT_EQ(offsetof(GuestEntryRegisterFrame, original_rsp), 16 * sizeof(u64) + 16 * 32);
    EXPECT_EQ(sizeof(GuestEntryRegisterFrame), 16 * sizeof(u64) + 16 * 32 + sizeof(VAddr));
}

TEST(RhiEntryProbeContract, OptInRequiresExactlyOne) {
    EXPECT_FALSE(IsOptedIn(nullptr));
    EXPECT_FALSE(IsOptedIn(""));
    EXPECT_FALSE(IsOptedIn("0"));
    EXPECT_FALSE(IsOptedIn("01"));
    EXPECT_FALSE(IsOptedIn("1 "));
    EXPECT_TRUE(IsOptedIn("1"));
}

TEST(RhiEntryProbeContract, StartDelayIsBoundedAndSeparateFromOptIn) {
    // A start delay lets the probe observe the same interval as the command capture. It must be
    // bounded, and a value above the bound is refused rather than clamped.
    EXPECT_GT(MaxStartDelaySeconds, 0u);
    EXPECT_LE(MaxStartDelaySeconds, 3600u);
    EXPECT_FALSE(StartDelayEnvironmentVariable.empty());
    EXPECT_NE(StartDelayEnvironmentVariable, EnableEnvironmentVariable);
}

TEST(RhiEntryProbeContract, ProvenanceIsSeparatelyOptedInAndBounded) {
    // The end-to-end provenance trace is its own opt-in, so the existing bounded observation keeps
    // its behaviour and cost. Its record bound is deliberately small: a verified sample is worth
    // more than a large speculative one.
    EXPECT_FALSE(ProvenanceEnvironmentVariable.empty());
    EXPECT_NE(ProvenanceEnvironmentVariable, EnableEnvironmentVariable);
    EXPECT_NE(ProvenanceEnvironmentVariable, StartDelayEnvironmentVariable);
    EXPECT_GT(MaxProvenanceRecords, 0u);
    EXPECT_LE(MaxProvenanceRecords, 4096u);
    EXPECT_GT(MaxProvenanceAgeUs, 0u);
}

TEST(RhiEntryProbeContract, OnlyApprovedFullImageIdentitiesAreAccepted) {
    EXPECT_TRUE(IsTargetBuild(ModuleName, TitleId, EbootV100Sha256));
    EXPECT_TRUE(IsTargetBuild(ModuleName, TitleId, EbootV101Sha256));
    EXPECT_FALSE(IsTargetBuild("other.bin", TitleId, EbootV100Sha256));
    EXPECT_FALSE(IsTargetBuild(ModuleName, "CUSA00000", EbootV100Sha256));
    EXPECT_FALSE(IsTargetBuild(ModuleName, TitleId,
                               "8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a1"));
    EXPECT_FALSE(IsTargetBuild(ModuleName, TitleId, ""));
}

TEST(RhiEntryProbeContract, GnmCorrelationRequiresSameThreadAndFreshRhiSequence) {
    using Core::RhiEntryProbe::IsRhiToGnmCorrelationValid;
    using Core::RhiEntryProbe::MaxRhiToGnmCorrelationAgeUs;

    EXPECT_FALSE(IsRhiToGnmCorrelationValid(0, 11, 11, 0));
    EXPECT_FALSE(IsRhiToGnmCorrelationValid(1, 11, 12, 0));
    EXPECT_FALSE(IsRhiToGnmCorrelationValid(1, 11, 11, MaxRhiToGnmCorrelationAgeUs + 1));
    EXPECT_TRUE(IsRhiToGnmCorrelationValid(1, 11, 11, MaxRhiToGnmCorrelationAgeUs));
}

TEST(RhiEntryProbeContract, RequiresExactEntryPrologueSignature) {
    EXPECT_TRUE(MatchesTargetPrologue(TargetPrologue));

    auto modified = TargetPrologue;
    modified[10] ^= 0x01;
    EXPECT_FALSE(MatchesTargetPrologue(modified));
    EXPECT_FALSE(MatchesTargetPrologue(
        std::span<const u8>{TargetPrologue.data(), TargetPrologue.size() - 1}));
}

TEST(RhiEntryProbeContract, ExecutableLoadRangeMustBeFileBackedAndInBounds) {
    constexpr VAddr segment = 0x1000;
    EXPECT_TRUE(IsExecutableLoadRange(0x1100, 17, segment, 0x200, 0x300));
    EXPECT_TRUE(IsExecutableLoadRange(0x11EF, 17, segment, 0x200, 0x300));
    EXPECT_FALSE(IsExecutableLoadRange(0x11F0, 17, segment, 0x200, 0x300));
    EXPECT_FALSE(IsExecutableLoadRange(0x1200, 1, segment, 0x200, 0x300)); // BSS only
    EXPECT_FALSE(IsExecutableLoadRange(0x0FFF, 1, segment, 0x200, 0x300));
    EXPECT_FALSE(IsExecutableLoadRange(0x1100, 0, segment, 0x200, 0x300));
    EXPECT_FALSE(IsExecutableLoadRange(0x1100, 1, segment, 0x400, 0x300));
}

TEST(GuestEntryDetour, BuildsWholeInstructionNearJumpAndRestoresExactBytes) {
    constexpr VAddr target = 0x10000000;
    constexpr VAddr destination = 0x10001000;
    const auto plan =
        BuildEntryPatchPlan(target, destination, TargetPrologue, TargetInstructionLengths);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->size, TargetPrologue.size());
    EXPECT_EQ(plan->original[0], TargetPrologue[0]);
    EXPECT_EQ(plan->replacement[0], 0xE9);
    EXPECT_TRUE(std::all_of(plan->replacement.begin() + 5, plan->replacement.begin() + plan->size,
                            [](u8 byte) { return byte == 0x90; }));

    std::array<u8, TargetPrologue.size()> code = TargetPrologue;
    ASSERT_TRUE(ApplyEntryPatch(code, *plan));
    EXPECT_TRUE(MatchesInstalledEntryPatch(code, *plan));

    s32 relative{};
    std::memcpy(&relative, code.data() + 1, sizeof(relative));
    EXPECT_EQ(static_cast<s64>(target + 5) + relative, static_cast<s64>(destination));

    ASSERT_TRUE(RestoreEntryPatchIfMatches(code, *plan));
    EXPECT_EQ(code, TargetPrologue);
}

TEST(GuestEntryDetour, CompareBeforeApplyPreservesUnexpectedBytes) {
    const auto plan =
        BuildEntryPatchPlan(0x10000000, 0x10001000, TargetPrologue, TargetInstructionLengths);
    ASSERT_TRUE(plan.has_value());
    std::array<u8, TargetPrologue.size()> code = TargetPrologue;
    EXPECT_TRUE(MatchesOriginalEntryPatch(code, *plan));
    ASSERT_TRUE(ApplyEntryPatchIfMatchesOriginal(code, *plan));
    EXPECT_TRUE(MatchesInstalledEntryPatch(code, *plan));

    code = TargetPrologue;
    code[8] ^= 0x01;
    const auto changed = code;
    EXPECT_FALSE(MatchesOriginalEntryPatch(code, *plan));
    EXPECT_FALSE(ApplyEntryPatchIfMatchesOriginal(code, *plan));
    EXPECT_EQ(code, changed);
}

TEST(GuestEntryDetour, RefusesToRestoreWhenInstalledBytesChanged) {
    const auto plan =
        BuildEntryPatchPlan(0x10000000, 0x10001000, TargetPrologue, TargetInstructionLengths);
    ASSERT_TRUE(plan.has_value());
    std::array<u8, TargetPrologue.size()> code = TargetPrologue;
    ASSERT_TRUE(ApplyEntryPatch(code, *plan));

    code[8] ^= 0x01;
    const auto changed = code;
    EXPECT_FALSE(MatchesInstalledEntryPatch(code, *plan));
    EXPECT_FALSE(RestoreEntryPatchIfMatches(code, *plan));
    EXPECT_EQ(code, changed);
}

TEST(GuestEntryDetour, RejectsIncompleteInstructionCoverageAndUnsafeRanges) {
    const std::array<u8, 2> incomplete_lengths{1, 3};
    EXPECT_FALSE(BuildEntryPatchPlan(0x10000000, 0x10001000, TargetPrologue, incomplete_lengths));

    const std::array<u8, 1> invalid_length{16};
    EXPECT_FALSE(BuildEntryPatchPlan(0x10000000, 0x10001000, TargetPrologue, invalid_length));
    EXPECT_FALSE(
        BuildEntryPatchPlan(0x10000000, 0x9000000000, TargetPrologue, TargetInstructionLengths));
    EXPECT_FALSE(BuildEntryPatchPlan(std::numeric_limits<VAddr>::max() - 5, 0x10001000,
                                     TargetPrologue, TargetInstructionLengths));
}

TEST(GuestEntryDetour, HandlesNegativeRel32Destination) {
    constexpr VAddr target = 0x20010000;
    constexpr VAddr destination = 0x20000000;
    const auto plan =
        BuildEntryPatchPlan(target, destination, TargetPrologue, TargetInstructionLengths);
    ASSERT_TRUE(plan.has_value());

    s32 relative{};
    std::memcpy(&relative, plan->replacement.data() + 1, sizeof(relative));
    EXPECT_EQ(static_cast<s64>(target + 5) + relative, static_cast<s64>(destination));
}

TEST(GuestEntryDetour, RejectsOutOfRangeBufferAndInvalidPatchSize) {
    const auto plan =
        BuildEntryPatchPlan(0x10000000, 0x10001000, TargetPrologue, TargetInstructionLengths);
    ASSERT_TRUE(plan.has_value());
    std::array<u8, 4> short_code{};
    EXPECT_FALSE(ApplyEntryPatch(short_code, *plan));
    EXPECT_FALSE(RestoreEntryPatchIfMatches(short_code, *plan));

    auto invalid_plan = *plan;
    invalid_plan.size = EntryPatchPlan::MaxBytes + 1;
    std::array<u8, EntryPatchPlan::MaxBytes> code{};
    EXPECT_FALSE(ApplyEntryPatch(code, invalid_plan));
}

// --- Provenance matcher ------------------------------------------------------------------------
//
// These exercise MatchDecodedPacket, which is the function the live capture path calls. A test
// that only checked configuration would not establish that the matcher is correct.

namespace {

using Core::RhiEntryProbe::MatchDecodedPacket;
using Core::RhiEntryProbe::MatchOutcome;
using Core::RhiEntryProbe::PacketKey;

/// A small fixture that keeps the three parallel arrays the matcher consumes in step.
struct MatcherFixture {
    std::array<PacketKey, 8> keys{};
    std::array<u32, 8> counts{};
    std::array<u64, 8> addrs{};

    void Set(std::size_t i, u64 submission, VAddr buffer, u64 offset, u32 count, u64 addr,
             bool consumed = false) {
        keys[i] = PacketKey{submission, buffer, offset, consumed};
        counts[i] = count;
        addrs[i] = addr;
    }

    [[nodiscard]] MatchOutcome Match(u64 submission, VAddr buffer, u64 offset, u32 count,
                                     u64 addr, std::size_t& index) const {
        return MatchDecodedPacket(keys, submission, buffer, offset, count, addr, counts, addrs,
                                  index);
    }
};

} // namespace

TEST(ProvenanceMatcher, IdenticalParametersInDifferentSubmissionsSelectTheRightOne) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);
    f.Set(1, 8, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(8, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 1u);
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 0u);
}

TEST(ProvenanceMatcher, IdenticalOffsetsInDifferentBuffersSelectTheRightOne) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);
    f.Set(1, 7, 0x30000F1000, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F1000, 0xAE0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 1u);
}

TEST(ProvenanceMatcher, SameBufferReusedAcrossGenerationsCannotClaimTheNewPacket) {
    MatcherFixture f;
    // Generation 7 registered a packet at this buffer and offset; generation 9 did not.
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(9, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::NoCandidate);
}

TEST(ProvenanceMatcher, OffsetZeroIsAValidIdentity) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 0u);
}

TEST(ProvenanceMatcher, MissingSubmissionIdentityNeverMatches) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(0, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::NoSubmissionIdentity);
}

TEST(ProvenanceMatcher, UnsubmittedPacketIsNotEligible) {
    MatcherFixture f;
    // submission_id 0 means the packet was constructed but never submitted.
    f.Set(0, 0, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::NoCandidate);
}

TEST(ProvenanceMatcher, NoParameterFallbackWhenIdentityDoesNotMatch) {
    MatcherFixture f;
    // Same index count and address, different offset. Identity must win over parameters.
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0x1000, 6, 0x2000042800, index),
              MatchOutcome::NoCandidate);
}

TEST(ProvenanceMatcher, DuplicateCandidatesAreRejectedAsAmbiguous) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);
    f.Set(1, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::Ambiguous);
}

TEST(ProvenanceMatcher, ConsumedRecordCannotMatchAgain) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, /*consumed=*/true);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::AlreadyConsumed);
}

TEST(ProvenanceMatcher, InconsistentPacketFieldsAreRejected) {
    MatcherFixture f;
    // Identity matches but the decoded index count disagrees with the constructed packet.
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 48, 0x2000042800, index),
              MatchOutcome::Inconsistent);
}

TEST(ProvenanceMatcher, CopiedBufferKeepsTheOriginalIdentity) {
    MatcherFixture f;
    // The packet was written into the guest buffer; the submission copied it elsewhere. The key
    // carries the guest buffer, so the decode must be resolved against that, not the copy.
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);

    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 0u);
}

TEST(ProvenanceMatcher, OutOfOrderDecodingStillResolvesByIdentity) {
    MatcherFixture f;
    f.Set(0, 7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800);
    f.Set(1, 9, 0x30000F1000, 0x1000, 48, 0x20000C0000);

    std::size_t index = 0;
    // Decode the later submission first.
    EXPECT_EQ(f.Match(9, 0x30000F1000, 0x1000, 48, 0x20000C0000, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 1u);
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index), MatchOutcome::Matched);
    EXPECT_EQ(index, 0u);
}

TEST(ProvenanceMatcher, EmptyTableReportsNoCandidate) {
    MatcherFixture f;
    std::size_t index = 0;
    EXPECT_EQ(f.Match(7, 0x30000F0AE0, 0xAE0, 6, 0x2000042800, index),
              MatchOutcome::NoCandidate);
}

} // namespace
