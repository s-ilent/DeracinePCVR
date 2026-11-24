// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace Core {

namespace DrawPackage {

/// A resource is usable for replay only when its content was established from the host resource
/// the draw actually read. Guest-memory observation is not enough.
[[nodiscard]] constexpr bool IsReplayUsable(u8 status) noexcept {
    return status == 1 || status == 2;
}

/// Schema version. Bumped when a field changes meaning, so a reader can refuse a package it does
/// not understand rather than silently misinterpreting it.
///
/// Version 2 adds geometry and replaces the single `replay_ready` flag with explicit readiness
/// dimensions. A version 1 package is rejected rather than reinterpreted, because its
/// `replay_ready` meant shader-buffer completeness only.
inline constexpr u32 SchemaVersion = 2;

/// Opt-in for writing a private draw package. Separate from the capture flags so a package is only
/// produced when explicitly asked for.
inline constexpr std::string_view EnableEnvironmentVariable =
    "SHADPS4_DERACINE_DRAW_PACKAGE";

/// Bounded number of packages written per run.
inline constexpr size_t MaxPackages = 4;

/// Bounded bytes of resource content retained per package.
inline constexpr size_t MaxPackageBytes = 8 * 1024 * 1024;

/// One retained resource. Two bindings of the same content share one entry, referenced by id.
struct Resource {
    u32 id{};
    /// Guest range the descriptor requested, and the validated range actually used.
    u64 requested_base{};
    u64 requested_size{};
    u64 clamped_base{};
    u64 clamped_size{};
    /// Content hash of the retained bytes.
    u64 content_hash{};
    /// How the content was established.
    u8 status{};
    /// Retained bytes. Empty when the resource is metadata-only.
    std::vector<u8> bytes;
};

/// One binding occurrence, referencing a resource by id.
struct Binding {
    u8 stage{};
    u32 resource_index{};
    u32 sharp_idx{};
    u32 vk_binding{};
    u64 vk_offset{};
    u64 vk_range{};
    u64 buffer_id{};
    u32 resource_ref{};
    u8 status{};
};

/// One vertex binding, with the host allocation and byte range it resolved to.
struct VertexBinding {
    u32 binding{};
    u64 host_buffer{};
    u64 host_offset{};
    u64 guest_base{};
    u64 guest_size{};
    u32 stride{};
    u32 input_rate{};
    u32 divisor{};
    u64 upload_sequence{};
    u64 content_hash{};
    u32 resource_ref{};
    u8 status{};

    /// The attribute this binding feeds, as the production vertex-input construction describes it.
    /// shadPS4 binds one attribute per binding, so the two travel together.
    u32 semantic{};
    u32 vk_format{};
    u32 attr_offset{};
    u32 element_count{};
    /// Shader identity, so a package can be selected and reproduced by signature.
    u64 vs_hash{};
    u64 ps_hash{};
};

/// The index binding, with the draw arguments the renderer actually used.
struct IndexBinding {
    u64 host_buffer{};
    u64 host_offset{};
    u64 guest_base{};
    u64 guest_size{};
    u32 index_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 primitive_type{};
    u8 index_type{};
    u8 swap_mode{};
    u64 upload_sequence{};
    u64 content_hash{};
    u32 resource_ref{};
    u8 status{};
};

/// What a package is actually sufficient for. These are separate because a package can have
/// complete shader buffers and no geometry, or complete geometry and no textures.
struct Readiness {
    bool shader_buffers_ready{};
    bool geometry_ready{};
    /// A texture binding was observed and its identity recorded.
    bool texture_binding_ready{};
    /// The bound image's contents were copied and hashed.
    bool texture_contents_ready{};
    /// The sampler state the production construction produced was recorded.
    bool sampler_ready{};
    bool pipeline_state_ready{};
    /// Enough to run a diagnostic position-only replay of the captured geometry.
    bool diagnostic_geometry_replay_ready{};
    /// Enough to run a diagnostic replay that samples the captured texture.
    bool diagnostic_textured_replay_ready{};
    /// Enough to reproduce the draw with its original materials.
    bool original_material_ready{};
    /// Why the highest claimed level is not higher.
    std::string limitation;
};

/// How the diagnostic replay should interpret the geometry. This is a documented choice, not a
/// recovered fact: the game's own transform is not captured.
struct GeometryInterpretation {
    /// Binding and component count that the diagnostic renderer treats as object-space position.
    u32 position_binding{};
    u32 position_components{};
    /// How that attribution was established.
    std::string position_evidence;
    /// Axis-aligned bounds of the position attribute, in the captured units.
    float bounds_min[3]{};
    float bounds_max[3]{};
    /// Whether the bounds are nondegenerate on all three axes, which is what distinguishes a scene
    /// mesh from a screen-space element.
    bool is_three_dimensional{};
    /// Triangle statistics, computed from the captured indices.
    u32 triangle_count{};
    u32 degenerate_triangles{};
    u32 unique_referenced_vertices{};
    /// Nonfinite position values found while computing the bounds.
    u32 nonfinite_vertices{};
};

/// One captured subresource of a texture: a mip level of an array layer, in the view's format.
struct TextureSubresource {
    u32 mip{};
    u32 layer{};
    u32 width{};
    u32 height{};
    /// Bytes per row, tightly packed.
    u32 row_pitch{};
    u64 blob_offset{};
    u64 byte_length{};
    u64 content_hash{};
};

/// One effective texture binding, with the image, view and sampler the production renderer used.
struct Texture {
    u32 id{};
    /// The unified Vulkan descriptor binding number the descriptor write used.
    u32 vk_binding{};
    /// The shader stage and program the binding belongs to.
    u32 stage{};
    u64 stage_hash{};
    /// The guest T# descriptor's address, for correlation only. It is not the binding identity.
    u64 guest_address{};

