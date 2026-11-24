#include "replay/package.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace replay {

namespace {

using nlohmann::json;

bool ReadFile(const std::string& path, std::vector<unsigned char>& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(out.data()), size);
    return true;
}

bool ReadText(const std::string& path, std::string& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// A missing required field is a hard error, never a silent zero.
template <typename T>
bool Require(const json& j, const char* key, T& out, std::string& error) {
    auto it = j.find(key);
    if (it == j.end()) {
        error = std::string("missing required field '") + key + "'";
        return false;
    }
    out = it->get<T>();
    return true;
}

}  // namespace

const Resource* Package::GetResource(uint32_t id) const {
    for (const auto& r : resources) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

const Sampler* Package::FindSampler(uint32_t id) const {
    for (const auto& s : samplers) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const Texture* Package::FindTexture(uint32_t id) const {
    for (const auto& t : textures) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const char* FormatName(uint32_t vk_format) {
    switch (vk_format) {
        case 106: return "R32G32B32_SFLOAT";
        case 37: return "R8G8B8A8_UNORM";
        case 43: return "R8G8B8A8_SRGB";
        case 97: return "R16G16B16A16_SFLOAT";
        case 137: return "BC3_UNORM_BLOCK";
        case 138: return "BC3_SRGB_BLOCK";
        case 148: return "BC7_SRGB_BLOCK";
        default: return "unknown";
    }
}

bool LoadPackage(const std::string& dir, Package& out, std::string& error) {
    std::string text;
    if (!ReadText(dir + "/package.json", text, error)) return false;

    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        error = std::string("package.json parse error: ") + e.what();
        return false;
    }

    if (!Require(j, "schema_version", out.schema_version, error)) return false;
    if (out.schema_version != 2) {
        error = "unsupported schema_version " + std::to_string(out.schema_version);
        return false;
    }
    if (!Require(j, "scene", out.scene, error)) return false;
    if (!Require(j, "provenance_id", out.provenance_id, error)) return false;

    if (j.contains("pipeline")) {
        const auto& p = j["pipeline"];
        out.pipeline.vs_hash = p.value("vs_hash", 0ull);
        out.pipeline.ps_hash = p.value("ps_hash", 0ull);
        out.pipeline.primitive_type = p.value("primitive_type", 0u);
        out.pipeline.is_indexed = p.value("is_indexed", false);
        out.pipeline.num_indices = p.value("num_indices", 0u);
        out.pipeline.num_instances = p.value("num_instances", 1u);
    }

    // Resources carry their bytes inline as a base64-free byte array, or the
    // package stores them in resources.bin. The existing schema uses the latter.
    std::vector<unsigned char> resource_blob;
    if (!ReadFile(dir + "/resources.bin", resource_blob, error)) return false;

    if (!j.contains("resources")) {
        error = "missing 'resources'";
        return false;
    }
    for (const auto& r : j["resources"]) {
        Resource res;
        res.id = r.value("id", 0u);
        const uint64_t offset = r.value("blob_offset", 0ull);
        const uint64_t length = r.value("byte_length", 0ull);
        if (offset + length > resource_blob.size()) {
            error = "resource " + std::to_string(res.id) + " exceeds resources.bin";
            return false;
        }
        res.bytes.assign(resource_blob.begin() + static_cast<ptrdiff_t>(offset),
                         resource_blob.begin() + static_cast<ptrdiff_t>(offset + length));
        out.resources.push_back(std::move(res));
    }

    if (j.contains("vertex_bindings")) {
        for (const auto& v : j["vertex_bindings"]) {
            VertexBinding vb;
            vb.binding = v.value("binding", 0u);
            vb.stride = v.value("stride", 0u);
            vb.element_count = v.value("element_count", 0u);
            vb.vk_format = v.value("vk_format", 0u);
            vb.attr_offset = v.value("attr_offset", 0u);
            vb.resource_ref = v.value("resource_ref", 0u);
            vb.input_rate = v.value("input_rate", 0u);
            vb.divisor = v.value("divisor", 0u);
            out.vertex_bindings.push_back(vb);
        }
    }

    if (j.contains("index_binding")) {
        const auto& ib = j["index_binding"];
        out.index_binding.index_size = ib.value("index_size", 2u);
        out.index_binding.index_count = ib.value("index_count", 0u);
        out.index_binding.first_index = ib.value("first_index", 0u);
        out.index_binding.base_vertex = ib.value("base_vertex", 0);
        out.index_binding.instance_count = ib.value("instance_count", 1u);
        out.index_binding.first_instance = ib.value("first_instance", 0u);
        out.index_binding.resource_ref = ib.value("resource_ref", 0u);
    }

    if (j.contains("geometry")) {
        const auto& g = j["geometry"];
        out.geometry.position_binding = g.value("position_binding", 0u);
        out.geometry.position_components = g.value("position_components", 3u);
        out.geometry.triangle_count = g.value("triangle_count", 0u);
        out.geometry.is_three_dimensional = g.value("is_three_dimensional", false);
        if (g.contains("bounds_min")) {
            for (int i = 0; i < 3; ++i) out.geometry.bounds_min[i] = g["bounds_min"][i];
        }
        if (g.contains("bounds_max")) {
            for (int i = 0; i < 3; ++i) out.geometry.bounds_max[i] = g["bounds_max"][i];
        }
    }

    if (j.contains("texture_coordinate")) {
        const auto& t = j["texture_coordinate"];
        out.texture_coordinate.binding = t.value("binding", 0u);
        out.texture_coordinate.component_u = t.value("component_u", 0u);
        out.texture_coordinate.component_v = t.value("component_v", 1u);
        out.texture_coordinate.looks_like_uv = t.value("looks_like_uv", false);
    }

    if (j.contains("textures")) {
        for (const auto& t : j["textures"]) {
            Texture tex;
            tex.id = t.value("id", 0u);
            tex.vk_binding = t.value("vk_binding", 0u);
            tex.image_format = t.value("image_format", 0u);
            tex.width = t.value("width", 0u);
            tex.height = t.value("height", 0u);
            tex.mip_count = t.value("mip_count", 1u);
            tex.layer_count = t.value("layer_count", 1u);
            tex.block_columns = t.value("block_columns", 0u);
            tex.block_rows = t.value("block_rows", 0u);
            tex.block_texels_x = t.value("block_texels_x", 4u);
            tex.block_texels_y = t.value("block_texels_y", 4u);
            tex.bytes_per_block = t.value("bytes_per_block", 16u);
            tex.sampler_id = t.value("sampler_id", 0u);
            tex.sampler_verified = t.value("sampler_verified", false);
            if (t.contains("subresources")) {
                for (const auto& s : t["subresources"]) {
                    Subresource sr;
                    sr.mip = s.value("mip", 0u);
                    sr.layer = s.value("layer", 0u);
                    sr.width = s.value("width", 0u);
                    sr.height = s.value("height", 0u);
                    sr.row_pitch = s.value("row_pitch", 0u);
                    sr.blob_offset = s.value("blob_offset", 0ull);
                    sr.byte_length = s.value("byte_length", 0ull);
                    tex.subresources.push_back(sr);
                }
            }
            out.textures.push_back(tex);
        }
    }

    if (j.contains("samplers")) {
        for (const auto& s : j["samplers"]) {
            Sampler sm;
            sm.id = s.value("id", 0u);
            sm.mag_filter = s.value("mag_filter", 0u);
            sm.min_filter = s.value("min_filter", 0u);
            sm.mip_filter = s.value("mip_filter", 0u);
            sm.address_u = s.value("address_u", 0u);
            sm.address_v = s.value("address_v", 0u);
            sm.address_w = s.value("address_w", 0u);
            sm.min_lod = s.value("min_lod", 0.0f);
            sm.max_lod = s.value("max_lod", 0.0f);
            sm.anisotropy_enable = s.value("anisotropy_enable", false);
            sm.max_anisotropy = s.value("max_anisotropy", 1u);
            sm.compare_enable = s.value("compare_enable", false);
            out.samplers.push_back(sm);
        }
    }

    // Texture bytes live in textures.bin when the package has any.
    if (!out.textures.empty()) {
        if (!ReadFile(dir + "/textures.bin", out.texture_blob, error)) return false;
    }
    return true;
}

}  // namespace replay
