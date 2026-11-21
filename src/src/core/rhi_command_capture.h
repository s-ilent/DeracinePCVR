// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core {

class Module;

namespace RhiCommandCapture {

/// Checks the opt-in, exact image identity, executable PT_LOAD, and each command prologue, then
/// installs bounded passive observers on the five recovered command Execute wrappers before the
/// main guest thread starts.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

} // namespace RhiCommandCapture
} // namespace Core
