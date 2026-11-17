// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/camera/virtual_camera_clock.h"

using Libraries::Camera::VirtualCameraClock;
using Libraries::Camera::VirtualCameraPeriodUs;

namespace {

constexpr u64 Period = 16666; // one frame at 60 Hz

u64 Read(VirtualCameraClock& clock, u64 now_us, bool wait_next, u64* slept_us = nullptr) {
    u64 slept = 0;
    const u64 frame = clock.Read(now_us, wait_next, [&slept](u64 us) { slept += us; });
    if (slept_us != nullptr) {
        *slept_us = slept;
    }
    return frame;
}

} // namespace

TEST(VirtualCameraClock, PeriodFollowsTheEmulatedRefreshRate) {
    EXPECT_EQ(VirtualCameraPeriodUs(60), 1000000 / 60);
    EXPECT_EQ(VirtualCameraPeriodUs(120), 1000000 / 120);
    EXPECT_EQ(VirtualCameraPeriodUs(0), 1000000 / 60);
}

TEST(VirtualCameraClock, FirstReadDeliversImmediately) {
    VirtualCameraClock clock(Period);
    u64 slept = 1;
    const u64 frame = Read(clock, 1000, /*wait_next=*/true, &slept);
    EXPECT_EQ(frame, 1000u);
    EXPECT_EQ(slept, 0u);
    EXPECT_EQ(clock.frame_count(), 1u);
    EXPECT_EQ(clock.next_frame_time_us(), 1000u + Period);
}

TEST(VirtualCameraClock, WaitNextFrameWaitsForTheSensorClockOnly) {
    VirtualCameraClock clock(Period);
    Read(clock, 0, false);
    u64 slept = 0;
    const u64 frame = Read(clock, 0, /*wait_next=*/true, &slept);
    EXPECT_EQ(slept, Period);
    EXPECT_EQ(frame, Period);
    EXPECT_EQ(clock.frame_count(), 2u);
}

TEST(VirtualCameraClock, WaitNextFrameNeverRunsAheadMoreThanOneFrame) {
    VirtualCameraClock clock(Period);
    Read(clock, 0, false);
    // The reader stalled for ten frame periods; it must still get exactly one new frame, so the
    // sensor can never overwrite the ring entry the title is about to consume.
    const u64 frame = Read(clock, 10 * Period, /*wait_next=*/true);
    EXPECT_EQ(frame, 10 * Period);
    EXPECT_EQ(clock.frame_count(), 2u);
    EXPECT_EQ(clock.next_frame_time_us(), 11 * Period);
}

TEST(VirtualCameraClock, LazyReadOnlyAdvancesWhenAFrameIsDue) {
    VirtualCameraClock clock(Period);
    const u64 first = Read(clock, 0, /*wait_next=*/false);
    EXPECT_EQ(first, 0u);
    const u64 still_first = Read(clock, Period / 2, /*wait_next=*/false);
    EXPECT_EQ(still_first, 0u);
    EXPECT_EQ(clock.frame_count(), 1u);
    const u64 second = Read(clock, Period, /*wait_next=*/false);
    EXPECT_EQ(second, Period);
    EXPECT_EQ(clock.frame_count(), 2u);
}

TEST(VirtualCameraClock, SensorKeepsDeliveringWhileNothingPresents) {
    // A title that reads the camera before it presents is exactly the case the old guest-frame
    // wait deadlocked: here the sensor clock alone advances the stream.
    VirtualCameraClock clock(Period);
    u64 now = 0;
    u64 frames = 0;
    for (int i = 0; i < 120; ++i) {
        Read(clock, now, /*wait_next=*/true);
        now += Period;
        frames = clock.frame_count();
    }
    EXPECT_EQ(frames, 120u);
    EXPECT_EQ(clock.frame_time_us(), 119 * Period);
}

TEST(VirtualCameraClock, PausedClockDoesNotReplayPastTimestamps) {
    VirtualCameraClock clock(Period);
    Read(clock, 1000, false);
    const u64 frame = Read(clock, 1000 + 100 * Period, /*wait_next=*/true);
    EXPECT_EQ(frame, 1000 + 100 * Period);
    EXPECT_GT(clock.next_frame_time_us(), frame);
}

TEST(VirtualCameraClock, ResetStartsANewSensorSession) {
    VirtualCameraClock clock(Period);
    Read(clock, 5000, true);
    Read(clock, 6000, true);
    clock.Reset(VirtualCameraPeriodUs(120));
    EXPECT_EQ(clock.frame_count(), 0u);
    const u64 frame = Read(clock, 50, true);
    EXPECT_EQ(frame, 50u);
    EXPECT_EQ(clock.frame_count(), 1u);
    EXPECT_EQ(clock.period_us(), VirtualCameraPeriodUs(120));
}
