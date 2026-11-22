// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core {

class Module;

namespace RhiCommandCapture {

/// Checks the opt-in, exact image identity, executable PT_LOAD, and each command prologue, then
/// installs bounded passive observers on the five recovered command Execute wrappers before the
/// main guest thread starts. Installation is transactional: a failure restores earlier observers.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

/// Emits the captured records and the summary. Called after the guest has stopped, so the export
/// is outside the guest execution hot path. Does nothing when the capture is not installed.
void ExportCapturedCommands();

/// The deferred command currently executing on this thread, if any. The RHI entry observer uses
/// it to connect a PS4 RHI invocation to the UE4 command that issued it. Returns false when no
/// captured command is in scope, which is the normal case for a direct RHI call that bypassed the
/// recovered deferred wrapper.
struct CurrentCommand {
    std::uint64_t capture_id{};
    std::uint8_t command_type{};
    std::uint64_t command_list{};
    std::uint64_t rhi_context{};
    std::uint32_t ue4_primitive_type{};
    std::uint64_t index_resource{};
};
[[nodiscard]] bool CurrentDeferredCommand(CurrentCommand& out) noexcept;

/// Clears the deferred-command scope for this thread. Called by the RHI entry observer once it has
/// claimed the identity, so a second invocation cannot inherit the same command.
void ConsumeDeferredCommand() noexcept;

} // namespace RhiCommandCapture
} // namespace Core
