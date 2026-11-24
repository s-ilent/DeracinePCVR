// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the texture capture's snapshot accounting.
//
// The defect these cover: the snapshot budget was incremented by a call site that did not exist,
// and the eligibility check compared a value it had just assigned, so the comparison was dead. A
// draw whose image the cache refused could therefore consume the budget, and a draw whose contents
// were never copied could still have its binding records retained.

#include <gtest/gtest.h>

#include "core/texture_capture.h"

namespace {

using Core::TextureCapture::ContentStatus;
using Core::TextureCapture::ImageRecord;

ImageRecord MakeRecord(u64 provenance, u64 stage_hash, u32 binding, u64 image_uid) {
    ImageRecord record{};
    record.provenance_id = provenance;
    record.stage_hash = stage_hash;
    record.vk_binding = binding;
    record.image_uid = image_uid;
    record.mip_first = 0;
    record.layer_first = 0;
    record.status = static_cast<u8>(ContentStatus::BindingOnly);
    return record;
}

class TextureCaptureAccounting : public ::testing::Test {
protected:
    void SetUp() override {
        Core::TextureCapture::ResetForTesting();
        Core::TextureCapture::SetEnabledForTesting(1);
    }
    void TearDown() override {
        Core::TextureCapture::SetEnabledForTesting(-1);
        Core::TextureCapture::ResetForTesting();
    }
};

TEST_F(TextureCaptureAccounting, CommittingADrawWithoutASnapshotRetainsNothing) {
    // A draw that bound images but whose contents were never copied must not contribute binding
    // records: a record without contents cannot be replayed, and keeping it fills the budget.
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::CommitPendingImages(7);
    EXPECT_EQ(Core::TextureCapture::ImageCount(), 0u);
}

TEST_F(TextureCaptureAccounting, CommittingAfterASnapshotRetainsTheDrawsRecords) {
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::NoteSnapshotTaken(7);
    Core::TextureCapture::CommitPendingImages(7);
    ASSERT_EQ(Core::TextureCapture::ImageCount(), 1u);
    EXPECT_EQ(Core::TextureCapture::Images()[0].provenance_id, 7u);
}

TEST_F(TextureCaptureAccounting, ARecordForAnotherDrawIsNotRetained) {
    // The snapshot belongs to draw 7, so a record that arrived for draw 8 must not be kept under
    // it. This is the check that was dead when the provenance was assigned before comparison.
    Core::TextureCapture::NoteImageBinding(MakeRecord(8, 0x22, 1, 200));
    Core::TextureCapture::NoteSnapshotTaken(7);
    Core::TextureCapture::CommitPendingImages(8);
    EXPECT_EQ(Core::TextureCapture::ImageCount(), 0u);
}

TEST_F(TextureCaptureAccounting, MultiStageRecordsForTheSnapshotDrawAreAllRetained) {
    // BindTextures runs once per stage, so a draw's records arrive in several commits. Every stage
    // must survive, which is what the incremental commit exists for.
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::NoteSnapshotTaken(7);
    Core::TextureCapture::CommitPendingImages(7);

    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x22, 2, 200));
    Core::TextureCapture::CommitPendingImages(7);

    ASSERT_EQ(Core::TextureCapture::ImageCount(), 2u);
    EXPECT_EQ(Core::TextureCapture::Images()[0].stage_hash, 0x11u);
    EXPECT_EQ(Core::TextureCapture::Images()[1].stage_hash, 0x22u);
}

TEST_F(TextureCaptureAccounting, ARepeatedStageDoesNotDuplicateItsBindings) {
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::NoteSnapshotTaken(7);
    Core::TextureCapture::CommitPendingImages(7);

    // The same stage visited again reports the same descriptor.
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::CommitPendingImages(7);

    EXPECT_EQ(Core::TextureCapture::ImageCount(), 1u);
}

TEST_F(TextureCaptureAccounting, SeparateBindingsOfTheSameImageAreBothRetained) {
    // Two descriptors can reference one image at different bindings. They are two bindings, not a
    // duplicate, and the identity includes the binding number so they are distinguished.
    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 1, 100));
    Core::TextureCapture::NoteSnapshotTaken(7);
    Core::TextureCapture::CommitPendingImages(7);

    Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, 2, 100));
    Core::TextureCapture::CommitPendingImages(7);

    EXPECT_EQ(Core::TextureCapture::ImageCount(), 2u);
}

TEST_F(TextureCaptureAccounting, TheSnapshotBudgetStopsFurtherSnapshots) {
    // The budget counts snapshots that produced a copy. Once it is spent, ShouldSnapshot reports
    // false so no further copy is scheduled.
    for (u32 i = 0; i < Core::TextureCapture::MaxSnapshotDraws; ++i) {
        EXPECT_TRUE(Core::TextureCapture::ShouldSnapshot()) << "at " << i;
        Core::TextureCapture::NoteSnapshotTaken(1000 + i);
    }
    EXPECT_FALSE(Core::TextureCapture::ShouldSnapshot());
}

TEST_F(TextureCaptureAccounting, ARejectedSnapshotDoesNotConsumeTheBudget) {
    // A draw whose image the cache refused never calls NoteSnapshotTaken, so the budget is intact.
    // This is the defect: the budget used to be incremented by a call site that did not exist, so
    // it was never consumed at all, and separately a commit could consume it without a copy.
    for (u32 i = 0; i < Core::TextureCapture::MaxSnapshotDraws * 2; ++i) {
        Core::TextureCapture::NoteImageBinding(MakeRecord(1000 + i, 0x11, 1, 100));
        Core::TextureCapture::CommitPendingImages(1000 + i);
    }
    EXPECT_TRUE(Core::TextureCapture::ShouldSnapshot());
    EXPECT_EQ(Core::TextureCapture::ImageCount(), 0u);
}

TEST_F(TextureCaptureAccounting, CapacityExhaustionIsReported) {
    // A capture that hit the record budget is truncated, and a package built from it must not claim
    // completeness. The flag is what carries that.
    EXPECT_FALSE(Core::TextureCapture::CapacityExhausted());
    Core::TextureCapture::NoteSnapshotTaken(7);
    for (u32 i = 0; i < Core::TextureCapture::MaxImageRecords + 8; ++i) {
        Core::TextureCapture::NoteImageBinding(MakeRecord(7, 0x11, i, 100 + i));
        Core::TextureCapture::CommitPendingImages(7);
    }
    EXPECT_TRUE(Core::TextureCapture::CapacityExhausted());
}

TEST_F(TextureCaptureAccounting, ALateReadbackJoinsOnlyItsOwnDraw) {
    // A deferred readback arrives after the draw was committed. It must be stored under the draw
    // that scheduled it, and a later draw must not inherit it.
    ImageRecord late = MakeRecord(7, 0x11, 1, 100);
    late.status = static_cast<u8>(ContentStatus::HostContentCaptured);
    late.bytes = {1, 2, 3, 4};
    late.content_hash = Core::TextureCapture::HashBytes(late.bytes.data(), late.bytes.size());
    Core::TextureCapture::StoreLateImage(late);

    ASSERT_EQ(Core::TextureCapture::ImageCount(), 1u);
    EXPECT_EQ(Core::TextureCapture::Images()[0].provenance_id, 7u);
    EXPECT_TRUE(Core::TextureCapture::HasContentFor(7));
    EXPECT_FALSE(Core::TextureCapture::HasContentFor(8));
}

} // namespace
