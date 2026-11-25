// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for sampler binding identity.
//
// The defect these exist to prevent: `TextureCapture::NoteSampler` deduplicated on
// (guest_hash, stage_hash), which describes sampler state reuse rather than descriptor-binding
// identity. One shader may bind identical sampler state at several Vulkan bindings, and the second
// binding was discarded while the capture still reported itself complete.

#include <gtest/gtest.h>

#include "core/texture_capture.h"

namespace {

using Core::TextureCapture::SamplerRecord;

SamplerRecord MakeSampler(u64 guest_hash, u64 stage_hash, u32 binding, u64 provenance,
                          u32 associated_image = 0) {
    SamplerRecord record{};
    record.guest_hash = guest_hash;
    record.stage_hash = stage_hash;
    record.vk_binding = binding;
    record.has_vk_binding = true;
    record.provenance_id = provenance;
    record.associated_image = associated_image;
    return record;
}

void Reset() {
    Core::TextureCapture::ResetForTesting();
}

/// The number of retained sampler observations.
size_t Count() {
    return Core::TextureCapture::SamplerCount();
}

TEST(SamplerBinding, IdenticalStateAtTwoBindingsIsTwoObservations) {
    // The defect in its smallest form: the same state at two bindings is two descriptors.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 7, 1));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, IdenticalStateInTwoStagesIsTwoObservations) {
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x22, 3, 1));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, IdenticalStateInTwoDrawsIsTwoObservations) {
    // Each draw's package must name its own observation.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 2));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, DifferentStateAtTheSameBindingIsTwoObservations) {
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    Core::TextureCapture::NoteSampler(MakeSampler(0xBB, 0x11, 3, 1));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, AGenuinelyIdenticalObservationIsDeduplicated) {
    // Same draw, stage, binding, element and state is one observation reported twice.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    EXPECT_EQ(Count(), 1u);
}

TEST(SamplerBinding, TheSameBindingWithADifferentImageIsTwoObservations) {
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1, /*associated_image=*/0));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1, /*associated_image=*/1));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, ADifferentArrayElementIsADifferentObservation) {
    Reset();
    auto first = MakeSampler(0xAA, 0x11, 3, 1);
    first.array_element = 0;
    auto second = MakeSampler(0xAA, 0x11, 3, 1);
    second.array_element = 1;
    Core::TextureCapture::NoteSampler(first);
    Core::TextureCapture::NoteSampler(second);
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, AMissingProvenanceIsStillRecorded) {
    // A record with no draw is a weaker observation, but it is not silently merged with another.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 0));
    Core::TextureCapture::NoteSampler(MakeSampler(0xAA, 0x11, 3, 1));
    EXPECT_EQ(Count(), 2u);
}

TEST(SamplerBinding, AMissingBindingIsStillRecorded) {
    // A record with no binding identity cannot satisfy a declaration, but it is preserved so the
    // gap is visible rather than silently dropped.
    Reset();
    auto record = MakeSampler(0xAA, 0x11, 0, 1);
    record.has_vk_binding = false;
    Core::TextureCapture::NoteSampler(record);
    EXPECT_EQ(Count(), 1u);
}

TEST(SamplerBinding, CapacityExhaustionIsReportedRatherThanSilent) {
    // A dropped binding must be visible, not hidden behind a complete-capture claim.
    Reset();
    const size_t limit = Core::TextureCapture::MaxSamplerRecords;
    for (size_t i = 0; i < limit; ++i) {
        Core::TextureCapture::NoteSampler(
            MakeSampler(0xAA + i, 0x11, static_cast<u32>(i), 1));
    }
    EXPECT_EQ(Count(), limit);
    Core::TextureCapture::NoteSampler(MakeSampler(0xFFFF, 0x11, 9999, 1));
    EXPECT_EQ(Count(), limit);
    EXPECT_GT(Core::TextureCapture::SamplerCapacityDropped(), 0u);
}

} // namespace
