// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core::TextureCapture {

/// Opt-in for the effective texture and sampler observation. Default OFF: with the variable unset,
/// absent or anything other than "1", no record is produced and no image is read.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_TEXTURE_CAPTURE";

/// Bounded number of distinct (draw, binding) image records.
inline constexpr size_t MaxImageRecords = 256;

/// Bounded bytes of image content retained per record. A title-screen texture is far smaller than
/// this; a larger one is reported as partial rather than silently truncated. The bound covers a
/// 1024x1024 RGBA8 image, which is the largest a scene mesh's material commonly uses.
inline constexpr size_t MaxBytesPerImage = 8 * 1024 * 1024;

/// Bounded number of distinct sampler records.
inline constexpr size_t MaxSamplerRecords = 128;

/// Bounded number of draws whose images are snapshotted per run. The first draws with a sampled
/// image are not necessarily the ones the package selects, so several are taken. The cost is one
/// synchronized copy per draw, which is why the bound is small rather than unlimited.
inline constexpr u32 MaxSnapshotDraws = 24;

/// How a texture binding's content was established.
enum class ContentStatus : u8 {
    /// The binding was observed but no content was read.
    BindingOnly = 0,
    /// The image's subresources were copied through a synchronized staging transfer.
    HostContentCaptured = 1,
    /// The copy was scheduled but its completion was not established.
    CopyPending = 2,
    /// The image cannot legally be copied, for example a depth or multisampled target.
    Unsupported = 3,
    /// The copy covered only part of the requested subresources.
    Partial = 4,
    /// The record is internally inconsistent.
    Invalid = 5,
};

[[nodiscard]] const char* StatusName(u8 status) noexcept;

/// Content hash of a captured byte range. The same function the shader-buffer capture uses, so a
/// reader has one hash to trust.
[[nodiscard]] inline u64 HashBytes(const u8* data, size_t size) noexcept {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// The effective sampler state, as the production sampler construction produced it. This is the
/// state the VkSampler was built from, not a guess at material defaults.
struct SamplerRecord {
    u64 id{};
    /// The guest S# descriptor hash the cache keyed the sampler on.
    u64 guest_hash{};
    /// The guest descriptor words, kept so the state can be re-derived independently.
    std::array<u32, 4> guest_words{};

    u32 mag_filter{};
    u32 min_filter{};
    u32 mip_filter{};
    u32 address_u{};
    u32 address_v{};
    u32 address_w{};
    float lod_bias{};
    float min_lod{};
    float max_lod{};
    bool anisotropy_enable{};
    float max_anisotropy{};
    bool compare_enable{};
    u32 compare_op{};
    u32 border_color{};
    bool unnormalized_coordinates{};
    /// The guest's own requested anisotropy, before any host cap.
    float guest_max_aniso{};
};

/// One effective image binding, recorded where the production renderer binds it.
struct ImageRecord {
    u64 provenance_id{};
    u64 stage_hash{};
    u32 stage{};
    /// The unified Vulkan descriptor binding number the descriptor write used.
    u32 vk_binding{};
    /// The guest T# descriptor's logical slot and address, for correlation only.
    u32 guest_slot{};
    u64 guest_address{};

    /// The texture-cache image this resolved to, and its generation.
    u32 image_id{};
    u64 image_uid{};
    /// The Vulkan image and view the descriptor was built from.
    u64 vk_image{};
    u64 vk_image_view{};

    /// The image's own format and the view's format, which may differ.
    u32 image_format{};
    u32 view_format{};
    u32 component_mapping{};
    u32 aspect{};
    u32 mip_first{};
    u32 mip_count{};
    u32 layer_first{};
    u32 layer_count{};
    u32 descriptor_layout{};

    /// Dimensions of the base mip.
    u32 width{};
    u32 height{};
    u32 depth{};
    u32 samples{};

    /// The sampler this binding pairs with, or zero.
    u64 sampler_id{};

    u8 status{};
    /// Retained content, in the view's format, tightly packed per subresource.
    std::vector<u8> bytes;
    u64 content_hash{};
    /// Bytes per row of the base mip, for a reader that needs the layout.
    u32 row_pitch{};
    /// How many subresources the copy covered.
    u32 subresources_captured{};
    u32 subresources_requested{};
};

/// Whether the observation is opted in.
[[nodiscard]] bool Enabled() noexcept;

/// Whether a snapshot should be taken now. The capture takes a bounded number of draws' images
/// per run, so the cost stays bounded and the run stays comparable to an uninstrumented one.
[[nodiscard]] bool ShouldSnapshot() noexcept;

/// Whether any image record for this draw carries captured content. The package selection uses
/// this to prefer a draw whose texture was actually captured.
[[nodiscard]] bool HasContentFor(u64 provenance_id) noexcept;

/// Commits the pending image records for the selected draw and marks the snapshot as taken. Only
/// that draw's records are kept, so the record budget holds the selected draw rather than the
/// first few draws the title screen binds.
void CommitPendingImages(u64 provenance_id) noexcept;

/// Drops the pending records for a draw that was not selected.
void DiscardPendingImages() noexcept;

/// Stores a record whose content arrived after the draw was committed, which is the case for a
/// snapshot readback: the copy completes later, so the record cannot go through the per-draw
/// pending list.
void StoreLateImage(ImageRecord record) noexcept;

/// Clears the per-draw records. Called when a new draw begins.
void BeginDraw() noexcept;

/// Records one effective image binding. Called from the production binding loop, after the
/// descriptor's image and view are known.
void NoteImageBinding(const ImageRecord& record) noexcept;

/// Records one effective sampler. Called from the production sampler loop.
void NoteSampler(const SamplerRecord& record) noexcept;

/// Hands the collected records to the export.
void EndDraw() noexcept;

/// Test seam: the number of image records taken so far.
[[nodiscard]] size_t ImageCount() noexcept;

/// Test seam: the image records taken so far.
[[nodiscard]] std::span<const ImageRecord> Images() noexcept;

/// Test seam: the sampler records taken so far.
[[nodiscard]] std::span<const SamplerRecord> Samplers() noexcept;

/// Test seam: clears the collector. Not called by the emulator.
void ResetForTesting() noexcept;

} // namespace Core::TextureCapture
