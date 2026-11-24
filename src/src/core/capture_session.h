// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace Core::CaptureSession {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Opt-in for the capture session. Default OFF.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_CAPTURE_SESSION";

/// The output directory for the session manifest. Required when the session is enabled.
inline constexpr std::string_view DirectoryEnvironmentVariable =
    "SHADPS4_DERACINE_CAPTURE_SESSION_DIR";

/// Initialises the session identity. Called once, before any observer captures data.
///
/// The identity is generated from a process-specific nonce rather than a wall-clock timestamp
/// alone, because two runs can start within the same clock tick. It is not a security token: its
/// only job is to let artifacts prove they claim the same execution.
void TryInstall() noexcept;

/// The session identity for this process, or 0 when the session is not enabled.
///
/// Every exporter reads this same value. Generating a separate id inside each exporter would make
/// the shared-session check meaningless.
[[nodiscard]] u64 SessionId() noexcept;

/// Whether the session is enabled.
[[nodiscard]] bool IsEnabled() noexcept;

/// Writes the session manifest. Called after the guest has stopped.
void ExportManifest() noexcept;

} // namespace Core::CaptureSession
