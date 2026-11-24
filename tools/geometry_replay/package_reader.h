// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Shared reader for the SC-10S resource package.
//
// Two replays consume this format: the diagnostic textured replay and the semantic tape replay.
// Keeping one reader means a field added to the package is read the same way by both, and a
// validation rule cannot drift between them.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace package {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct Resource {
    u32 id{};
    u64 clamped_size{};
    u64 content_hash{};
    u64 blob_offset{};
    u64 byte_length{};
    std::vector<u8> bytes;
};

struct VertexBinding {
    u32 binding{};
    u32 stride{};
    u32 resource_ref{};
    u32 semantic{};
    u32 vk_format{};
    u32 attr_offset{};
    u32 element_count{};
};

struct IndexBinding {
    u32 index_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 resource_ref{};
    u64 guest_base{};
    u64 guest_size{};
};

struct TextureSubresource {
    u32 mip{};
    u32 layer{};
    u32 width{};
    u32 height{};
    u32 row_pitch{};
    u64 blob_offset{};
    u64 byte_length{};
    u64 content_hash{};
};

struct Texture {
    u32 id{};
    u32 vk_binding{};
    u32 image_format{};
    u32 view_format{};
    u32 width{};
    u32 height{};
    u32 block_columns{};
    u32 block_rows{};
    u32 block_texels_x{1};
    u32 block_texels_y{1};
    u32 bytes_per_block{};
    u64 transfer_id{};
    u64 sampler_id{};
    bool sampler_verified{};
    std::vector<TextureSubresource> subresources;
};

struct Sampler {
    u64 id{};
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
};

struct TextureCoordinate {
    u32 binding{};
    u32 component_u{};
    u32 component_v{};
    bool looks_like_uv{};
};

struct Geometry {
    u32 position_binding{};
    u32 position_components{};
    bool is_three_dimensional{};
    float bounds_min[3]{};
    float bounds_max[3]{};
};

struct Readiness {
    bool geometry_ready{};
    bool texture_binding_ready{};
    bool texture_contents_ready{};
    bool sampler_ready{};
    bool diagnostic_textured_replay_ready{};
};

struct Package {
    u32 schema_version{};
    /// The capture session the package was exported in. Zero for a legacy package.
    u64 session_id{};
    u64 provenance_id{};
    bool is_indexed{};
    std::vector<Resource> resources;
    std::vector<VertexBinding> vertex_bindings;
    bool has_index{};
    IndexBinding index{};
    Geometry geometry;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    TextureCoordinate uv;
    Readiness readiness;
    /// The texture payload sidecar, kept separate from the geometry blob.
    std::vector<u8> texture_blob;
};

/// Reads a package directory. Returns false with a reason when a required field is absent or the
/// JSON is malformed, rather than producing a partially populated package.
[[nodiscard]] bool Read(const std::string& directory, Package& out, std::string& error);

/// The resource with the given id, or nullptr.
[[nodiscard]] const Resource* FindResource(const Package& package, u32 id);

/// The texture with captured content, or nullptr when none has any.
[[nodiscard]] const Texture* FindTextured(const Package& package);

/// The sampler a texture pairs with, or nullptr when the association is unresolved.
[[nodiscard]] const Sampler* FindSampler(const Package& package, const Texture& texture);

} // namespace package
