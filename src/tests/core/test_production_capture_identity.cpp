// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Production-path tests for image-array and sampler identity.
//
// These call the real capture functions — `NoteImageBinding`, `CommitPendingImages`,
// `NoteSnapshotTaken`, `NoteSampler` — rather than constructing package objects directly. The
// RHI-04R4 tests built packages by hand, which proved the validator's contract but never exercised
// the producer that fills it. The defects these exist to prevent:
//
//   - the descriptor-write loop recorded only the first element of an image array;
//   - the snapshot loop derived Vulkan binding numbers from flattened image indices;
//   - the image-sampler association searched every retained sampler rather than the selected draw's.

#include <gtest/gtest.h>

#include "core/texture_capture.h"

namespace {

using Core::TextureCapture::ContentStatus;
using Core::TextureCapture::ImageRecord;
using Core::TextureCapture::SamplerRecord;

void Reset() {
    Core::TextureCapture::ResetForTesting();
}

/// An image binding observation shaped the way the descriptor-write loop now writes it.
ImageRecord MakeImage(u64 provenance, u64 stage_hash, u32 binding, u32 element, u32 array_count,
                      u64 image_uid, u32 shader_image_index) {
    ImageRecord record{};
    record.provenance_id = provenance;
    record.stage_hash = stage_hash;
    record.stage = 0;
    record.vk_binding = binding;
    record.array_element = element;
    record.array_count = array_count;
    record.shader_image_index = shader_image_index;
    record.guest_slot = shader_image_index;
    record.image_id = static_cast<u32>(image_uid & 0xFFFF);
    record.image_uid = image_uid;
    record.vk_image = 0x1000 + image_uid;
    record.vk_image_view = 0x2000 + image_uid;
    record.image_format = 37;
    record.view_format = 37;
    record.aspect = 1;
    record.mip_first = 0;
    record.mip_count = 1;
    record.layer_first = 0;
    record.layer_count = 1;
    record.width = 4;
    record.height = 4;
    record.row_pitch = 16;
    record.status = static_cast<u8>(ContentStatus::BindingOnly);
    return record;
}

SamplerRecord MakeSampler(u64 provenance, u64 stage_hash, u32 binding, u64 guest_hash,
                          u32 associated_image) {
    SamplerRecord record{};
    record.provenance_id = provenance;
    record.stage_hash = stage_hash;
    record.vk_binding = binding;
    record.has_vk_binding = true;
    record.guest_hash = guest_hash;
    record.associated_image = associated_image;
    return record;
}

// --- Image array observations ---------------------------------------------------------------

TEST(ProductionArrayIdentity, ATwoElementArrayProducesTwoObservationsAtOneBinding) {
    // The defect: the write loop recorded one observation per descriptor, using only the first
    // element, so element 1 was never observed.
    Reset();
    Core::TextureCapture::NoteSnapshotTaken(1);
    Core::TextureCapture::NoteImageBinding(MakeImage(1, 0x11, /*binding=*/4, /*element=*/0,
                                                     /*array_count=*/2, /*uid=*/0xA, 0));
    Core::TextureCapture::NoteImageBinding(MakeImage(1, 0x11, /*binding=*/4, /*element=*/1,
                                                     /*array_count=*/2, /*uid=*/0xB, 1));
    Core::TextureCapture::CommitPendingImages(1);

    const auto images = Core::TextureCapture::Images();
    ASSERT_EQ(images.size(), 2u);
    // Both observations name the same binding, and differ only by element.
    EXPECT_EQ(images[0].vk_binding, 4u);
    EXPECT_EQ(images[1].vk_binding, 4u);
    EXPECT_EQ(images[0].array_element, 0u);
    EXPECT_EQ(images[1].array_element, 1u);
    EXPECT_EQ(images[0].array_count, 2u);
    EXPECT_EQ(images[1].array_count, 2u);
}

TEST(ProductionArrayIdentity, TwoElementsOfTheSameImageKeepDistinctIdentities) {
    // Two elements may reference the same image and view. They are still two descriptors.
    Reset();
    Core::TextureCapture::NoteSnapshotTaken(1);
    Core::TextureCapture::NoteImageBinding(
        MakeImage(1, 0x11, 4, /*element=*/0, /*array_count=*/2, /*uid=*/0xA, 0));
    Core::TextureCapture::NoteImageBinding(
        MakeImage(1, 0x11, 4, /*element=*/1, /*array_count=*/2, /*uid=*/0xA, 1));
    Core::TextureCapture::CommitPendingImages(1);

    const auto images = Core::TextureCapture::Images();
    ASSERT_EQ(images.size(), 2u);
    // Same image, different element: the dedup must not collapse them.
    EXPECT_EQ(images[0].image_uid, images[1].image_uid);
    EXPECT_NE(images[0].array_element, images[1].array_element);
}

TEST(ProductionArrayIdentity, AGenuinelyIdenticalObservationIsDeduplicated) {
    Reset();
    Core::TextureCapture::NoteSnapshotTaken(1);
    Core::TextureCapture::NoteImageBinding(MakeImage(1, 0x11, 4, 0, 2, 0xA, 0));
    Core::TextureCapture::NoteImageBinding(MakeImage(1, 0x11, 4, 0, 2, 0xA, 0));
    Core::TextureCapture::CommitPendingImages(1);
    EXPECT_EQ(Core::TextureCapture::Images().size(), 1u);
}

TEST(ProductionArrayIdentity, ARecordFromAnotherDrawIsNotCommitted) {
    // Only the draw a snapshot was taken for contributes records.
    Reset();
    Core::TextureCapture::NoteSnapshotTaken(1);
    Core::TextureCapture::NoteImageBinding(MakeImage(2, 0x11, 4, 0, 1, 0xA, 0));
    Core::TextureCapture::CommitPendingImages(1);
    EXPECT_EQ(Core::TextureCapture::Images().size(), 0u);
}

TEST(ProductionArrayIdentity, TheSnapshotRejoinsTheCorrectBindingAndElement) {
    // The defect: the snapshot derived its binding from a flattened image index, so a two-element
    // array at binding 4 produced snapshots at bindings 4 and 5.
    Reset();
    Core::TextureCapture::NoteSnapshotTaken(1);
    // A binding observation and its late content, both carrying the write's own identity.
    Core::TextureCapture::NoteImageBinding(MakeImage(1, 0x11, 4, /*element=*/1, /*count=*/2, 0xB, 1));
    auto content = MakeImage(1, 0x11, 4, /*element=*/1, /*count=*/2, 0xB, 1);
    content.status = static_cast<u8>(ContentStatus::HostContentCaptured);
    content.bytes = {1, 2, 3, 4};
    content.content_hash = 0x1234;
    Core::TextureCapture::NoteImageBinding(content);
    Core::TextureCapture::CommitPendingImages(1);

    const auto images = Core::TextureCapture::Images();
    // The two records share binding and element, so they are one descriptor, not two.
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].vk_binding, 4u);
    EXPECT_EQ(images[0].array_element, 1u);
}

