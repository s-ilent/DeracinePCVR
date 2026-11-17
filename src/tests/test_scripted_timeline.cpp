// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The scripted-input timeline: a line may be timed by the clock (the original behaviour) or by
// the guest's own frame count (deterministic when the title's frame pacing is not). These tests
// pin the selection rules without an emulator or a GPU.

#include "input/scripted_timeline.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using Input::Scripted::Active;
using Input::Scripted::Begun;
using Input::Scripted::Later;
using Input::Scripted::ParseFramesToken;
using Input::Scripted::ParseFrameToken;
using Input::Scripted::StepTiming;

StepTiming Timed(double start, double end) {
    StepTiming step;
    step.start = start;
    step.end = end;
    return step;
}

StepTiming AtFrame(std::uint64_t frame, std::uint32_t frames = 0) {
    StepTiming step;
    step.frame = frame;
    step.frames = frames;
    return step;
}

TEST(ScriptedTimeline, ClockLinesKeepTheirMeaning) {
    const StepTiming line = Timed(2.0, 3.0);
    EXPECT_FALSE(Begun(line, 1.9, 0));
    EXPECT_TRUE(Begun(line, 2.0, 0));
    EXPECT_FALSE(Active(line, 1.9, 0));
    EXPECT_TRUE(Active(line, 2.0, 0));
    EXPECT_TRUE(Active(line, 2.9, 0));
    EXPECT_FALSE(Active(line, 3.0, 0));
    // The guest frame counter means nothing to a clock-numbered line.
    EXPECT_FALSE(Active(line, 0.5, 10000));
}

TEST(ScriptedTimeline, FrameLineStartsOnItsFrame) {
    const StepTiming line = AtFrame(120);
    EXPECT_FALSE(Begun(line, 100.0, 119));
    EXPECT_TRUE(Begun(line, 0.0, 120));
    EXPECT_FALSE(Active(line, 100.0, 119));
    EXPECT_TRUE(Active(line, 0.0, 120));
    // Without a count it stays in effect, like a pose line that persists.
    EXPECT_TRUE(Active(line, 0.0, 9999));
}

TEST(ScriptedTimeline, FramesCountLimitsTheLine) {
    const StepTiming line = AtFrame(300, 60);
    EXPECT_FALSE(Active(line, 0.0, 299));
    EXPECT_TRUE(Active(line, 0.0, 300));
    EXPECT_TRUE(Active(line, 0.0, 359));
    EXPECT_FALSE(Active(line, 0.0, 360));
}

TEST(ScriptedTimeline, FrameLineIgnoresTheClock) {
    // A wall clock far past the times a frame-numbered line may also carry: only its frame counts.
    const StepTiming line = AtFrame(50, 10);
    EXPECT_FALSE(Active(line, 1e6, 49));
    EXPECT_TRUE(Active(line, 1e6, 50));
}

TEST(ScriptedTimeline, LaterPrefersFrameNumberedLines) {
    // (A frame-numbered line is deterministic, so it wins over a clock-numbered one when both
    // describe the same thing; otherwise each domain compares within itself.)
    EXPECT_TRUE(Later(AtFrame(1), Timed(100.0, 200.0)));
    EXPECT_FALSE(Later(Timed(100.0, 200.0), AtFrame(1)));
    EXPECT_TRUE(Later(AtFrame(5), AtFrame(4)));
    EXPECT_FALSE(Later(AtFrame(4), AtFrame(5)));
    EXPECT_TRUE(Later(Timed(5.0, 6.0), Timed(4.0, 5.0)));
    EXPECT_FALSE(Later(Timed(4.0, 5.0), Timed(5.0, 6.0)));
    // The most recently started line wins, so equal starts count as later.
    EXPECT_TRUE(Later(AtFrame(7), AtFrame(7)));
}

TEST(ScriptedTimeline, FrameTokensParse) {
    std::optional<std::uint64_t> frame;
    EXPECT_TRUE(ParseFrameToken("frame=1234", frame));
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(*frame, 1234u);

    frame.reset();
    EXPECT_FALSE(ParseFrameToken("frame=", frame));
    EXPECT_FALSE(ParseFrameToken("frame=12x", frame));
    EXPECT_FALSE(ParseFrameToken("frames=12", frame));
    EXPECT_FALSE(ParseFrameToken("head=0,0,0", frame));
    EXPECT_FALSE(frame.has_value());

    std::uint32_t frames = 0;
    EXPECT_TRUE(ParseFramesToken("frames=60", frames));
    EXPECT_EQ(frames, 60u);
    EXPECT_TRUE(ParseFramesToken("frames=0", frames));
    EXPECT_EQ(frames, 0u);
    EXPECT_FALSE(ParseFramesToken("frames=", frames));
    EXPECT_FALSE(ParseFramesToken("frames=-1", frames));
    EXPECT_FALSE(ParseFramesToken("frame=60", frames));
}

} // namespace
