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

/// Registers a successfully constructed DrawIndex2 packet against the provenance record that
/// produced it. Called only after the packet was actually written, so a rejected GNM call cannot
/// create a packet association. `cmdbuf` is the guest buffer the packet was written into.
void RegisterConstructedPacket(std::uintptr_t cmdbuf, std::uint32_t index_count,
                               std::uintptr_t index_addr) noexcept;

/// Records the submission that carries a DCB, so packet provenance is generation-aware. `dcb_data`
/// is the submitted span after any copy, and `copied` says whether the copy path was taken.
/// Returns the submission identity to carry into the graphics task, or 0 when provenance is off.
std::uint64_t NoteSubmission(const void* dcb_data, std::size_t dcb_words,
                             std::uintptr_t original_dcb, bool copied) noexcept;

/// Captures a decoded PM4 DrawIndex2 packet, when installed. Runs on the GPU thread.
void CapturePm4DrawIndex2(std::uint32_t max_size, std::uint32_t index_base_lo,
                          std::uint32_t index_base_hi, std::uint32_t index_count,
                          std::uint32_t draw_initiator) noexcept;

/// Captures a decoded PM4 DrawIndex2 packet together with its position in the submitted DCB, so
/// the packet can be matched to the record that constructed it. `submission_id` and
/// `source_buffer` are the identity assigned by the submission that queued this DCB; a decode
/// without them cannot be resolved exactly and is counted as unmatched.
void CapturePm4DrawIndex2At(std::uint32_t max_size, std::uint32_t index_base_lo,
                            std::uint32_t index_base_hi, std::uint32_t index_count,
                            std::uint32_t draw_initiator, std::uintptr_t packet_address,
                            std::uintptr_t dcb_base, std::uint64_t submission_id,
                            std::uintptr_t source_buffer) noexcept;

/// Records that the rasterizer processed a draw for the given provenance record, with the actual
/// pipeline state observed at draw time.
void NoteDrawRecorded(std::uint64_t record_id, std::uint64_t vs_hash, std::uint64_t ps_hash,
                      std::uint64_t pipeline_hash, std::uint32_t index_count,
                      std::uint32_t instance_count, std::uint32_t primitive_type,
                      std::uint64_t index_addr, std::uint32_t color_mask,
                      std::uint64_t depth_address, bool depth_test, bool depth_write) noexcept;

/// Records that a decoded packet produced no rasterizer draw, with the reason.
void NoteDrawSkipped(std::uint64_t record_id, const char* reason) noexcept;

/// Returns the provenance record id currently associated with the draw being processed, or 0.
[[nodiscard]] std::uint64_t CurrentDrawProvenanceId() noexcept;

/// Clears the current draw identity for this thread. Called once the draw has been reported, so a
/// later unrelated draw cannot inherit it.
void ClearCurrentDrawProvenance() noexcept;

/// Writes the provenance summary and per-record lines. Runs after the guest has stopped.
void ExportProvenance() noexcept;

} // namespace RhiEntryProbe
} // namespace Core