// --- Sampler provenance ---------------------------------------------------------------------

TEST(ProductionSamplerIdentity, IdenticalStateAtTwoBindingsIsTwoObservations) {
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(1, 0x11, 3, 0xAA, 0));
    Core::TextureCapture::NoteSampler(MakeSampler(1, 0x11, 7, 0xAA, 0));
    EXPECT_EQ(Core::TextureCapture::SamplerCount(), 2u);
}

TEST(ProductionSamplerIdentity, AForeignSamplerIsNotSelectedForAssociation) {
    // The defect: the association searched every retained sampler, so a sampler from another draw
    // with the same shader hash and image index could be paired with this draw's image.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(/*provenance=*/2, 0x11, 3, 0xAA, /*image=*/0));
    Core::TextureCapture::NoteSampler(MakeSampler(/*provenance=*/1, 0x11, 5, 0xBB, /*image=*/0));

    // The association the exporter performs: only this draw's samplers are candidates.
    const auto samplers = Core::TextureCapture::Samplers();
    u32 matches = 0;
    u64 selected = 0;
    for (const auto& sampler : samplers) {
        if (sampler.provenance_id != 1) {
            continue;
        }
        if (sampler.associated_image != 0) {
            continue;
        }
        if (sampler.stage_hash != 0x11) {
            continue;
        }
        ++matches;
        selected = sampler.id;
    }
    EXPECT_EQ(matches, 1u);
    EXPECT_NE(selected, 0u);
    // The foreign sampler is still retained, but it was never a candidate.
    EXPECT_EQ(samplers.size(), 2u);
}

TEST(ProductionSamplerIdentity, TwoSamplersClaimingOneImageAreAmbiguous) {
    // Two samplers in one draw naming the same image index means the evidence does not identify
    // which was used, so the association must not be claimed as verified.
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(1, 0x11, 3, 0xAA, /*image=*/0));
    Core::TextureCapture::NoteSampler(MakeSampler(1, 0x11, 5, 0xBB, /*image=*/0));

    const auto samplers = Core::TextureCapture::Samplers();
    u32 matches = 0;
    for (const auto& sampler : samplers) {
        if (sampler.provenance_id != 1 || sampler.associated_image != 0 ||
            sampler.stage_hash != 0x11) {
            continue;
        }
        ++matches;
    }
    EXPECT_EQ(matches, 2u);
}

TEST(ProductionSamplerIdentity, ASamplerFromAnotherDrawIsRetainedButSeparate) {
    Reset();
    Core::TextureCapture::NoteSampler(MakeSampler(1, 0x11, 3, 0xAA, 0));
    Core::TextureCapture::NoteSampler(MakeSampler(2, 0x11, 3, 0xAA, 0));
    // Same state, same binding, different draw: two observations.
    EXPECT_EQ(Core::TextureCapture::SamplerCount(), 2u);
}

} // namespace
