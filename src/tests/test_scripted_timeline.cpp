// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The scripted-input timeline: a line may be timed by the clock (the original behaviour) or by
// the guest's own frame count (deterministic when the title's frame pacing is not). These tests
// pin the selection rules without an emulator or a GPU.

#include "input/scripted_timeline.h"
#include "input/scripted_worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using Input::Scripted::Active;
using Input::Scripted::Begun;
using Input::Scripted::Later;
using Input::Scripted::ParseFramesToken;
using Input::Scripted::ParseFrameToken;
using Input::Scripted::ScriptLifetime;
using Input::Scripted::StepTiming;
using Input::Scripted::Worker;
using State = ScriptLifetime::State;

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

// ---------------------------------------------------------------------------------------------
// Whole-script lifetime: a script lasts until both timing domains have run out. The stall bound
// defaults to off in these tests (0); the watchdog has its own cases.
// ---------------------------------------------------------------------------------------------

ScriptLifetime Lifetime(std::initializer_list<StepTiming> steps, double stall_seconds = 0.0) {
    return ScriptLifetime{std::vector<StepTiming>(steps), stall_seconds};
}

TEST(ScriptedLifetime, FrameOnlyScriptWaitsForItsFrame) {
    auto lifetime = Lifetime({AtFrame(120)});
    EXPECT_EQ(lifetime.Update(0.0, 0), State::Running);
    // Even a wall clock far past any time the script carries does not end it: only the frame
    // does. (This is the defect the lifetime fixes: the old wall-clock end would fire here.)
    EXPECT_EQ(lifetime.Update(1e6, 119), State::Running);
    EXPECT_EQ(lifetime.Update(1e6, 120), State::Finished);
    // Finished is final.
    EXPECT_EQ(lifetime.Update(1e9, 120), State::Finished);
}

TEST(ScriptedLifetime, FrameOnlyScriptWithDurationWaitsOutItsFrames) {
    auto lifetime = Lifetime({AtFrame(300, 60)});
    EXPECT_EQ(lifetime.FrameEnd(), 360u);
    EXPECT_EQ(lifetime.Update(1e6, 359), State::Running);
    EXPECT_EQ(lifetime.Update(1e6, 360), State::Finished);
}

TEST(ScriptedLifetime, ClockOnlyScriptFinishesAtItsEnd) {
    auto lifetime = Lifetime({Timed(2.0, 3.0)});
    EXPECT_EQ(lifetime.ClockEnd(), 3.0);
    EXPECT_EQ(lifetime.Update(2.99, 0), State::Running);
    EXPECT_EQ(lifetime.Update(3.0, 0), State::Finished);
    // The guest frame counter means nothing to a clock-only script.
    EXPECT_EQ(Lifetime({Timed(0.0, 1.0)}).Update(1.0, 1'000'000), State::Finished);
}

TEST(ScriptedLifetime, MixedScriptWaitsForBothDomains) {
    // The frame part runs out first; the clock still holds the script open.
    auto clock_pending = Lifetime({Timed(0.0, 2.0), AtFrame(500, 10)});
    EXPECT_EQ(clock_pending.Update(1.0, 510), State::Running);
    EXPECT_EQ(clock_pending.Update(2.0, 510), State::Finished);

    // The clock part runs out first; the frame still holds the script open.
    auto frame_pending = Lifetime({Timed(0.0, 2.0), AtFrame(500)});
    EXPECT_EQ(frame_pending.Update(100.0, 499), State::Running);
    EXPECT_EQ(frame_pending.Update(100.0, 500), State::Finished);
}

TEST(ScriptedLifetime, ActionDurationIsCountedInGuestFrames) {
    // `frames=1` is in effect on its start frame only; `frames=0` stays in effect (a pose).
    const StepTiming one = AtFrame(500, 1);
    EXPECT_TRUE(Active(one, 0.0, 500));
    EXPECT_FALSE(Active(one, 0.0, 501));
    const StepTiming persistent = AtFrame(500);
    EXPECT_TRUE(Active(persistent, 0.0, 500));
    EXPECT_TRUE(Active(persistent, 0.0, 5000));
    // A persistent frame line is complete at its frame, so the script does not wait on it.
    EXPECT_EQ(Lifetime({persistent}).Update(1e6, 499), State::Running);
    EXPECT_EQ(Lifetime({persistent}).Update(1e6, 500), State::Finished);
}

