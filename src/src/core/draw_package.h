// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/hash.h"
#include "common/sha256.h"
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
/// Version 3 adds the effective shader binaries and the pipeline state needed to execute the draw
/// with its original shaders. A version 2 package is refused rather than reinterpreted, because its
/// readiness flags did not distinguish "the shader binaries exist" from "the draw can run on them".
/// Version 3 adds the capture session identity, so a package can be shown to come from the same
/// execution as the semantic tape and the index bridge, and the effective shader binaries and
/// pipeline state needed to execute the draw with its original shaders.
/// Version 4 adds the descriptor interface of each effective shader permutation, and replaces the
/// zero-as-sentinel topology check with an explicit presence flag. A version 3 package is refused
/// rather than reinterpreted, because its readiness flags could not distinguish "a binding exists"
/// from "the effective module's required resources are all available".
///
/// Schema 4 carries the session identity that version 3 introduced, with the same meaning. The
/// merge of the two branches is a reconciliation, not a choice between them: RHI-02I-R3's own
/// handoff requires schema 4 to retain the field.
inline constexpr u32 SchemaVersion = 4;

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

    // The original-shader dimensions. These are separate because a package can carry the shader
    // binaries and still be unable to run them: the bindings may be incomplete, the push constants
    // may be missing, or the pipeline state may not have been captured. Collapsing them into one
    // flag is what lets a package claim readiness it does not have.
    /// Both stages' effective SPIR-V are present and hash-verified.
    bool shader_binaries_ready{};
    /// Every descriptor the SPIR-V declares has a captured binding.
    bool shader_bindings_ready{};
    /// The push constants and user data the shaders consume were captured.
    bool shader_parameters_ready{};
    /// The graphics pipeline state the draw used was captured.
    bool pipeline_state_ready_original{};
    /// A pre-draw and post-draw reference of the colour attachment was captured.
    bool reference_output_ready{};
    /// Everything above, so the draw can run on its original shaders and be compared.
    bool original_shader_replay_ready{};

    /// Why the highest claimed level is not higher.
    std::string limitation;
};

/// One effective shader stage of the captured draw.
///
/// The identity is (program hash, permutation index), not the program hash alone. One GCN program
/// compiles to several permutations whose specialization depends on runtime state, resource shapes,
/// fetch-shader state and binding allocation, so a hash match alone does not identify the module the
/// draw executed.
struct ShaderStage {
    /// The logical stage: 0 fragment, 3 vertex.
    u32 logical_stage{};
    /// The hardware stage the program was compiled for.
    u32 hardware_stage{};
    /// The original GCN program hash.
    u64 program_hash{};
    /// The permutation index within that program.
    u32 permutation_index{};
    /// HashCombine(program_hash, permutation_index). This is what the pipeline key carries.
    u64 permutation_hash{};
    /// Whether a shader patch was applied. When true, the effective module is the patch, not the
    /// generated SPIR-V.
    bool patched{};
    /// SHA-256 of the effective module's bytes.
    std::string effective_sha256;
    /// SHA-256 of the generated SPIR-V, before any patch.
    std::string generated_sha256;
    /// Entry point the module exports.
    std::string entry_point;
    /// Byte length of the effective module, so a reader can size its buffer before loading.
    u64 effective_byte_length{};
    /// Byte length of the GCN instruction words.
    u64 gcn_byte_length{};
    /// The sidecar file holding the effective module.
    std::string effective_file;
    /// The sidecar file holding the GCN words, when they were retained.
    std::string gcn_file;
    /// The effective module's bytes. Written to the sidecar named by effective_file; not serialized
    /// into the JSON, so a reader loads it from the file and verifies it against effective_sha256.
    std::vector<u8> effective_bytes;
    /// The GCN instruction words, when they were retained.
    std::vector<u8> gcn_bytes;
};

/// One descriptor the effective module declares, as the production layout allocated it.
struct ShaderDescriptor {
    u32 set{};
    u32 binding{};
    /// The descriptor class, in the vocabulary the SPIR-V uses.
    u32 descriptor_class{};
    u32 count{1};
    /// The shader stages that can see this binding, as a Vulkan stage-flags mask.
    u32 stage_flags{};
    /// The guest shader-resource index this came from, or 0xFFFFFFFF when it did not.
    u32 guest_resource_index{0xFFFFFFFFu};
    bool written{};
};

