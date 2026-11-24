// A native NRI replay of a captured Deracine draw package.
//
// Consumes the existing SC-09H / SC-10S package schema unchanged. The package is
// the authority for geometry, indices, texture bytes and sampler state; this
// program only interprets it and drives the semantic RHI.
//
// The diagnostic substitutions are explicit and reported:
//   * the camera and MVP are diagnostic, not the game's;
//   * the shaders are diagnostic, not the game's;
//   * alpha is forced opaque in the textured path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace replay {

// A captured resource blob.
struct Resource {
    uint32_t id = 0;
    std::vector<unsigned char> bytes;
};

struct VertexBinding {
    uint32_t binding = 0;
    uint32_t stride = 0;
    uint32_t element_count = 0;
    uint32_t vk_format = 0;
    uint32_t attr_offset = 0;
    uint32_t resource_ref = 0;
    uint32_t input_rate = 0;
    uint32_t divisor = 0;
};

struct IndexBinding {
    uint32_t index_size = 2;
    uint32_t index_count = 0;
    uint32_t first_index = 0;
    int32_t base_vertex = 0;
    uint32_t instance_count = 1;
    uint32_t first_instance = 0;
    uint32_t resource_ref = 0;
};

struct Geometry {
    uint32_t position_binding = 0;
    uint32_t position_components = 3;
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    uint32_t triangle_count = 0;
    bool is_three_dimensional = false;
};

struct TextureCoord {
    uint32_t binding = 0;
    uint32_t component_u = 0;
    uint32_t component_v = 1;
    bool looks_like_uv = false;
};

struct Subresource {
    uint32_t mip = 0;
    uint32_t layer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_pitch = 0;
    uint64_t blob_offset = 0;
    uint64_t byte_length = 0;
};

struct Texture {
    uint32_t id = 0;
    uint32_t vk_binding = 0;
    uint32_t image_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_count = 1;
    uint32_t layer_count = 1;
    uint32_t block_columns = 0;
    uint32_t block_rows = 0;
    uint32_t block_texels_x = 4;
    uint32_t block_texels_y = 4;
    uint32_t bytes_per_block = 16;
    uint32_t sampler_id = 0;
    bool sampler_verified = false;
    std::vector<Subresource> subresources;
};

struct Sampler {
    uint32_t id = 0;
    uint32_t mag_filter = 0;
    uint32_t min_filter = 0;
    uint32_t mip_filter = 0;
    uint32_t address_u = 0;
    uint32_t address_v = 0;
    uint32_t address_w = 0;
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    bool anisotropy_enable = false;
    uint32_t max_anisotropy = 1;
    bool compare_enable = false;
};

struct Pipeline {
    uint64_t vs_hash = 0;
    uint64_t ps_hash = 0;
    uint32_t primitive_type = 0;
    bool is_indexed = false;
    uint32_t num_indices = 0;
    uint32_t num_instances = 1;
};

// A parsed draw package. Schema 2, as produced by the existing capture harness.
struct Package {
    uint32_t schema_version = 0;
    std::string scene;
    uint64_t provenance_id = 0;
    Pipeline pipeline;
    std::vector<Resource> resources;
    std::vector<VertexBinding> vertex_bindings;
    IndexBinding index_binding;
    Geometry geometry;
    TextureCoord texture_coordinate;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    // Raw texture blob bytes, loaded from textures.bin.
    std::vector<unsigned char> texture_blob;

    const Resource* GetResource(uint32_t id) const;
    const Sampler* FindSampler(uint32_t id) const;
    const Texture* FindTexture(uint32_t id) const;
};

// Loads a package directory. Returns false and fills 'error' on any failure.
bool LoadPackage(const std::string& dir, Package& out, std::string& error);

// Maps a captured Vulkan format enum to a semantic format name, for reporting.
const char* FormatName(uint32_t vk_format);

}  // namespace replay
