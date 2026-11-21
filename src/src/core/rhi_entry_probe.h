// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace Core {

class Module;

namespace RhiEntryProbe {

/// Checks the opt-in, exact image identity, executable PT_LOAD, and entry signature, then installs
/// the bounded observer before the main guest thread starts.
void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version);

/// Captures the next indexed GNM draw on the same host thread after a probe event, when installed.
void CaptureGnmDrawIndex(std::uintptr_t cmdbuf, std::uint32_t size, std::uint32_t index_count,
                         std::uintptr_t index_addr, std::uint32_t flags,
                         std::uint32_t type) noexcept;

/// Captures a decoded PM4 DrawIndex2 packet, when installed. Runs on the GPU thread.
void CapturePm4DrawIndex2(std::uint32_t max_size, std::uint32_t index_base_lo,
                          std::uint32_t index_base_hi, std::uint32_t index_count,
                          std::uint32_t draw_initiator) noexcept;

} // namespace RhiEntryProbe
} // namespace Core