/// The descriptor interface of one effective shader permutation.
///
/// The identity is (program hash, permutation index), matching the binary capture. One program can
/// produce permutations with different resource interfaces, so a lookup by program hash alone is
/// insufficient.
struct ShaderInterface {
    u64 program_hash{};
    u32 permutation_index{};
    u64 permutation_hash{};
    u32 hardware_stage{};
    u32 logical_stage{};
    bool patched{};
    std::vector<ShaderDescriptor> descriptors;
    /// The push-constant block this stage declares.
    u32 push_constant_offset{};
    u32 push_constant_size{};
    u32 push_constant_stage_flags{};
    /// The number of unified bindings the stage consumed.
    u32 unified_binding_count{};
};

/// The graphics pipeline state the draw used, as the renderer actually configured it.
struct PipelineState {
    /// Whether the topology was captured. Zero is a legitimate Vulkan topology value, so it cannot
    /// double as a missing-data sentinel.
    bool has_topology{};
    /// Vulkan primitive topology.
    u32 topology{};
    /// Cull mode and front-face convention.
    u32 cull_mode{};
    u32 front_face{};
    /// Polygon mode.
    u32 polygon_mode{};
    /// Depth test and write enables, and the compare operation.
    bool depth_test{};
    bool depth_write{};
    u32 depth_compare_op{};
    /// Colour attachment formats, in attachment order.
    std::vector<u32> color_formats;
    u32 depth_format{};
    /// Sample count.
    u32 samples{};
    /// Blend state per attachment: whether blending is enabled and the write mask.
    std::vector<u32> blend_enable;
    std::vector<u32> write_mask;
    /// The viewport and scissor the draw used.
    float viewport_x{};
    float viewport_y{};
    float viewport_width{};
    float viewport_height{};
    float viewport_min_depth{};
    float viewport_max_depth{};
    /// The push-constant range the pipeline layout declares.
    u32 push_constant_offset{};
    u32 push_constant_size{};
    /// The push-constant bytes the draw submitted.
    std::vector<u8> push_constant_bytes;
};

/// A reference image of the colour attachment around the draw.
struct ReferenceOutput {
    /// Width and height of the captured subresource.
    u32 width{};
    u32 height{};
    /// Vulkan format of the attachment.
    u32 format{};
    /// Bytes per pixel, so a reader can size its buffer.
    u32 bytes_per_pixel{};
    /// SHA-256 of the pre-draw and post-draw contents.
    std::string pre_sha256;
    std::string post_sha256;
    /// Sidecar files holding the contents.
    std::string pre_file;
    std::string post_file;
    /// Whether the pre-draw contents were captured. A draw that does not depend on prior contents
    /// does not need them, and saying so is more useful than an empty image.
    bool has_pre{};
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
    /// The capture session this package was exported in. Read from the one CaptureSession identity.
    u64 session_id{};
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

    /// The effective shader stages of this draw, one per stage the pipeline used.
    std::vector<ShaderStage> shader_stages;
    /// The descriptor interface of each effective stage, keyed by permutation hash.
    std::vector<ShaderInterface> shader_interfaces;
    /// The graphics pipeline state the draw used.
    PipelineState pipeline_state;
    /// The reference output around the draw.
    ReferenceOutput reference_output;
    /// The reference image bytes. Written to the sidecars named by reference_output; not serialized
    /// into the JSON, so a reader loads them from the files and verifies them against the hashes.
    std::vector<u8> reference_pre_bytes;
    std::vector<u8> reference_post_bytes;

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

        // The original-shader dimensions. Each is derived from what the package actually carries,
        // so a package cannot claim to run on its original shaders without the binaries, the
        // bindings, the parameters and the pipeline state all being present.
        if (package.readiness.shader_binaries_ready) {
            if (package.shader_stages.empty()) {
                error = "shader binary readiness claimed with no shader stages";
                return false;
            }
            // A draw needs both a vertex and a fragment stage. A package carrying one cannot run.
            bool has_vertex = false;
            bool has_fragment = false;
            for (const auto& stage : package.shader_stages) {
                if (stage.effective_sha256.empty()) {
                    error = "shader stage has no effective module hash";
                    return false;
                }
                if (stage.effective_byte_length == 0) {
                    error = "shader stage has no effective module bytes";
                    return false;
                }
                if (stage.permutation_hash == 0) {
                    error = "shader stage has no permutation hash";
                    return false;
                }
                // The permutation hash must be the combination of the program hash and the
                // permutation index, or the identity is internally inconsistent.
                if (stage.permutation_hash !=
                    HashCombine(stage.program_hash,
                                static_cast<u64>(stage.permutation_index))) {
                    error = "shader stage permutation hash does not match its program hash and "
                            "permutation index";
                    return false;
                }
                // The retained bytes must be the ones the hash describes. A stage whose bytes and
                // hash disagree is not the module the draw executed.
                if (!stage.effective_bytes.empty()) {
                    if (stage.effective_bytes.size() != stage.effective_byte_length) {
                        error = "shader stage byte length does not match its retained bytes";
                        return false;
                    }
                    sha256::SHA256 hasher;
                    hasher.update(stage.effective_bytes.data(), stage.effective_bytes.size());
                    const auto digest = hasher.final();
                    static constexpr char kHex[] = "0123456789abcdef";
                    std::string hex;
                    hex.reserve(64);
                    for (const u8 byte : digest) {
                        hex.push_back(kHex[byte >> 4]);
                        hex.push_back(kHex[byte & 0x0F]);
                    }
                    if (hex != stage.effective_sha256) {
                        error = "shader stage content hash does not match its bytes";
                        return false;
                    }
                }
                if (stage.logical_stage == 3) {
                    has_vertex = true;
                } else if (stage.logical_stage == 0) {
                    has_fragment = true;
                }
            }
            if (!has_vertex || !has_fragment) {
                error = "shader binary readiness claimed without both a vertex and a fragment "
                        "stage";
                return false;
            }
        }

