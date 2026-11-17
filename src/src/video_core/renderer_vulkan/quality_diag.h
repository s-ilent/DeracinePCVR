// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "common/types.h"

namespace AmdGpu {
struct ColorBuffer;
struct DepthBuffer;
} // namespace AmdGpu

namespace VideoCore {
struct Image;
}

namespace Vulkan::QualityDiag {

/// SHADPS4_QUALITY_DIAG=1: what the scene's draws have as render targets, as the guest's
/// descriptors hold them (address, format, size, sample count) and as the Vulkan images made
/// from them are (extent, format, sample count, usage), summarised once a second; how many
/// layers a pass draws; every resolve the guest asks for; and every change of the sample count
/// a guest image is held at on the host. A cvar changing says nothing about what reaches the
/// GPU: these do. Off unless the variable is exactly 1.
bool Enabled();

/// A draw's targets are about to be collected.
void BeginRenderState();

/// One of the draw's color attachments: its descriptor and the image behind it.
void NoteColorTarget(u32 index, const AmdGpu::ColorBuffer& buffer, const VideoCore::Image& image);

/// The draw's depth attachment.
void NoteDepthTarget(const AmdGpu::DepthBuffer& buffer, const VideoCore::Image& image);

/// The draw's layers and the sample counts its pipeline was made with, once they are known.
void NoteRenderState(u32 num_layers, u32 pipeline_samples, u32 pipeline_depth_samples);

/// A draw that is drawn.
void NoteDraw();

/// The color-control operation of a draw the guest issues, so a resolve or decompress pass the
/// guest asks for is seen even where the guest's own output is not what draws it.
void NoteOperation(u32 mode);

/// A guest resolve pass (CB_COLOR_CONTROL.MODE = Resolve): the two sides of it.
void NoteResolve(const VideoCore::Image& src, const VideoCore::Image& dst);

/// A guest image held on the host with a different sample count than before.
void NoteBacking(VAddr address, u32 from_samples, u32 to_samples, bool copy_backing);

/// SHADPS4_QUALITY_PROBE: values of guest memory, so a run can show what the title's own
/// settings hold rather than what was written for them. A list of `type@address` separated by
/// ';', address in guest virtual address space: u8, u32, f32, or ptr_f32 (a pointer to a
/// float, as the engine keeps a console variable's value). Polled at most once a second, only
/// for addresses that are mapped. Off unless the variable is set.
void PollProbes();

} // namespace Vulkan::QualityDiag