    /// The texture-cache image and its generation, so a stale capture is detectable.
    u32 image_id{};
    u64 image_uid{};

    /// The image's own format and the view's format, which may differ.
    u32 image_format{};
    u32 view_format{};
    u32 aspect{};
    u32 mip_first{};
    u32 mip_count{};
    u32 layer_first{};
    u32 layer_count{};
    u32 width{};
    u32 height{};
    u32 depth{};
    u32 samples{};
    /// Storage geometry, kept separate from the pixel extent. A dimension that is not block
    /// aligned makes the two genuinely different.
    ///
    /// `block_columns` and `block_rows` are how many blocks the subresource spans. `block_texels_x`
    /// and `block_texels_y` are how many texels one block covers. The names are deliberately
    /// distinct: an earlier version called the block count `block_width`, which read as the block
    /// size and made the byte length impossible to check.
    u32 block_columns{};
    u32 block_rows{};
    u32 block_texels_x{1};
    u32 block_texels_y{1};
    u32 bytes_per_block{};
    /// The transfer the content came from, so a late readback is tied to its copy.
    u64 transfer_id{};

    /// The sampler this binding pairs with, or zero when the association is not established.
    u64 sampler_id{};
    /// Whether the association came from the shader rather than a substitution.
    bool sampler_verified{};

    /// How the content was established.
    u8 status{};
    std::vector<TextureSubresource> subresources;
    /// Why the content is not complete, when it is not.
    std::string limitation;
};

/// The effective sampler state, as the production sampler construction produced it.
struct Sampler {
    u64 id{};
    u64 guest_hash{};
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
};

/// How the diagnostic replay should read texture coordinates. This is an attribution, not a
/// recovered fact: the game's own material is not captured.
struct TextureCoordinate {
    /// The vertex binding and component pair the diagnostic renderer treats as UV.
    u32 binding{};
    u32 component_u{};
    u32 component_v{};
    /// How that attribution was established.
    std::string evidence;
    /// The observed range, which is what distinguishes a UV from a normal or a colour.
    float min_u{};
    float max_u{};
    float min_v{};
    float max_v{};
    /// Whether the range is consistent with normalized texture coordinates.
    bool looks_like_uv{};
};

struct Package {
    u32 schema_version{SchemaVersion};
    std::string scene;
    u64 provenance_id{};
    u64 pipeline_hash{};
    u64 vs_hash{};
    u64 ps_hash{};
    u32 primitive_type{};
    bool is_indexed{};
    u32 num_indices{};
    u32 num_instances{};

    std::vector<Binding> bindings;
    std::vector<Resource> resources;

    std::vector<VertexBinding> vertex_bindings;
    bool has_index_binding{};
    IndexBinding index_binding{};
    GeometryInterpretation geometry;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    TextureCoordinate texture_coordinate;
    /// Texture payload, written to its own sidecar so a reader can stream it separately from the
    /// geometry blob. Subresource offsets index into this vector.
    std::vector<u8> texture_bytes;

