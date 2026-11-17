// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include "common/types.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/texture_cache/image_info.h"

namespace VideoCore {

/// How much of a destination image a whole-image copy is going to write.
///
/// `Image::CopyImage(dst, src)` transfers, for every mip level below the smaller of the two level
/// counts, the source's extent of that level with its top-left corner at (0, 0) of the
/// destination, for every array layer the two images share (for 3D images: every slice up to the
/// source's depth). Anything the destination has beyond that — its own larger extent, its extra
/// levels, its extra layers — is not touched.
///
/// This is the question the texture cache asks before uploading a newly created destination image
/// from guest memory: guest memory only has to be read where the copy does not go afterwards.
struct CopyCoverage {
    /// The destination's levels [0, mips) end up entirely written by the copy.
    u32 mips = 0;
    /// Within those levels, the destination's layers [0, layers) end up written.
    u32 layers = 0;

    bool CoversNothing() const {
        return mips == 0 || layers == 0;
    }

    /// Whether destination level `level` of an image with `dst_layers` layers ends up entirely
    /// written by the copy, i.e. whether uploading it from guest memory first would be work the
    /// copy immediately throws away.
    bool CoversLevel(u32 level, u32 dst_layers) const {
        return level < mips && layers >= dst_layers;
    }
};

/// The part of `dst` that `Image::CopyImage(dst, src)` overwrites, or an empty coverage when that
/// cannot be established. An empty answer means "assume nothing is overwritten" and leaves the
/// caller with its original behaviour.
///
/// Every condition below exists to make the answer a proof rather than an estimate. The predicate
/// has to stay in step with `Image::CopyImage` and `SanitizeCopyLayers` in image.cpp: if those
/// start copying in a shape this function does not model, this function must stop answering.
///
/// Not modelled on purpose, because the copy's region is not the source's extent there:
///  * 2D source to 3D destination and the reverse (the layer count and the extent's depth trade
///    places),
///  * 1D images (Vulkan requires a height of 1, which the texel arithmetic here would not check),
///  * sources and destinations of different texel size, aspect or block-compressedness.
///
/// The copy being authoritative is a property of the caller, not of this function: creator callers
/// (see TextureCache::ExpandImage) only ask about a copy whose whole purpose is to carry the old
/// image's contents into the new one, so the copy wins over guest memory wherever it lands.
inline CopyCoverage CoveringCopyOf(const ImageInfo& src, const ImageInfo& dst) {
    enum class Kind { Unknown, Line, Plane, Volume };

    const auto kind_of = [](AmdGpu::ImageType type) {
        switch (type) {
        case AmdGpu::ImageType::Color1D:
        case AmdGpu::ImageType::Color1DArray:
            return Kind::Line;
        case AmdGpu::ImageType::Color2D:
        case AmdGpu::ImageType::Color2DMsaa:
        case AmdGpu::ImageType::Color2DArray:
            return Kind::Plane;
        case AmdGpu::ImageType::Color3D:
            return Kind::Volume;
        default:
            return Kind::Unknown;
        }
    };

    const Kind src_kind = kind_of(src.type);
    if (src_kind == Kind::Unknown || src_kind == Kind::Line || src_kind != kind_of(dst.type)) {
        return {};
    }

    // Same texel footprint and same aspect: the copy moves exactly the data the upload would have
    // moved, so a region the copy covers cannot end up with different contents than before.
    if (src.num_bits != dst.num_bits || src.num_samples != dst.num_samples) {
        return {};
    }
    // Colour only. A depth image's contents are not just the texels: the tile metadata (HTILE)
    // and the stencil aspect live beside them, and no copy here carries those across. The fork
    // that inspired this optimisation keeps its equivalent skip colour-only for the same reason,
    // and Déraciné's depth/storage aliasing goes through TextureCache::ResolveDepthOverlap
    // instead, so declining costs an upload rather than correctness.
    if (src.props.is_depth || dst.props.is_depth || src.props.has_stencil ||
        dst.props.has_stencil) {
        return {};
    }
    if (src.props.is_block != dst.props.is_block) {
        return {};
    }

    // Extents are halved per level from the base extent, so "at least as large at level 0" is
    // "at least as large at every level the copy reaches".
    if (src.size.width < dst.size.width || src.size.height < dst.size.height) {
        return {};
    }

    CopyCoverage coverage;
    coverage.mips = std::min(src.resources.levels, dst.resources.levels);
    if (src_kind == Kind::Volume) {
        if (src.size.depth < dst.size.depth) {
            return {};
        }
        // 3D copies move one slice per copy region and the depth comes from the extent.
        coverage.layers = 1;
    } else {
        coverage.layers = std::min(src.resources.layers, dst.resources.layers);
    }

    if (coverage.CoversNothing() || dst.resources.levels == 0 || dst.resources.layers == 0) {
        return {};
    }
    return coverage;
}

} // namespace VideoCore
