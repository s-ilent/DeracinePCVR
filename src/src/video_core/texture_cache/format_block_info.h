// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

/// The storage geometry of a Vulkan format.
///
/// Vulkan exposes no query for a format's block dimensions or block byte size, so a table is
/// unavoidable. This one covers exactly the formats the renderer can produce for a sampled image,
/// and reports an unknown format as unsupported rather than guessing a block size.
///
/// The distinction this type exists to keep straight: a block-compressed image's *storage* is
/// measured in blocks, but a Vulkan copy region's extent and buffer row length are measured in
/// texels. Conflating the two copies a fraction of the image.
struct FormatBlockInfo {
    /// Texels per block on each axis. One for an uncompressed format.
    u32 block_width{1};
    u32 block_height{1};
    /// Bytes one block occupies. For an uncompressed format this is the bytes per texel.
    u32 bytes_per_block{};
    bool compressed{};

    /// Whether the format is known well enough to compute a copy from.
    [[nodiscard]] bool valid() const noexcept {
        return bytes_per_block != 0;
    }

    /// Texels per block, as a count.
    [[nodiscard]] u32 texels_per_block() const noexcept {
        return block_width * block_height;
    }

    /// The block count covering `texels`, rounding up. A dimension that is not a multiple of the
    /// block size still occupies a whole block, which is why this rounds rather than divides.
    [[nodiscard]] u32 BlocksFor(u32 texels) const noexcept {
        return (texels + block_width - 1) / block_width;
    }

    [[nodiscard]] u32 BlockRowsFor(u32 texels) const noexcept {
        return (texels + block_height - 1) / block_height;
    }

    /// Bytes per row of blocks, for a tightly packed transfer of `texel_width` texels.
    [[nodiscard]] u64 RowPitchFor(u32 texel_width) const noexcept {
        return static_cast<u64>(BlocksFor(texel_width)) * bytes_per_block;
    }

    /// Total bytes for a tightly packed transfer of `texel_width` by `texel_height` texels.
    [[nodiscard]] u64 ByteSizeFor(u32 texel_width, u32 texel_height) const noexcept {
        return RowPitchFor(texel_width) * BlockRowsFor(texel_height);
    }
};

/// The block geometry of a Vulkan format, or an invalid entry when the format is not one the
/// capture supports.
[[nodiscard]] FormatBlockInfo GetFormatBlockInfo(vk::Format format) noexcept;

} // namespace VideoCore
