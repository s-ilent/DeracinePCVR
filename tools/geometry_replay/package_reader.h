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

/// One shader-buffer binding the production path actually wrote.
///
/// This is the correspondence between a guest shader resource and the Vulkan descriptor the
/// renderer bound. It is captured, not derived: `vk_binding` is the unified binding number the
/// descriptor write used, and `resource_ref` names the package resource whose bytes were bound.
///
/// `resource_index` is the guest shader-resource index and `sharp_idx` is the flattened descriptor
/// index. Neither equals `vk_binding`, and treating them as equal would bind the wrong resource.
struct ShaderBinding {
    /// The shader stage, in the producer's vocabulary.
    u32 stage{};
    /// The guest shader-resource index.
    u32 resource_index{};
    /// The flattened descriptor index.
    u32 sharp_idx{};
    /// The unified Vulkan binding number the descriptor write used.
    u32 vk_binding{};
    /// The offset and range the descriptor was written with.
    u64 vk_offset{};
    u64 vk_range{};
    /// The producer's buffer identity.
    u64 buffer_id{};
    /// The package resource whose bytes were bound.
    u32 resource_ref{};
    /// The capture status, as the producer named it.
    std::string status;
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
    u32 push_constant_offset{};
    u32 push_constant_size{};
    u32 push_constant_stage_flags{};
    u32 unified_binding_count{};
};

/// One effective shader stage: the module identity and the bytes it resolved to.
struct ShaderStage {
    u64 program_hash{};
    u32 permutation_index{};
    u64 permutation_hash{};
    u32 hardware_stage{};
    u32 logical_stage{};
    bool patched{};
    std::string generated_sha256;
    std::string effective_sha256;
    std::string entry_point;
    u64 generated_byte_length{};
    u64 effective_byte_length{};
    std::string generated_file;
    std::string effective_file;
    /// The effective module's bytes, loaded from its sidecar.
    std::vector<u8> effective_bytes;
};

/// Which pipeline-state fields the package actually captured.
///
/// Presence is explicit because zero is a legitimate value for every one of these enums:
/// `VK_POLYGON_MODE_FILL` is 0, `VK_COMPARE_OP_NEVER` is 0, `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` is 0
/// and `VK_CULL_MODE_NONE` is 0. Testing a value against zero to decide whether it was captured
/// would substitute the diagnostic default for exactly the fields the game set to their zero value.
struct PipelineStatePresent {
    bool topology : 1;
    bool cull_mode : 1;
    bool front_face : 1;
    bool polygon_mode : 1;
    bool depth_test : 1;
    bool depth_write : 1;
    bool depth_compare_op : 1;
    bool samples : 1;
    bool color_formats : 1;
    bool depth_format : 1;
    bool write_mask : 1;
    bool blend_enable : 1;
    bool viewport : 1;
};

/// The graphics pipeline state the draw used, as the renderer actually configured it.
struct PipelineState {
    /// Which fields were captured. A field not marked present keeps the diagnostic default and is
    /// reported as substituted.
    PipelineStatePresent present{};
    /// Whether the topology was captured. Kept for compatibility with the producer's own field.
    bool has_topology{};
    u32 topology{};
    u32 cull_mode{};
    u32 front_face{};
    u32 polygon_mode{};
    bool depth_test{};
    bool depth_write{};
    u32 depth_compare_op{};
    std::vector<u32> color_formats;
    u32 depth_format{};
    u32 samples{};
    std::vector<u32> blend_enable;
    std::vector<u32> write_mask;
    float viewport_x{};
    float viewport_y{};
    float viewport_width{};
    float viewport_height{};
    float viewport_min_depth{};
    float viewport_max_depth{};
    u32 push_constant_offset{};
    u32 push_constant_size{};
    std::vector<u8> push_constant_bytes;
};

/// A reference image of the colour attachment around the draw.
struct ReferenceOutput {
    u32 width{};
    u32 height{};
    u32 format{};
    u32 bytes_per_pixel{};
    std::string pre_sha256;
    std::string post_sha256;
    std::string pre_file;
    std::string post_file;
    bool has_pre{};
    /// The post-draw contents, loaded from its sidecar.
    std::vector<u8> post_bytes;
    /// The pre-draw contents, when captured.
    std::vector<u8> pre_bytes;
};

struct Readiness {
    bool geometry_ready{};
    bool texture_binding_ready{};
    bool texture_contents_ready{};
    bool sampler_ready{};
    bool diagnostic_textured_replay_ready{};
    // Schema 4 adds the original-shader dimensions. Each is a claim to verify, not permission to
    // skip verification.
    bool shader_binaries_ready{};
    bool shader_bindings_ready{};
    bool shader_parameters_ready{};
    bool pipeline_state_ready_original{};
    bool reference_output_ready{};
    bool original_shader_replay_ready{};
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
    // Schema 4.
    std::vector<ShaderStage> shader_stages;
    std::vector<ShaderInterface> shader_interfaces;
    /// The shader-buffer bindings the production path wrote, which is what connects a descriptor
    /// to the resource whose bytes were bound.
    std::vector<ShaderBinding> shader_bindings;
    PipelineState pipeline_state;
    ReferenceOutput reference;
    /// The execution identity within the session, when the producer recorded one. The session id
    /// itself is declared above, because RHI-02I-R2 introduced it before schema 4 existed.
    u64 execution_id{};
};

/// The schema versions this reader accepts.
///
/// Schema 2 is the resource-only package. Schema 3 adds the capture session identity and the
/// effective shader binaries, and is what RHI-02I-R2 and RHI-02I-R3 write. Schema 4 adds the
/// descriptor interface of each effective permutation, the explicit pipeline-state presence flags
/// and the reference images.
///
/// Schema 3 is accepted as a compatibility path, not as an equivalent: it carries no descriptor
/// interface, so effective-shader mode refuses it. A version outside this set is refused rather
/// than reinterpreted, because a missing field would otherwise read as a default and a default is
/// not a captured value.
inline constexpr u32 SchemaResourceOnly = 2;
inline constexpr u32 SchemaSessionIdentity = 3;
inline constexpr u32 SchemaEffectiveShader = 4;

/// Reads a package directory. Returns false with a reason when a required field is absent, a
/// sidecar is missing or the wrong length, or the JSON is malformed, rather than producing a
/// partially populated package.
///
/// param require_effective  When true, a schema-2 package is refused. Effective-shader mode must
///                          not silently fall back to a resource-only package.
[[nodiscard]] bool Read(const std::string& directory, Package& out, std::string& error,
                        bool require_effective = false);

/// The resource with the given id, or nullptr.
[[nodiscard]] const Resource* FindResource(const Package& package, u32 id);

/// The texture with captured content, or nullptr when none has any.
[[nodiscard]] const Texture* FindTextured(const Package& package);

/// The sampler a texture pairs with, or nullptr when the association is unresolved.
[[nodiscard]] const Sampler* FindSampler(const Package& package, const Texture& texture);

} // namespace package
