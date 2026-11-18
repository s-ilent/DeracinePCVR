// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "imgui/renderer/imgui_exit_guard.h"

using ImGui::Core::ExitSaveGuard;

// The two possible last moments of the ImGui settings are Shutdown (context still alive) and the
// std::at_quick_exit callback (context destroyed by Shutdown, or still alive on the ordinary
// exit). Whichever comes first must save exactly once; the other must do nothing - that is what
// stops the quick-exit callback from calling GetIO() after DestroyContext.

TEST(ImGuiExitGuard, ShutdownSavesOnceAndDisarmsQuickExit) {
    ExitSaveGuard guard;
    int saves = 0;
    guard.SaveBeforeDestroy([&] { ++saves; });
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(guard.Saved());
    EXPECT_TRUE(guard.Destroyed());

    // The at_quick_exit callback runs afterwards: it must not save again, and the caller's
    // null-context check would stop it before touching ImGui anyway.
    guard.SaveOnQuickExit([&] { ++saves; });
    EXPECT_EQ(saves, 1);

    // A second Shutdown must not save or touch the device/context again.
    guard.SaveBeforeDestroy([&] { ++saves; });
    EXPECT_EQ(saves, 1);
}

TEST(ImGuiExitGuard, QuickExitSavesOnceThenShutdownDoesNot) {
    ExitSaveGuard guard;
    int saves = 0;
    guard.SaveOnQuickExit([&] { ++saves; });
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(guard.Saved());
    EXPECT_FALSE(guard.Destroyed());

    guard.SaveBeforeDestroy([&] { ++saves; });
    EXPECT_EQ(saves, 1);
    EXPECT_TRUE(guard.Destroyed());

    guard.SaveOnQuickExit([&] { ++saves; });
    EXPECT_EQ(saves, 1);
}

TEST(ImGuiExitGuard, QuickExitAfterDestroyedContextIsANoOp) {
    ExitSaveGuard guard;
    int saves = 0;
    guard.SaveBeforeDestroy([&] { ++saves; });
    guard.SaveOnQuickExit([&] { ++saves; });
    guard.SaveOnQuickExit([&] { ++saves; });
    EXPECT_EQ(saves, 1);
}

TEST(ImGuiExitGuard, NoSaveIfNothingEverAsks) {
    ExitSaveGuard guard;
    EXPECT_FALSE(guard.Saved());
    EXPECT_FALSE(guard.Destroyed());
}
