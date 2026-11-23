// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core {

class Module;

namespace SemanticTape {

/// Checks the opt-in and the exact image identity, then records the semantic state of the selected
/// draw. Installation is read-only: it observes the existing capture records and never installs a
/// new guest entry point, so the game's rendering path is untouched.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

/// Writes the tape for the selected draw. Called after the guest has stopped, so the export is
/// outside the guest execution hot path. Does nothing when the tape is not enabled.
void ExportTape() noexcept;

} // namespace SemanticTape
} // namespace Core