TEST(ScriptedLifetime, StallWatchdogEndsAStalledScript) {
    auto lifetime = Lifetime({AtFrame(1000)}, 30.0);
    EXPECT_EQ(lifetime.Update(0.0, 0), State::Running);
    EXPECT_EQ(lifetime.Update(29.9, 0), State::Running);
    EXPECT_EQ(lifetime.Update(30.1, 0), State::Stalled);
    EXPECT_EQ(lifetime.StallSeconds(), 30.0);
    // Stalled is final too: shutdown releases the inputs once.
    EXPECT_EQ(lifetime.Update(100.0, 1000), State::Stalled);
}

TEST(ScriptedLifetime, StallWatchdogRestartsWhenFramesMove) {
    auto lifetime = Lifetime({AtFrame(1000)}, 30.0);
    EXPECT_EQ(lifetime.Update(0.0, 0), State::Running);
    EXPECT_EQ(lifetime.Update(20.0, 7), State::Running);
    EXPECT_EQ(lifetime.Update(49.9, 7), State::Running); // 29.9 s since the last movement
    EXPECT_EQ(lifetime.Update(50.1, 7), State::Stalled);
}

TEST(ScriptedLifetime, StallWatchdogIgnoresPendingClockWork) {
    // The frames are standing still, but no frame-numbered step is pending any more: only the
    // clock holds the script open, so the watchdog must not fire.
    auto lifetime = Lifetime({Timed(0.0, 1000.0), AtFrame(10, 5)}, 30.0);
    EXPECT_EQ(lifetime.Update(0.0, 15), State::Running);
    EXPECT_EQ(lifetime.Update(1e6, 15), State::Finished);
}

TEST(ScriptedLifetime, StallWatchdogCanBeDisabled) {
    for (const double bound : {0.0, -1.0}) {
        auto lifetime = Lifetime({AtFrame(1000)}, bound);
        EXPECT_EQ(lifetime.Update(0.0, 0), State::Running);
        EXPECT_EQ(lifetime.Update(1e6, 0), State::Running);
    }
}

// ---------------------------------------------------------------------------------------------
// Worker ownership: the replay thread is owned and stoppable, so shutdown can join it with a
// bound instead of leaving a detached thread behind.
// ---------------------------------------------------------------------------------------------

TEST(ScriptedWorker, StopJoinsARunningWorker) {
    Worker worker;
    std::atomic<int> ticks{0};
    ASSERT_TRUE(worker.Start([&worker, &ticks] {
        while (!worker.StopRequested()) {
            ticks.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    EXPECT_TRUE(worker.Running());
    for (int i = 0; i < 2000 && ticks.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(ticks.load(), 0);
    EXPECT_TRUE(worker.Stop(std::chrono::seconds(5)));
    EXPECT_FALSE(worker.Running());
    // Stopping again (or before ever starting) is safe and does not hang.
    EXPECT_TRUE(worker.Stop(std::chrono::seconds(1)));
}

TEST(ScriptedWorker, StopBeforeStartIsSafe) {
    Worker worker;
    EXPECT_TRUE(worker.Stop(std::chrono::seconds(1)));
    EXPECT_FALSE(worker.Running());
}

TEST(ScriptedWorker, SecondStartIsRefusedWhileRunning) {
    Worker worker;
    std::atomic<bool> seen{false};
    ASSERT_TRUE(worker.Start([&worker, &seen] {
        seen = true;
        while (!worker.StopRequested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));
    EXPECT_FALSE(worker.Start([] {}));
    for (int i = 0; i < 2000 && !seen.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(worker.Stop(std::chrono::seconds(5)));
}

TEST(ScriptedWorker, AWorkerCanRunAgainAfterItStops) {
    Worker worker;
    for (int run = 0; run < 3; ++run) {
        std::atomic<bool> ran{false};
        ASSERT_TRUE(worker.Start([&ran] { ran = true; }));
        for (int i = 0; i < 2000 && !ran.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(ran.load());
        EXPECT_TRUE(worker.Stop(std::chrono::seconds(5)));
    }
}

TEST(ScriptedWorker, BoundedStopLeavesAStuckWorkerBehind) {
    // A body that ignores the stop flag stands in for a thread stuck in a driver call: the stop
    // must return in bounded time instead of deadlocking the exit.
    struct Stuck {
        std::atomic<bool> release{false};
        std::atomic<bool> done{false};
    };
    auto stuck = std::make_shared<Stuck>();
    Worker worker;
    ASSERT_TRUE(worker.Start([stuck] {
        while (!stuck->release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stuck->done = true;
    }));
    EXPECT_FALSE(worker.Stop(std::chrono::milliseconds(30)));
    // Let the detached body finish before the test returns, so it reads no dead state.
    stuck->release = true;
    for (int i = 0; i < 4000 && !stuck->done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(stuck->done.load());
}

} // namespace