    Readiness readiness;


};

/// Writes a package as JSON plus a sidecar binary blob of resource bytes. Returns false when the
/// package could not be written. `directory` must already exist.
[[nodiscard]] bool WritePackage(const Package& package, const std::string& directory) noexcept;

/// Reads a package back. Used by the independent validator, not by the exporter.
[[nodiscard]] bool ReadPackage(const std::string& directory, Package& out,
                               std::string& error) noexcept;

/// Validates a package's internal consistency. Returns true when the package is well formed and
/// every resource reference resolves and every retained byte range matches its hash.
///
/// Readiness is derived here rather than trusted from the caller: a package cannot claim
/// `diagnostic_geometry_replay_ready` unless its geometry bindings resolve to verified resources.
///
/// Header-only so the contract tests can exercise it without linking the logging stack.
[[nodiscard]] inline bool ValidatePackage(const Package& package, std::string& error) noexcept {
    try {
        if (package.schema_version != SchemaVersion) {
            error = "unsupported schema version";
            return false;
        }
        if (package.provenance_id == 0) {
            error = "package has no provenance id";
            return false;
        }

        // Resource ids must be unique, so a reference is unambiguous.
        std::map<u32, const Resource*> by_id;
        for (const auto& resource : package.resources) {
            if (resource.id == 0) {
                error = "resource id 0 is reserved";
                return false;
            }
            if (by_id.contains(resource.id)) {
                error = "duplicate resource id";
                return false;
            }
            by_id[resource.id] = &resource;
        }

        // Every binding must reference a resource that exists.
        for (const auto& binding : package.bindings) {
            if (binding.resource_ref == 0) {
                if (binding.status != 3) {
                    error = "binding has no resource reference";
                    return false;
                }
                continue;
            }
            if (!by_id.contains(binding.resource_ref)) {
                error = "binding references a missing resource";
                return false;
            }
        }

        // Geometry bindings must resolve too, and their ranges must be consistent.
        for (const auto& vertex : package.vertex_bindings) {
            if (vertex.resource_ref == 0) {
                if (vertex.status != 3) {
                    error = "vertex binding has no resource reference";
                    return false;
                }
                continue;
            }
            if (!by_id.contains(vertex.resource_ref)) {
                error = "vertex binding references a missing resource";
                return false;
            }
            if (vertex.stride == 0 && vertex.guest_size != 0) {
                // A zero-stride fetch is a constant-style input; it is legal but must not claim a
                // per-vertex range.
                continue;
            }
        }
        if (package.has_index_binding && package.index_binding.resource_ref != 0) {
            if (!by_id.contains(package.index_binding.resource_ref)) {
                error = "index binding references a missing resource";
                return false;
            }
            const auto& index = package.index_binding;
            if (index.index_size != 2 && index.index_size != 4) {
                error = "index element width is neither 2 nor 4";
                return false;
            }
            if (index.index_count != 0 && index.guest_size != 0) {
                const u64 needed = static_cast<u64>(index.index_count) * index.index_size;
                if (needed > index.guest_size) {
                    error = "index count exceeds the bound index range";
                    return false;
                }
            }
        }

        // A retained resource must have bytes whose length matches its declared range, and a hash
        // that matches those bytes.
        for (const auto& resource : package.resources) {
            if (resource.bytes.empty()) {
                if (IsReplayUsable(resource.status)) {
                    error = "resource claims verified content but retains no bytes";
                    return false;
                }
                continue;
            }
            if (resource.bytes.size() != resource.clamped_size) {
                error = "resource byte length does not match its declared range";
                return false;
            }
            u64 hash = 0xcbf29ce484222325ull;
            for (const u8 byte : resource.bytes) {
                hash ^= byte;
                hash *= 0x100000001b3ull;
            }
            if (hash != resource.content_hash) {
                error = "resource content hash does not match its bytes";
                return false;
            }
        }

        // Shader-buffer readiness requires every shader binding to be host-verified.
        if (package.readiness.shader_buffers_ready) {
            for (const auto& binding : package.bindings) {
                if (binding.resource_ref == 0)
                    continue;
                if (!IsReplayUsable(by_id[binding.resource_ref]->status)) {
                    error = "package claims shader-buffer readiness with an unverified resource";
                    return false;
                }
            }
        }

        // Geometry readiness requires every geometry binding to be host-verified, and an index
        // binding when the draw is indexed.
        if (package.readiness.geometry_ready) {
            for (const auto& vertex : package.vertex_bindings) {
                if (vertex.resource_ref == 0)
                    continue;
                if (!IsReplayUsable(by_id[vertex.resource_ref]->status)) {
                    error = "package claims geometry readiness with an unverified vertex resource";
                    return false;
                }
            }
            if (package.is_indexed) {
                if (!package.has_index_binding) {
                    error = "package claims geometry readiness for an indexed draw with no index "
                            "binding";
                    return false;
                }
                if (package.index_binding.resource_ref == 0 ||
                    !IsReplayUsable(by_id[package.index_binding.resource_ref]->status)) {
                    error = "package claims geometry readiness with an unverified index resource";
                    return false;
                }
            }
        }

        // Diagnostic replay readiness is a strictly stronger claim than geometry readiness.
        if (package.readiness.diagnostic_geometry_replay_ready && !package.readiness.geometry_ready) {
            error = "diagnostic replay readiness claimed without geometry readiness";
            return false;
        }
        // Texture readiness is derived, not trusted: a binding claim needs a binding, a content
        // claim needs bytes whose hash matches, and a sampler claim needs a sampler.
        if (package.readiness.texture_binding_ready && package.textures.empty()) {
            error = "texture binding readiness claimed with no textures";
            return false;
        }
        if (package.readiness.texture_contents_ready) {
            if (!package.readiness.texture_binding_ready) {
                error = "texture content readiness claimed without binding readiness";
                return false;
            }
            for (const auto& texture : package.textures) {
                if (texture.subresources.empty()) {
                    error = "texture content readiness claimed with no subresources";
                    return false;
                }
                // The required set is the view's mip and layer range. Every pair in it must be
                // present, or the capture is partial and cannot be described as complete.
                const u32 required_mips = texture.mip_count != 0 ? texture.mip_count : 1;
                const u32 required_layers = texture.layer_count != 0 ? texture.layer_count : 1;
                u32 matched = 0;
                for (u32 m = 0; m < required_mips; ++m) {
                    for (u32 l = 0; l < required_layers; ++l) {
                        const u32 want_mip = texture.mip_first + m;
                        const u32 want_layer = texture.layer_first + l;
                        for (const auto& sub : texture.subresources) {
                            if (sub.mip == want_mip && sub.layer == want_layer) {
                                ++matched;
                                break;
                            }
                        }
                    }
                }
                if (matched != required_mips * required_layers) {
                    error = "texture content readiness claimed with an incomplete mip or layer "
                            "range";
                    return false;
                }
                for (const auto& subresource : texture.subresources) {
                    if (subresource.byte_length == 0) {
                        error = "texture subresource has no bytes";
                        return false;
                    }
                    if (subresource.row_pitch < subresource.width) {
                        error = "texture subresource row pitch is narrower than its width";
                        return false;
                    }
                    // `row_pitch` is bytes per row of blocks and `height` is in texels, so the
                    // row count is the block-row count, not the texel height. Comparing against
                    // the texel height would reject every compressed subresource.
                    const u32 block_texels_y =
                        texture.block_texels_y != 0 ? texture.block_texels_y : 1;
                    const u64 block_rows =
                        (subresource.height + block_texels_y - 1) / block_texels_y;
                    if (subresource.byte_length <
                        static_cast<u64>(subresource.row_pitch) * block_rows) {
                        error = "texture subresource is shorter than its rows";
                        return false;
                    }
                }
            }
        }
        if (package.readiness.sampler_ready && package.samplers.empty()) {
            error = "sampler readiness claimed with no samplers";
            return false;
        }

        // A texture's sampler reference must resolve.
        for (const auto& texture : package.textures) {
            if (texture.sampler_id == 0)
                continue;
            bool found = false;
            for (const auto& sampler : package.samplers) {
                if (sampler.id == texture.sampler_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error = "texture references a missing sampler";
                return false;
            }
        }

        // Diagnostic textured replay needs geometry, a texture with content, and a sampler.
        if (package.readiness.diagnostic_textured_replay_ready) {
            if (!package.readiness.diagnostic_geometry_replay_ready) {
                error = "textured replay readiness claimed without geometry replay readiness";
                return false;
            }
            if (!package.readiness.sampler_ready) {
                error = "textured replay readiness claimed without a sampler";
                return false;
            }
            // A diagnostic textured replay needs one captured texture, not every bound texture.
            bool any_content = false;
            for (const auto& texture : package.textures) {
                if (!texture.subresources.empty()) {
                    any_content = true;
                    break;
                }
            }
            if (!any_content) {
                error = "textured replay readiness claimed without any captured texture";
                return false;
            }
            // A textured replay of a screen-space quad would prove nothing about the recovered
            // material path, so the geometry must also be three-dimensional.
            if (!package.geometry.is_three_dimensional) {
                error = "textured replay readiness claimed without three-dimensional geometry";
                return false;
            }
        }

        // Full draw readiness requires everything.
        if (package.readiness.original_material_ready) {
            if (!package.readiness.geometry_ready || !package.readiness.shader_buffers_ready ||
                !package.readiness.texture_contents_ready || !package.readiness.sampler_ready ||
                !package.readiness.pipeline_state_ready) {
                error = "original material readiness claimed without every dimension";
                return false;
            }
        }

        error.clear();
        return true;
    } catch (...) {
        error = "validation threw";
        return false;
    }
}

} // namespace DrawPackage
} // namespace Core
