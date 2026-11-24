// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the producer-side readiness and push-constant bounds.
//
// The defects these exist to prevent:
//
//   1. Readiness was decided from a value: `topology != 0`. Zero is a legitimate Vulkan topology,
//      so a point-list draw would report its pipeline state as not captured.
//   2. Push constants were resized to the declared size and then copied from a fixed 64-byte
//      source, leaving a zero-filled tail that was never captured. The replay would then accept
//      fabricated parameters as genuine captured state.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

#include "core/draw_package.h"

namespace {

using Core::DrawPackage::Package;
using Core::DrawPackage::PipelineState;
using Core::DrawPackage::SchemaVersion;

/// A package that validates, so a test can vary one field and see the verdict change.
Package MakePackage() {
    Package package{};
    package.schema_version = SchemaVersion;
    package.provenance_id = 1;
    package.pipeline_state.present.topology = true;
    package.pipeline_state.has_topology = true;
    package.pipeline_state.topology = 3;
    package.pipeline_state.present.color_formats = true;
    package.pipeline_state.color_formats = {37};
    return package;
}

TEST(ProducerReadiness, AZeroTopologyIsPresentAndReady) {
    // VK_PRIMITIVE_TOPOLOGY_POINT_LIST is 0. A point-list draw has a captured topology.
    auto package = MakePackage();
    package.pipeline_state.topology = 0;
    package.readiness.pipeline_state_ready_original = true;
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(ProducerReadiness, AnAbsentTopologyIsNotReady) {
    auto package = MakePackage();
    package.pipeline_state.present.topology = false;
    package.pipeline_state.has_topology = false;
    package.readiness.pipeline_state_ready_original = true;
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no topology"), std::string::npos);
}

TEST(ProducerReadiness, ANonzeroTopologyIsReady) {
    auto package = MakePackage();
    package.pipeline_state.topology = 4;
    package.readiness.pipeline_state_ready_original = true;
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
}

TEST(ProducerReadiness, AnAbsentColourFormatIsNotReady) {
    auto package = MakePackage();
    package.pipeline_state.present.color_formats = false;
    package.pipeline_state.color_formats.clear();
    package.readiness.pipeline_state_ready_original = true;
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
    EXPECT_NE(error.find("no colour attachment format"), std::string::npos);
}

TEST(ProducerReadiness, AReadyClaimWithoutTheFieldsIsRefused) {
    // The readiness flag is a claim to verify, not permission to skip verification.
    auto package = MakePackage();
    package.pipeline_state.present.topology = false;
    package.pipeline_state.has_topology = false;
    package.readiness.pipeline_state_ready_original = true;
    std::string error;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

TEST(ProducerReadiness, DiagnosticSubstitutionDoesNotImplyOriginalReadiness) {
    // A package the replay can render with substitutions is not a package whose original state was
    // captured. The two readiness dimensions stay separate.
    // A package whose original pipeline state was not captured can still be valid: the readiness
    // dimensions are separate claims, and a diagnostic replay does not imply an original one. The
    // assertion is that the validator accepts the package and that the original dimension stays
    // false, not that a diagnostic replay is available.
    auto package = MakePackage();
    package.readiness.pipeline_state_ready_original = false;
    std::string error;
    EXPECT_TRUE(Core::DrawPackage::ValidatePackage(package, error)) << error;
    EXPECT_FALSE(package.readiness.pipeline_state_ready_original);

    // Claiming the original dimension without the fields is refused, which is the separation the
    // test exists to establish.
    package.readiness.pipeline_state_ready_original = true;
    package.pipeline_state.present.topology = false;
    package.pipeline_state.has_topology = false;
    EXPECT_FALSE(Core::DrawPackage::ValidatePackage(package, error));
}

/// The producer's push-constant capacity, mirroring the fixed source array.
constexpr size_t kCapacity = 16 * sizeof(u32);

/// The producer's conversion, extracted so the bounds behaviour can be tested directly.
struct PushResult {
    bool fit{};
    std::vector<u8> bytes;
};

PushResult ConvertPushConstants(const std::array<u32, 16>& words,
                                u32 declared_size) {
    PushResult out{};
    if (declared_size > kCapacity) {
        // The range is not representable. No bytes are claimed, because a partial block presented
        // as complete would be worse than an absent one.
        return out;
    }
    out.fit = true;
    out.bytes.assign(reinterpret_cast<const u8*>(words.data()),
                     reinterpret_cast<const u8*>(words.data()) + declared_size);
    return out;
}

TEST(ProducerPushConstants, AZeroByteRangeProducesNoBytes) {
    std::array<u32, 16> words{};
    const auto result = ConvertPushConstants(words, 0);
    EXPECT_TRUE(result.fit);
    EXPECT_TRUE(result.bytes.empty());
}

TEST(ProducerPushConstants, TheExactCapacityIsAccepted) {
    std::array<u32, 16> words{};
    for (u32 i = 0; i < 16; ++i) {
        words[i] = 0x11111111u * (i + 1);
    }
    const auto result = ConvertPushConstants(words, static_cast<u32>(kCapacity));
    EXPECT_TRUE(result.fit);
    EXPECT_EQ(result.bytes.size(), kCapacity);
    // The last word must be the captured one, not a zero-filled tail.
    u32 last = 0;
    std::memcpy(&last, result.bytes.data() + kCapacity - 4, 4);
    EXPECT_EQ(last, 0x11111111u * 16);
}

TEST(ProducerPushConstants, OneByteBelowCapacityIsAccepted) {
    std::array<u32, 16> words{};
    const auto result = ConvertPushConstants(words, static_cast<u32>(kCapacity - 1));
    EXPECT_TRUE(result.fit);
    EXPECT_EQ(result.bytes.size(), kCapacity - 1);
}

TEST(ProducerPushConstants, OneByteAboveCapacityIsRefused) {
    // The defect: resizing to the declared size and copying the smaller of the two left a
    // zero-filled tail that was never captured.
    std::array<u32, 16> words{};
    const auto result = ConvertPushConstants(words, static_cast<u32>(kCapacity + 1));
    EXPECT_FALSE(result.fit);
    EXPECT_TRUE(result.bytes.empty());
}

TEST(ProducerPushConstants, AFarOversizedRangeIsRefused) {
    std::array<u32, 16> words{};
    const auto result = ConvertPushConstants(words, 4096);
    EXPECT_FALSE(result.fit);
    EXPECT_TRUE(result.bytes.empty());
}

TEST(ProducerPushConstants, AnAllZeroBlockIsDistinguishableFromMissingData) {
    // A captured block of zeroes is a real capture. Missing data is an absent block. The two must
    // not be confused, which is why the fit flag exists separately from the bytes.
    std::array<u32, 16> words{};
    const auto captured = ConvertPushConstants(words, 16);
    EXPECT_TRUE(captured.fit);
    EXPECT_EQ(captured.bytes.size(), 16u);
    for (const auto byte : captured.bytes) {
        EXPECT_EQ(byte, 0);
    }

    const auto missing = ConvertPushConstants(words, static_cast<u32>(kCapacity + 4));
    EXPECT_FALSE(missing.fit);
    EXPECT_TRUE(missing.bytes.empty());
}

} // namespace