        if (package.readiness.shader_bindings_ready) {
            if (!package.readiness.shader_binaries_ready) {
                error = "shader binding readiness claimed without shader binaries";
                return false;
            }
            // A binding existing is not the same as the effective modules' required resources being
            // available. Every stage must carry an interface, and every descriptor that interface
            // declares must resolve to a captured binding.
            if (package.shader_interfaces.empty()) {
                error = "shader binding readiness claimed with no descriptor interface";
                return false;
            }
            for (const auto& stage : package.shader_stages) {
                bool found = false;
                for (const auto& interface : package.shader_interfaces) {
                    if (interface.permutation_hash == stage.permutation_hash) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error = "a shader stage has no descriptor interface for its permutation";
                    return false;
                }
            }
            for (const auto& interface : package.shader_interfaces) {
                for (const auto& descriptor : interface.descriptors) {
                    if (descriptor.count == 0) {
                        error = "a descriptor interface declares a zero descriptor count";
                        return false;
                    }
                    // A buffer or image descriptor must resolve to a captured binding. A sampler
                    // may legitimately have no buffer, so it is not required to.
                    if (descriptor.descriptor_class == 1 || descriptor.descriptor_class == 2 ||
                        descriptor.descriptor_class == 3 || descriptor.descriptor_class == 4 ||
                        descriptor.descriptor_class == 6) {
                        bool resolved = false;
                        for (const auto& binding : package.bindings) {
                            if (binding.vk_binding == descriptor.binding) {
                                resolved = true;
                                break;
                            }
                        }
                        if (!resolved) {
                            error = "a descriptor interface declares a binding with no captured "
                                    "resource";
                            return false;
                        }
                    }
                }
            }
        }

        if (package.readiness.shader_parameters_ready) {
            if (!package.readiness.shader_bindings_ready) {
                error = "shader parameter readiness claimed without binding readiness";
                return false;
            }
            // A push-constant claim needs the bytes, not merely a declared range.
            if (package.pipeline_state.push_constant_size != 0 &&
                package.pipeline_state.push_constant_bytes.size() !=
                    package.pipeline_state.push_constant_size) {
                error = "shader parameter readiness claimed with a push-constant range that does "
                        "not match its bytes";
                return false;
            }
        }

        if (package.readiness.pipeline_state_ready_original) {
            // Presence is explicit. Zero is a legitimate Vulkan topology value, so it cannot be
            // used as a missing-data sentinel.
            if (!package.pipeline_state.has_topology) {
                error = "pipeline state readiness claimed with no topology";
                return false;
            }
            if (package.pipeline_state.color_formats.empty()) {
                error = "pipeline state readiness claimed with no colour attachment format";
                return false;
            }
        }

        if (package.readiness.reference_output_ready) {
            if (package.reference_output.post_sha256.empty()) {
                error = "reference output readiness claimed with no post-draw reference";
                return false;
            }
            if (package.reference_output.width == 0 || package.reference_output.height == 0) {
                error = "reference output readiness claimed with no extent";
                return false;
            }
        }

        // The strongest claim requires every dimension below it. This is the check that stops a
        // package from asserting it can run the original shaders when one input is missing.
        if (package.readiness.original_shader_replay_ready) {
            if (!package.readiness.shader_binaries_ready ||
                !package.readiness.shader_bindings_ready ||
                !package.readiness.shader_parameters_ready ||
                !package.readiness.pipeline_state_ready_original ||
                !package.readiness.reference_output_ready) {
                error = "original shader replay readiness claimed without every dimension";
                return false;
            }
            if (!package.readiness.geometry_ready) {
                error = "original shader replay readiness claimed without geometry";
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
