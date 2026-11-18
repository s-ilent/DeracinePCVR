// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the submission profiler reports is decided by the arithmetic in GpuTimelineMath, and every
// way that arithmetic can be wrong still reads as a plausible number in the log: a gap invented
// where the GPU was never idle, busy time lost to an unusable pair, or ticks reported as if they
// were nanoseconds. These pin the rules down without a device.

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_gpu_timing_math.h"

namespace {

using Vulkan::GpuTimelineMath;

// Every bit of a 64-bit timestamp counts.
constexpr u64 FullMask = ~u64{0};

TEST(GpuTiming, BusyIsTheIntervalScaledByTheTickPeriod) {
    GpuTimelineMath math;
    // 2000 ticks of a 0.5 ns tick is 1000 ns, not 2000: the raw counter is not a nanosecond.
    const auto measured = math.Add(1000, 3000, 0.5, FullMask);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 1000u);
    EXPECT_EQ(measured.gap_ns, 0u);
}

TEST(GpuTiming, TheFirstSubmissionHasNoGap) {
    GpuTimelineMath math;
    const auto measured = math.Add(5000, 6000, 1.0, FullMask);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 1000u);
    // Nothing came before it, so there is no time between submissions to report.
    EXPECT_EQ(measured.gap_ns, 0u);
}

// A timestamp of zero is a reading, not "nothing has happened yet": the first interval must not
// be mistaken for the absence of a previous one.
TEST(GpuTiming, AZeroTimestampIsAReadingAndNotAnAbsentOne) {
    GpuTimelineMath math;
    const auto first = math.Add(0, 100, 1.0, FullMask);
    ASSERT_TRUE(first.counted);
    EXPECT_EQ(first.busy_ns, 100u);
    EXPECT_EQ(first.gap_ns, 0u);

    const auto second = math.Add(150, 200, 1.0, FullMask);
    ASSERT_TRUE(second.counted);
    EXPECT_EQ(second.busy_ns, 50u);
    EXPECT_EQ(second.gap_ns, 50u);
}

TEST(GpuTiming, GapRunsFromThePreviousEndToThisStart) {
    GpuTimelineMath math;
    ASSERT_TRUE(math.Add(1000, 2000, 1.0, FullMask).counted);
    const auto measured = math.Add(2500, 4000, 1.0, FullMask);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 1500u);
    // 2500 - 2000, not 2500 - 1000: the gap starts where the previous submission finished.
    EXPECT_EQ(measured.gap_ns, 500u);
}

// The next submission starting before the previous one ended is no gap at all. Reporting a
// negative one as a huge positive is exactly the failure this rule exists to prevent.
TEST(GpuTiming, AStartBeforeThePreviousEndIsNoGap) {
    GpuTimelineMath math;
    ASSERT_TRUE(math.Add(1000, 2000, 1.0, FullMask).counted);
    const auto measured = math.Add(1800, 3000, 1.0, FullMask);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 1200u);
    EXPECT_EQ(measured.gap_ns, 0u);
}

// An unusable pair contributes nothing, and must not move the point the next gap is measured
// from: the interval after it still runs from the last end that was real.
TEST(GpuTiming, AReversedPairCountsNothingAndKeepsTheLastRealEnd) {
    GpuTimelineMath math;
    ASSERT_TRUE(math.Add(1000, 2000, 1.0, FullMask).counted);

    const auto reversed = math.Add(5000, 4000, 1.0, FullMask);
    EXPECT_FALSE(reversed.counted);
    EXPECT_EQ(reversed.busy_ns, 0u);
    EXPECT_EQ(reversed.gap_ns, 0u);

    const auto next = math.Add(3000, 3500, 1.0, FullMask);
    ASSERT_TRUE(next.counted);
    EXPECT_EQ(next.busy_ns, 500u);
    EXPECT_EQ(next.gap_ns, 1000u);
}

// Only the bits the device says are valid mean anything; the rest is garbage a driver may leave.
TEST(GpuTiming, TheValidMaskHidesGarbageHighBits) {
    GpuTimelineMath math;
    constexpr u64 Low32 = (u64{1} << 32) - 1;
    const auto measured = math.Add(0x100000010ull, 0x200000020ull, 1.0, Low32);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 0x10u);
}

TEST(GpuTiming, AZeroLengthIntervalIsStillCounted) {
    GpuTimelineMath math;
    const auto measured = math.Add(1000, 1000, 1.0, FullMask);
    ASSERT_TRUE(measured.counted);
    EXPECT_EQ(measured.busy_ns, 0u);
    EXPECT_EQ(measured.gap_ns, 0u);
}

} // namespace
