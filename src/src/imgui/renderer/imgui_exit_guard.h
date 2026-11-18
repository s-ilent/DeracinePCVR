// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace ImGui::Core {

/// Decides who saves the ImGui settings, and makes sure it happens at most once.
///
/// The settings have two possible last moments: the explicit Shutdown (when the presenter, and
/// with it the Vulkan instance, is destroyed) and the std::at_quick_exit callback registered at
/// Initialize (the ordinary process exit path, which skips static destructors). Whichever comes
/// first saves; the other must do nothing, because after Shutdown the ImGui context - and the
/// ini filename buffer - are gone, and dereferencing them from the quick-exit callback is the
/// "GImGui != NULL" assert.
///
/// Deliberately free of any ImGui or Vulkan dependency, so the lifecycle can be unit-tested
/// without creating a graphics context.
class ExitSaveGuard {
public:
    /// Shutdown: save while the context still exists, then mark it destroyed. Repeated calls
    /// (or a call after the quick-exit callback already saved) do not save again.
    template <typename SaveFn>
    void SaveBeforeDestroy(SaveFn&& save) {
        if (destroyed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (!saved.exchange(true, std::memory_order_acq_rel)) {
            save();
        }
    }

    /// std::at_quick_exit: save only if Shutdown has not already destroyed the context. The
    /// caller is responsible for not touching a destroyed context inside `save`; this guard
    /// makes the call itself safe when Shutdown came first.
    template <typename SaveFn>
    void SaveOnQuickExit(SaveFn&& save) {
        if (destroyed.load(std::memory_order_acquire)) {
            return;
        }
        if (!saved.exchange(true, std::memory_order_acq_rel)) {
            save();
        }
    }

    bool Saved() const {
        return saved.load(std::memory_order_acquire);
    }

    bool Destroyed() const {
        return destroyed.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> saved{false};
    std::atomic<bool> destroyed{false};
};

} // namespace ImGui::Core
