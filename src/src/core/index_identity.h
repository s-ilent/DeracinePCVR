// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "core/index_identity_contract.h"

namespace Core {

class Module;

namespace IndexIdentity {

/// Checks the opt-in and the exact image identity, then records the execution identity. The
/// observer itself is installed by the RHI entry probe, which already has the validated hook.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

/// The execution identity for this process. Every record carries it, so two records from different
/// runs can never be joined however well their fields agree.
[[nodiscard]] u64 ExecutionId() noexcept;

/// Records the recovered identity for one draw.
///
/// Called from the RHI entry observer, which has already validated the guest pointers. The values
/// are copied by value: nothing here retains a pointer into guest memory for later dereferencing.
void NoteIdentity(const OriginalIndexResourceIdentity& identity) noexcept;

/// Looks up the identity for a draw in this execution. Returns false when none was recorded.
[[nodiscard]] bool FindByProvenance(u64 provenance_id, OriginalIndexResourceIdentity& out) noexcept;

/// Number of retained records, or 0 when the observer is not enabled.
[[nodiscard]] u32 RetainedCount() noexcept;

/// Copies one retained record by index.
[[nodiscard]] bool GetRetained(u32 index, OriginalIndexResourceIdentity& out) noexcept;

[[nodiscard]] bool IsEnabled() noexcept;

/// Writes the retained records and a summary. Called after the guest has stopped.
void ExportIdentities() noexcept;

} // namespace IndexIdentity
} // namespace Core
