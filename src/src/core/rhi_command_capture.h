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

/// One captured command, as the tape exporter needs it. Scalars and opaque pointer values only:
/// nothing here is dereferenced after the hook returns.
struct CapturedCommandView {
    std::uint64_t sequence{};
    std::uint64_t native_thread_id{};
    std::uint64_t guest_thread_id{};
    std::uint64_t list_pointer{};
    std::uint64_t context_pointer{};
    std::uint64_t vtable_rva{};
    std::uint64_t payload[8]{};
    std::uint64_t render_targets[8]{};
    std::uint64_t depth_target{};
    std::uint64_t uavs[8]{};
    std::uint64_t shader_state{};
    std::uint64_t shader_resource{};
    std::uint64_t program_data{};
    std::uint32_t shader_modifier{};
    std::uint8_t command_id{};
    std::uint8_t payload_words{};
    std::uint8_t render_target_count{};
    std::uint8_t uav_count{};
    std::uint8_t shader_status{};
};

/// Number of captured commands, or 0 when the capture is not installed.
[[nodiscard]] std::uint32_t CapturedCommandCount() noexcept;

/// Copies one captured command by index. Returns false when the index is out of range.
[[nodiscard]] bool GetCapturedCommand(std::uint32_t index, CapturedCommandView& out) noexcept;

} // namespace RhiCommandCapture
} // namespace Core