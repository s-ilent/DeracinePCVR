// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone diagnostic textured replay for a Deracine draw package.
//
// Independent of shadPS4: it links only the Vulkan loader and the C++ standard library. It reads a
// package directory, validates it, and renders the captured geometry with the captured texture and
// sampler state.
//
// What it proves: the effective image, its contents, the effective sampler state, the vertex
// layout and the draw arguments are sufficient to reproduce a textured draw outside the PS4
// command stream.
//
// What it does not prove: material correctness. There is no original shader, no lighting, and no
// fixed-function state beyond what the diagnostic pipeline needs.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <nlohmann/json.hpp>

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWidth = 640;
constexpr u32 kHeight = 640;

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
    u32 resource_ref{};
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
    u64 sampler_id{};
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

struct Package {
    u32 schema_version{};
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
    bool geometry_ready{};
    bool textured_ready{};
    std::vector<u8> texture_blob;
};

u64 Fnv1a(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

bool ReadPackage(const std::string& directory, Package& out, std::string& error) {
    std::ifstream json_in{std::filesystem::path{directory} / "package.json"};
    if (!json_in) {
        error = "cannot open package.json";
        return false;
    }
    nlohmann::json doc;
    try {
        json_in >> doc;
    } catch (const std::exception& e) {
        error = std::string{"package.json is not valid JSON: "} + e.what();
        return false;
    }

    const auto read_blob = [&directory](const char* name) {
        std::ifstream in{std::filesystem::path{directory} / name, std::ios::binary};
        std::vector<u8> bytes;
        if (in) {
            bytes.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
        }
        return bytes;
    };
    const auto blob = read_blob("resources.bin");
    const auto texture_blob = read_blob("textures.bin");

    try {
        out.schema_version = doc.at("schema_version").get<u32>();
        out.provenance_id = doc.at("provenance_id").get<u64>();
        out.is_indexed = doc.at("pipeline").value("is_indexed", false);

        for (const auto& entry : doc.at("resources")) {
            Resource resource{};
            resource.id = entry.at("id").get<u32>();
            resource.clamped_size = entry.value("clamped_size", 0ull);
            resource.content_hash = entry.value("content_hash", 0ull);
            resource.blob_offset = entry.value("blob_offset", 0ull);
            resource.byte_length = entry.value("byte_length", 0ull);
            if (resource.byte_length != 0 &&
                resource.blob_offset + resource.byte_length <= blob.size()) {
                resource.bytes.assign(
                    blob.begin() + static_cast<std::ptrdiff_t>(resource.blob_offset),
                    blob.begin() +
                        static_cast<std::ptrdiff_t>(resource.blob_offset + resource.byte_length));
            }
            out.resources.push_back(std::move(resource));
        }

        for (const auto& entry : doc.at("vertex_bindings")) {
            VertexBinding vertex{};
            vertex.binding = entry.at("binding").get<u32>();
            vertex.stride = entry.value("stride", 0u);
            vertex.resource_ref = entry.value("resource_ref", 0u);
            vertex.semantic = entry.value("semantic", 0u);
            vertex.vk_format = entry.value("vk_format", 0u);
            vertex.attr_offset = entry.value("attr_offset", 0u);
            vertex.element_count = entry.value("element_count", 0u);
            out.vertex_bindings.push_back(vertex);
        }

        if (doc.contains("index_binding")) {
            const auto& entry = doc.at("index_binding");
            out.has_index = true;
            out.index.index_size = entry.value("index_size", 0u);
            out.index.index_count = entry.value("index_count", 0u);
            out.index.resource_ref = entry.value("resource_ref", 0u);
        }

        if (doc.contains("geometry")) {
            const auto& entry = doc.at("geometry");
            out.geometry.position_binding = entry.value("position_binding", 0u);
            out.geometry.position_components = entry.value("position_components", 0u);
            out.geometry.is_three_dimensional = entry.value("is_three_dimensional", false);
            if (entry.contains("bounds_min") && entry["bounds_min"].size() == 3) {
                for (u32 i = 0; i < 3; ++i)
                    out.geometry.bounds_min[i] = entry["bounds_min"][i].get<float>();
            }
            if (entry.contains("bounds_max") && entry["bounds_max"].size() == 3) {
                for (u32 i = 0; i < 3; ++i)
                    out.geometry.bounds_max[i] = entry["bounds_max"][i].get<float>();
            }
        }

        for (const auto& entry : doc.at("textures")) {
            Texture texture{};
            texture.id = entry.at("id").get<u32>();
            texture.vk_binding = entry.value("vk_binding", 0u);
            texture.image_format = entry.value("image_format", 0u);
            texture.view_format = entry.value("view_format", 0u);
            texture.width = entry.value("width", 0u);
            texture.height = entry.value("height", 0u);
            texture.sampler_id = entry.value("sampler_id", 0ull);
            for (const auto& sub : entry.value("subresources", nlohmann::json::array())) {
                TextureSubresource record{};
                record.mip = sub.value("mip", 0u);
                record.layer = sub.value("layer", 0u);
                record.width = sub.value("width", 0u);
                record.height = sub.value("height", 0u);
                record.row_pitch = sub.value("row_pitch", 0u);
                record.blob_offset = sub.value("blob_offset", 0ull);
                record.byte_length = sub.value("byte_length", 0ull);
                record.content_hash = sub.value("content_hash", 0ull);
                texture.subresources.push_back(record);
            }
            out.textures.push_back(std::move(texture));
        }

        for (const auto& entry : doc.at("samplers")) {
            Sampler sampler{};
            sampler.id = entry.at("id").get<u64>();
            sampler.mag_filter = entry.value("mag_filter", 0u);
            sampler.min_filter = entry.value("min_filter", 0u);
            sampler.mip_filter = entry.value("mip_filter", 0u);
            sampler.address_u = entry.value("address_u", 0u);
            sampler.address_v = entry.value("address_v", 0u);
            sampler.address_w = entry.value("address_w", 0u);
            sampler.lod_bias = entry.value("lod_bias", 0.0f);
            sampler.min_lod = entry.value("min_lod", 0.0f);
            sampler.max_lod = entry.value("max_lod", 0.0f);
            sampler.anisotropy_enable = entry.value("anisotropy_enable", false);
            sampler.max_anisotropy = entry.value("max_anisotropy", 1.0f);
            sampler.compare_enable = entry.value("compare_enable", false);
            sampler.compare_op = entry.value("compare_op", 0u);
            sampler.border_color = entry.value("border_color", 0u);
            out.samplers.push_back(sampler);
        }

        if (doc.contains("texture_coordinate")) {
            const auto& entry = doc.at("texture_coordinate");
            out.uv.binding = entry.value("binding", 0u);
            out.uv.component_u = entry.value("component_u", 0u);
            out.uv.component_v = entry.value("component_v", 1u);
            out.uv.looks_like_uv = entry.value("looks_like_uv", false);
        }

        const auto& readiness = doc.at("readiness");
        out.geometry_ready = readiness.value("geometry_ready", false);
        out.textured_ready = readiness.value("diagnostic_textured_replay_ready", false);
    } catch (const std::exception& e) {
        error = std::string{"package.json is missing a required field: "} + e.what();
        return false;
    }

    // Keep the texture payload with the texture that owns it.
    for (auto& texture : out.textures) {
        for (auto& sub : texture.subresources) {
            if (sub.blob_offset + sub.byte_length <= texture_blob.size()) {
                sub.content_hash = Fnv1a(texture_blob.data() + sub.blob_offset,
                                         static_cast<size_t>(sub.byte_length));
            }
        }
    }
    out.texture_blob = std::move(texture_blob);
    error.clear();
    return true;
}

const u32 kVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000029, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0009000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x00000013,
    0x0000001f, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45,
    0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004, 0x475f4c47,
    0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x76755f76, 0x00000000, 0x00040005, 0x0000000b,
    0x755f6e69, 0x00000076, 0x00060005, 0x00000011, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x00000011, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x00000011,
    0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x00000011, 0x00000002,
    0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x00000011, 0x00000003, 0x435f6c67,
    0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x00000013, 0x00000000, 0x00040005, 0x00000017,
    0x68737550, 0x00000000, 0x00040006, 0x00000017, 0x00000000, 0x0070766d, 0x00030005, 0x00000019,
    0x00006370, 0x00040005, 0x0000001f, 0x705f6e69, 0x0000736f, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00040047, 0x0000000b, 0x0000001e, 0x00000001, 0x00030047, 0x00000011, 0x00000002,
    0x00050048, 0x00000011, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x00000011, 0x00000001,
    0x0000000b, 0x00000001, 0x00050048, 0x00000011, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x00000011, 0x00000003, 0x0000000b, 0x00000004, 0x00030047, 0x00000017, 0x00000002, 0x00040048,
    0x00000017, 0x00000000, 0x00000005, 0x00050048, 0x00000017, 0x00000000, 0x00000007, 0x00000010,
    0x00050048, 0x00000017, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000001f, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000003,
    0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040020, 0x0000000a, 0x00000001,
    0x00000007, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x00040017, 0x0000000d, 0x00000006,
    0x00000004, 0x00040015, 0x0000000e, 0x00000020, 0x00000000, 0x0004002b, 0x0000000e, 0x0000000f,
    0x00000001, 0x0004001c, 0x00000010, 0x00000006, 0x0000000f, 0x0006001e, 0x00000011, 0x0000000d,
    0x00000006, 0x00000010, 0x00000010, 0x00040020, 0x00000012, 0x00000003, 0x00000011, 0x0004003b,
    0x00000012, 0x00000013, 0x00000003, 0x00040015, 0x00000014, 0x00000020, 0x00000001, 0x0004002b,
    0x00000014, 0x00000015, 0x00000000, 0x00040018, 0x00000016, 0x0000000d, 0x00000004, 0x0003001e,
    0x00000017, 0x00000016, 0x00040020, 0x00000018, 0x00000009, 0x00000017, 0x0004003b, 0x00000018,
    0x00000019, 0x00000009, 0x00040020, 0x0000001a, 0x00000009, 0x00000016, 0x00040017, 0x0000001d,
    0x00000006, 0x00000003, 0x00040020, 0x0000001e, 0x00000001, 0x0000001d, 0x0004003b, 0x0000001e,
    0x0000001f, 0x00000001, 0x0004002b, 0x00000006, 0x00000021, 0x3f800000, 0x00040020, 0x00000027,
    0x00000003, 0x0000000d, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003d, 0x00000007, 0x0000000c, 0x0000000b, 0x0003003e, 0x00000009, 0x0000000c,
    0x00050041, 0x0000001a, 0x0000001b, 0x00000019, 0x00000015, 0x0004003d, 0x00000016, 0x0000001c,
    0x0000001b, 0x0004003d, 0x0000001d, 0x00000020, 0x0000001f, 0x00050051, 0x00000006, 0x00000022,
    0x00000020, 0x00000000, 0x00050051, 0x00000006, 0x00000023, 0x00000020, 0x00000001, 0x00050051,
    0x00000006, 0x00000024, 0x00000020, 0x00000002, 0x00070050, 0x0000000d, 0x00000025, 0x00000022,
    0x00000023, 0x00000024, 0x00000021, 0x00050091, 0x0000000d, 0x00000026, 0x0000001c, 0x00000025,
    0x00050041, 0x00000027, 0x00000028, 0x00000013, 0x00000015, 0x0003003e, 0x00000028, 0x00000026,
    0x000100fd, 0x00010038,
};

const u32 kFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000011, 0x00000015, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f,
    0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004,
    0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x65786574, 0x0000006c, 0x00040005,
    0x0000000d, 0x65745f75, 0x00000078, 0x00040005, 0x00000011, 0x76755f76, 0x00000000, 0x00050005,
    0x00000015, 0x5f74756f, 0x6f6c6f63, 0x00000072, 0x00040047, 0x0000000d, 0x00000021, 0x00000000,
    0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011, 0x0000001e, 0x00000000,
    0x00040047, 0x00000015, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00090019, 0x0000000a, 0x00000006, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000,
    0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f,
    0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000014, 0x00000003, 0x00000007,
    0x0004003b, 0x00000014, 0x00000015, 0x00000003, 0x00040017, 0x00000016, 0x00000006, 0x00000003,
    0x0004002b, 0x00000006, 0x00000019, 0x3f800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d,
    0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f, 0x00000012, 0x00000011, 0x00050057,
    0x00000007, 0x00000013, 0x0000000e, 0x00000012, 0x0003003e, 0x00000009, 0x00000013, 0x0004003d,
    0x00000007, 0x00000017, 0x00000009, 0x0008004f, 0x00000016, 0x00000018, 0x00000017, 0x00000017,
    0x00000000, 0x00000001, 0x00000002, 0x00050051, 0x00000006, 0x0000001a, 0x00000018, 0x00000000,
    0x00050051, 0x00000006, 0x0000001b, 0x00000018, 0x00000001, 0x00050051, 0x00000006, 0x0000001c,
    0x00000018, 0x00000002, 0x00070050, 0x00000007, 0x0000001d, 0x0000001a, 0x0000001b, 0x0000001c,
    0x00000019, 0x0003003e, 0x00000015, 0x0000001d, 0x000100fd, 0x00010038,
};

const u32 kFlatSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000011, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000010, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f,
    0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004,
    0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x5f74756f, 0x6f6c6f63, 0x00000072,
    0x00040005, 0x00000010, 0x76755f76, 0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040047, 0x00000010, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x0004002b, 0x00000006, 0x0000000a, 0x3f0ccccd, 0x0004002b, 0x00000006, 0x0000000b, 0x3f19999a,
    0x0004002b, 0x00000006, 0x0000000c, 0x3f800000, 0x0007002c, 0x00000007, 0x0000000d, 0x0000000a,
    0x0000000a, 0x0000000b, 0x0000000c, 0x00040017, 0x0000000e, 0x00000006, 0x00000002, 0x00040020,
    0x0000000f, 0x00000001, 0x0000000e, 0x0004003b, 0x0000000f, 0x00000010, 0x00000001, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0003003e, 0x00000009,
    0x0000000d, 0x000100fd, 0x00010038,
};

const u32 kUvSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000014, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f,
    0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004,
    0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x5f74756f, 0x6f6c6f63, 0x00000072,
    0x00040005, 0x0000000c, 0x76755f76, 0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040047, 0x0000000c, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x00040017, 0x0000000a, 0x00000006, 0x00000002, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000f, 0x00000000,
    0x0004002b, 0x00000006, 0x00000010, 0x3f800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x0006000c,
    0x0000000a, 0x0000000e, 0x00000001, 0x0000000a, 0x0000000d, 0x00050051, 0x00000006, 0x00000011,
    0x0000000e, 0x00000000, 0x00050051, 0x00000006, 0x00000012, 0x0000000e, 0x00000001, 0x00070050,
    0x00000007, 0x00000013, 0x00000011, 0x00000012, 0x0000000f, 0x00000010, 0x0003003e, 0x00000009,
    0x00000013, 0x000100fd, 0x00010038,
};

#define VK_CHECK(call)                                                                          \
    do {                                                                                        \
        const VkResult result_ = (call);                                                        \
        if (result_ != VK_SUCCESS) {                                                            \
            std::cerr << "Vulkan call failed: " #call " -> " << result_ << "\n";                \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

struct ReplayContext {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    u32 queue_family{};
    VkCommandPool command_pool{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
};

u32 FindMemoryType(const ReplayContext& ctx, u32 type_bits, VkMemoryPropertyFlags wanted) {
    for (u32 i = 0; i < ctx.memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (ctx.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return ~0u;
}

bool CreateBuffer(ReplayContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ctx.device, &info, nullptr, &buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(ctx.device, buffer, &requirements);
    const u32 type = FindMemoryType(ctx, requirements.memoryTypeBits, properties);
    if (type == ~0u)
        return false;
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = type;
    if (vkAllocateMemory(ctx.device, &alloc, nullptr, &memory) != VK_SUCCESS)
        return false;
    return vkBindBufferMemory(ctx.device, buffer, memory, 0) == VK_SUCCESS;
}

struct Mat4 {
    float m[16]{};
    static Mat4 Identity() {
        Mat4 out{};
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
        return out;
    }
    static Mat4 Perspective(float fov_y, float aspect, float near, float far) {
        Mat4 out{};
        const float f = 1.0f / std::tan(fov_y * 0.5f);
        out.m[0] = f / aspect;
        out.m[5] = f;
        out.m[10] = far / (near - far);
        out.m[11] = -1.0f;
        out.m[14] = (far * near) / (near - far);
        return out;
    }
    static Mat4 LookAt(const float eye[3], const float target[3], const float up[3]) {
        float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl < 1e-6f)
            fl = 1.0f;
        for (float& v : f)
            v /= fl;
        float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                      f[0] * up[1] - f[1] * up[0]};
        float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (sl < 1e-6f)
            sl = 1.0f;
        for (float& v : s)
            v /= sl;
        const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                            s[0] * f[1] - s[1] * f[0]};
        Mat4 out = Identity();
        out.m[0] = s[0];
        out.m[4] = s[1];
        out.m[8] = s[2];
        out.m[1] = u[0];
        out.m[5] = u[1];
        out.m[9] = u[2];
        out.m[2] = -f[0];
        out.m[6] = -f[1];
        out.m[10] = -f[2];
        out.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
        out.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
        out.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
        return out;
    }
    Mat4 operator*(const Mat4& other) const {
        Mat4 out{};
        for (u32 c = 0; c < 4; ++c) {
            for (u32 r = 0; r < 4; ++r) {
                float sum = 0.0f;
                for (u32 k = 0; k < 4; ++k)
                    sum += m[k * 4 + r] * other.m[c * 4 + k];
                out.m[c * 4 + r] = sum;
            }
        }
        return out;
    }
};

} // namespace

namespace {

/// Maps a captured Vulkan format to the format the replay creates. Only the formats a title
/// screen's sampled colour textures use are named; anything else is refused rather than guessed.
bool MapFormat(u32 captured, VkFormat& out, u32& channels) {
    switch (captured) {
    case 9: // VK_FORMAT_R8_UNORM
        out = VK_FORMAT_R8_UNORM;
        channels = 1;
        return true;
    case 37: // VK_FORMAT_R8G8B8A8_UNORM
        out = VK_FORMAT_R8G8B8A8_UNORM;
        channels = 4;
        return true;
    case 43: // VK_FORMAT_R8G8B8A8_SRGB
        out = VK_FORMAT_R8G8B8A8_SRGB;
        channels = 4;
        return true;
    case 44: // VK_FORMAT_B8G8R8A8_UNORM
        out = VK_FORMAT_B8G8R8A8_UNORM;
        channels = 4;
        return true;
    // Block-compressed formats. The captured bytes are already in the host image's format, so a
    // compressed image is uploaded as-is rather than decoded: decoding would be a lossy
    // reinterpretation of data that is already exactly what the shader sampled.
    case 131: // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
        out = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        channels = 4;
        return true;
    case 137: // VK_FORMAT_BC3_UNORM_BLOCK
        out = VK_FORMAT_BC3_UNORM_BLOCK;
        channels = 4;
        return true;
    case 138: // VK_FORMAT_BC3_SRGB_BLOCK
        out = VK_FORMAT_BC3_SRGB_BLOCK;
        channels = 4;
        return true;
    case 139: // VK_FORMAT_BC4_UNORM_BLOCK
        out = VK_FORMAT_BC4_UNORM_BLOCK;
        channels = 1;
        return true;
    case 145: // VK_FORMAT_BC5_UNORM_BLOCK
        out = VK_FORMAT_BC5_UNORM_BLOCK;
        channels = 2;
        return true;
    case 147: // VK_FORMAT_BC7_UNORM_BLOCK
        out = VK_FORMAT_BC7_UNORM_BLOCK;
        channels = 4;
        return true;
    case 148: // VK_FORMAT_BC7_SRGB_BLOCK
        out = VK_FORMAT_BC7_SRGB_BLOCK;
        channels = 4;
        return true;
    default:
        return false;
    }
}

/// Whether a format is block-compressed, which decides whether the copy extent is in blocks.
bool IsBlockCompressed(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return false;
    }
}

VkFilter ToFilter(u32 value) {
    // The capture stores the Vulkan enum value the production sampler used.
    return value == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerMipmapMode ToMipMode(u32 value) {
    return value == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

VkSamplerAddressMode ToAddress(u32 value) {
    switch (value) {
    case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 4:
        return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: deracine_textured_replay <package-directory> [output-prefix]\n";
        return 2;
    }
    const std::string directory = argv[1];
    const std::string prefix = argc > 2 ? argv[2] : "textured";
    // The comparison modes exist so the textured result can be shown to differ from an untextured
    // render and from a UV visualization, rather than merely asserted to.
    const bool untextured = argc > 3 && std::string{argv[3]} == "untextured";
    const bool show_uv = argc > 3 && std::string{argv[3]} == "uv";

    Package package{};
    std::string error;
    if (!ReadPackage(directory, package, error)) {
        std::cerr << "package read failed: " << error << "\n";
        return 1;
    }
    if (package.schema_version != 2) {
        std::cerr << "unsupported schema version\n";
        return 1;
    }
    if (!package.textured_ready) {
        std::cerr << "package is not marked diagnostic-textured-replay-ready\n";
        return 1;
    }

    // Pick the texture that actually has content.
    const Texture* texture = nullptr;
    for (const auto& candidate : package.textures) {
        if (!candidate.subresources.empty()) {
            texture = &candidate;
            break;
        }
    }
    if (texture == nullptr) {
        std::cerr << "no texture with captured content\n";
        return 1;
    }
    const TextureSubresource& sub = texture->subresources.front();

    VkFormat image_format{};
    u32 channels = 0;
    if (!MapFormat(texture->image_format, image_format, channels)) {
        std::cerr << "unsupported captured image format " << texture->image_format << "\n";
        return 1;
    }

    // The sampler the texture pairs with, or the first one if the binding did not name it.
    const Sampler* sampler = nullptr;
    for (const auto& candidate : package.samplers) {
        if (candidate.id == texture->sampler_id) {
            sampler = &candidate;
            break;
        }
    }
    if (sampler == nullptr && !package.samplers.empty()) {
        sampler = &package.samplers.front();
    }
    if (sampler == nullptr) {
        std::cerr << "no sampler in the package\n";
        return 1;
    }

    std::cout << "package: provenance=" << package.provenance_id
              << " textures=" << package.textures.size()
              << " samplers=" << package.samplers.size() << "\n";
    std::cout << "texture: id=" << texture->id << " binding=" << texture->vk_binding << " "
              << sub.width << "x" << sub.height << " format=" << texture->image_format
              << " bytes=" << sub.byte_length << "\n";
    std::cout << "sampler: mag=" << sampler->mag_filter << " min=" << sampler->min_filter
              << " mip=" << sampler->mip_filter << " aniso=" << sampler->max_anisotropy
              << " lod=[" << sampler->min_lod << "," << sampler->max_lod << "]\n";
    std::cout << "uv: binding=" << package.uv.binding << " components=" << package.uv.component_u
              << "," << package.uv.component_v << "\n";

    // Build the diagnostic vertex stream: position plus the attributed UV.
    const auto find_resource = [&package](u32 id) -> const Resource* {
        for (const auto& resource : package.resources) {
            if (resource.id == id)
                return &resource;
        }
        return nullptr;
    };
    const VertexBinding* position_binding = nullptr;
    const VertexBinding* uv_binding = nullptr;
    for (const auto& vertex : package.vertex_bindings) {
        if (vertex.binding == package.geometry.position_binding)
            position_binding = &vertex;
        if (vertex.binding == package.uv.binding)
            uv_binding = &vertex;
    }
    if (position_binding == nullptr || uv_binding == nullptr) {
        std::cerr << "position or UV binding missing\n";
        return 1;
    }
    const Resource* position_resource = find_resource(position_binding->resource_ref);
    const Resource* uv_resource = find_resource(uv_binding->resource_ref);
    if (position_resource == nullptr || uv_resource == nullptr) {
        std::cerr << "position or UV resource missing\n";
        return 1;
    }

    const u32 vertex_count = static_cast<u32>(position_resource->bytes.size() /
                                              position_binding->stride);
    const u32 uv_count = static_cast<u32>(uv_resource->bytes.size() / uv_binding->stride);
    const u32 count = std::min(vertex_count, uv_count);

    const float* lo = package.geometry.bounds_min;
    const float* hi = package.geometry.bounds_max;
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (u32 c = 0; c < 3; ++c)
        extent = std::max(extent, hi[c] - lo[c]);
    const float scale = extent > 1e-6f ? 2.0f / extent : 1.0f;

    // Interleaved position (3 floats) and UV (2 floats).
    std::vector<float> vertices(static_cast<size_t>(count) * 5);
    for (u32 v = 0; v < count; ++v) {
        for (u32 c = 0; c < 3; ++c) {
            float value = 0.0f;
            if (c < position_binding->element_count) {
                std::memcpy(&value,
                            position_resource->bytes.data() + v * position_binding->stride + c * 4,
                            sizeof(value));
            }
            vertices[v * 5 + c] = (value - centre[c]) * scale;
        }
        // The UV stream is half-float in the captured layout; convert to float here.
        for (u32 c = 0; c < 2; ++c) {
            const u32 component = c == 0 ? package.uv.component_u : package.uv.component_v;
            u16 raw = 0;
            std::memcpy(&raw,
                        uv_resource->bytes.data() + v * uv_binding->stride + component * 2,
                        sizeof(raw));
            const u32 sign = (raw >> 15) & 0x1u;
            const u32 exponent = (raw >> 10) & 0x1Fu;
            const u32 mantissa = raw & 0x3FFu;
            u32 bits = 0;
            if (exponent == 0) {
                bits = sign << 31;
            } else if (exponent == 0x1F) {
                bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
            } else {
                bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
            }
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            vertices[v * 5 + 3 + c] = value;
        }
    }

    std::vector<u32> indices;
    if (package.is_indexed && package.has_index) {
        const Resource* index_resource = find_resource(package.index.resource_ref);
        if (index_resource == nullptr) {
            std::cerr << "index resource missing\n";
            return 1;
        }
        const u32 width = package.index.index_size;
        const u32 index_count = package.index.index_count;
        if (index_count * width > index_resource->bytes.size()) {
            std::cerr << "index count exceeds captured bytes\n";
            return 1;
        }
        indices.resize(index_count);
        for (u32 i = 0; i < index_count; ++i) {
            if (width == 2) {
                u16 v = 0;
                std::memcpy(&v, index_resource->bytes.data() + i * 2, sizeof(v));
                indices[i] = v;
            } else {
                std::memcpy(&indices[i], index_resource->bytes.data() + i * 4, sizeof(u32));
            }
        }
    }

    std::cout << "replay: vertices=" << count << " indices=" << indices.size() << "\n";

    // --- Vulkan setup -------------------------------------------------------------------------
    ReplayContext ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_textured_replay";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed\n";
        return 1;
    }

    u32 device_count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, nullptr);
    if (device_count == 0) {
        std::cerr << "no Vulkan physical device\n";
        return 1;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, devices.data());
    ctx.physical = devices.front();
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physical, &props);
    std::cout << "device: " << props.deviceName << "\n";

    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &family_count, families.data());
    ctx.queue_family = ~0u;
    for (u32 i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            ctx.queue_family = i;
            break;
        }
    }
    if (ctx.queue_family == ~0u) {
        std::cerr << "no graphics queue\n";
        return 1;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = ctx.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    if (vkCreateDevice(ctx.physical, &device_info, nullptr, &ctx.device) != VK_SUCCESS) {
        std::cerr << "vkCreateDevice failed\n";
        return 1;
    }
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);
    vkGetPhysicalDeviceMemoryProperties(ctx.physical, &ctx.memory_properties);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx.queue_family;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool));

    // Vertex and index buffers.
    VkBuffer vertex_buffer{}, index_buffer{};
    VkDeviceMemory vertex_memory{}, index_memory{};
    const VkDeviceSize vertex_bytes = vertices.size() * sizeof(float);
    if (!CreateBuffer(ctx, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vertex_buffer, vertex_memory)) {
        std::cerr << "vertex buffer creation failed\n";
        return 1;
    }
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(ctx.device, vertex_memory, 0, vertex_bytes, 0, &mapped));
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vertex_bytes));
    vkUnmapMemory(ctx.device, vertex_memory);

    const VkDeviceSize index_bytes = indices.size() * sizeof(u32);
    if (index_bytes > 0) {
        if (!CreateBuffer(ctx, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          index_buffer, index_memory)) {
            std::cerr << "index buffer creation failed\n";
            return 1;
        }
        VK_CHECK(vkMapMemory(ctx.device, index_memory, 0, index_bytes, 0, &mapped));
        std::memcpy(mapped, indices.data(), static_cast<size_t>(index_bytes));
        vkUnmapMemory(ctx.device, index_memory);
    }

    // --- Texture image ------------------------------------------------------------------------
    VkImage texture_image{};
    VkDeviceMemory texture_memory{};
    VkImageView texture_view{};
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = image_format;
    // A compressed subresource records its extent in blocks, so the image is created at the pixel
    // extent the block count implies.
    const u32 block_shift = IsBlockCompressed(image_format) ? 2u : 0u;
    const u32 pixel_width = sub.width << block_shift;
    const u32 pixel_height = sub.height << block_shift;
    image_info.extent = {pixel_width, pixel_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &image_info, nullptr, &texture_image));

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(ctx.device, texture_image, &image_requirements);
    VkMemoryAllocateInfo image_alloc{};
    image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc.allocationSize = image_requirements.size;
    image_alloc.memoryTypeIndex =
        FindMemoryType(ctx, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &image_alloc, nullptr, &texture_memory));
    VK_CHECK(vkBindImageMemory(ctx.device, texture_image, texture_memory, 0));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &texture_view));

    // Upload the captured bytes through a staging buffer.
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    if (!CreateBuffer(ctx, sub.byte_length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_memory)) {
        std::cerr << "staging buffer creation failed\n";
        return 1;
    }
    VK_CHECK(vkMapMemory(ctx.device, staging_memory, 0, sub.byte_length, 0, &mapped));
    std::memcpy(mapped, package.texture_blob.data() + sub.blob_offset,
                static_cast<size_t>(sub.byte_length));
    vkUnmapMemory(ctx.device, staging_memory);

    // --- Sampler ------------------------------------------------------------------------------
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = ToFilter(sampler->mag_filter);
    sampler_info.minFilter = ToFilter(sampler->min_filter);
    sampler_info.mipmapMode = ToMipMode(sampler->mip_filter);
    sampler_info.addressModeU = ToAddress(sampler->address_u);
    sampler_info.addressModeV = ToAddress(sampler->address_v);
    sampler_info.addressModeW = ToAddress(sampler->address_w);
    sampler_info.mipLodBias = sampler->lod_bias;
    sampler_info.anisotropyEnable = sampler->anisotropy_enable ? VK_TRUE : VK_FALSE;
    sampler_info.maxAnisotropy = sampler->max_anisotropy >= 1.0f ? sampler->max_anisotropy : 1.0f;
    sampler_info.compareEnable = sampler->compare_enable ? VK_TRUE : VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.minLod = sampler->min_lod;
    sampler_info.maxLod = sampler->max_lod;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    VkSampler vk_sampler{};
    VK_CHECK(vkCreateSampler(ctx.device, &sampler_info, nullptr, &vk_sampler));

    // --- Descriptor set -----------------------------------------------------------------------
    VkDescriptorSetLayoutBinding layout_binding{};
    layout_binding.binding = 0;
    layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layout_binding.descriptorCount = 1;
    layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings = &layout_binding;
    VkDescriptorSetLayout descriptor_layout{};
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &layout_ci, nullptr, &descriptor_layout));

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    VkDescriptorPool descriptor_pool{};
    VK_CHECK(vkCreateDescriptorPool(ctx.device, &pool_ci, nullptr, &descriptor_pool));

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &descriptor_layout;
    VkDescriptorSet descriptor_set{};
    VK_CHECK(vkAllocateDescriptorSets(ctx.device, &set_alloc, &descriptor_set));

    VkDescriptorImageInfo descriptor_image{};
    descriptor_image.sampler = vk_sampler;
    descriptor_image.imageView = texture_view;
    descriptor_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptor_image;
    vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);

    // --- Render target ------------------------------------------------------------------------
    VkImage color_image{}, depth_image{};
    VkDeviceMemory color_memory{}, depth_memory{};
    VkImageView color_view{}, depth_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};

    const auto make_target = [&ctx](VkFormat format, VkImageUsageFlags usage, VkImage& image,
                                    VkDeviceMemory& memory, VkImageView& view) -> bool {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {kWidth, kHeight, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.device, &info, nullptr, &image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(ctx.device, image, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex =
            FindMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.device, &alloc, nullptr, &memory) != VK_SUCCESS)
            return false;
        if (vkBindImageMemory(ctx.device, image, memory, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = format;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(ctx.device, &view_ci, nullptr, &view) == VK_SUCCESS;
    };

    if (!make_target(VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     color_image, color_memory, color_view)) {
        std::cerr << "colour target creation failed\n";
        return 1;
    }
    if (!make_target(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     depth_image, depth_memory, depth_view)) {
        std::cerr << "depth target creation failed\n";
        return 1;
    }

    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    VkRenderPassCreateInfo pass_ci{};
    pass_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_ci.attachmentCount = 2;
    pass_ci.pAttachments = attachments;
    pass_ci.subpassCount = 1;
    pass_ci.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(ctx.device, &pass_ci, nullptr, &render_pass));

    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fb_ci{};
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = render_pass;
    fb_ci.attachmentCount = 2;
    fb_ci.pAttachments = fb_views;
    fb_ci.width = kWidth;
    fb_ci.height = kHeight;
    fb_ci.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &fb_ci, nullptr, &framebuffer));

    // --- Pipeline -----------------------------------------------------------------------------
    const auto make_module = [&ctx](const u32* code, size_t words) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = words * sizeof(u32);
        info.pCode = code;
        VkShaderModule module{};
        if (vkCreateShaderModule(ctx.device, &info, nullptr, &module) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return module;
    };
    VkShaderModule vert_module = make_module(kVertSpv, sizeof(kVertSpv) / sizeof(u32));
    VkShaderModule frag_module =
        make_module(untextured ? kFlatSpv : (show_uv ? kUvSpv : kFragSpv),
                    untextured ? sizeof(kFlatSpv) / sizeof(u32)
                               : (show_uv ? sizeof(kUvSpv) / sizeof(u32)
                                          : sizeof(kFragSpv) / sizeof(u32)));
    if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE) {
        std::cerr << "shader module creation failed\n";
        return 1;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding_desc{};
    binding_desc.binding = 0;
    binding_desc.stride = 5 * sizeof(float);
    binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = 3 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding_desc;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(kWidth);
    viewport.height = static_cast<float>(kHeight);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.size = sizeof(float) * 16;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    VkPipelineLayout pipeline_layout{};
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.stageCount = 2;
    pipeline_ci.pStages = stages;
    pipeline_ci.pVertexInputState = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState = &viewport_state;
    pipeline_ci.pRasterizationState = &raster;
    pipeline_ci.pMultisampleState = &multisample;
    pipeline_ci.pDepthStencilState = &depth_state;
    pipeline_ci.pColorBlendState = &blend;
    pipeline_ci.layout = pipeline_layout;
    pipeline_ci.renderPass = render_pass;
    pipeline_ci.subpass = 0;
    VkPipeline pipeline{};
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr,
                                       &pipeline));

    // --- Record, submit, read back ------------------------------------------------------------
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    if (!CreateBuffer(ctx, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback, readback_memory)) {
        std::cerr << "readback buffer creation failed\n";
        return 1;
    }

    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cmd_alloc, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    // Upload the texture: barrier to transfer-dst, copy, barrier to shader-read.
    VkImageMemoryBarrier2 to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.image = texture_image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_transfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    // vkCmdCopyBufferToImage takes the extent in texels even for a block-compressed format, so the
    // block count the capture recorded is expanded back to the pixel extent here. Passing the
    // block count would copy a quarter of the image and leave the rest undefined.
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {pixel_width, pixel_height, 1};
    vkCmdCopyBufferToImage(cmd, staging, texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    VkImageMemoryBarrier2 to_shader = to_transfer;
    to_shader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dep.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkClearValue clears[2]{};
    clears[0].color = {{0.05f, 0.05f, 0.08f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea = {{0, 0}, {kWidth, kHeight}};
    rp_begin.clearValueCount = 2;
    rp_begin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    const float eye[3] = {0.0f, 0.0f, 3.2f};
    const float target[3] = {0.0f, 0.0f, 0.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const Mat4 mvp = Mat4::Perspective(0.9f, 1.0f, 0.1f, 100.0f) *
                     Mat4::LookAt(eye, target, up);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                            &descriptor_set, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp), mvp.m);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    if (index_bytes > 0) {
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, static_cast<u32>(indices.size()), 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, count, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy readback_copy{};
    readback_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    readback_copy.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1,
                           &readback_copy);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    void* pixels = nullptr;
    if (vkMapMemory(ctx.device, readback_memory, 0, image_bytes, 0, &pixels) == VK_SUCCESS) {
        const auto out_path = std::filesystem::path{directory} / (prefix + ".ppm");
        std::ofstream ppm{out_path, std::ios::binary};
        if (ppm) {
            ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
            const auto* src = static_cast<const u8*>(pixels);
            for (u32 i = 0; i < kWidth * kHeight; ++i) {
                ppm.put(static_cast<char>(src[i * 4 + 0]));
                ppm.put(static_cast<char>(src[i * 4 + 1]));
                ppm.put(static_cast<char>(src[i * 4 + 2]));
            }
            std::cout << "replay: wrote " << out_path.string() << "\n";
        }
        vkUnmapMemory(ctx.device, readback_memory);
    }

    std::cout << "replay: drawIndexed(" << indices.size() << ", 1, 0, 0, 0) submitted and "
              << "completed with the captured texture\n";
    std::cout << "mode: " << (untextured ? "untextured" : (show_uv ? "uv" : "textured")) << "\n";
    std::cout << "note: diagnostic shader only; no original material, no lighting\n";

    vkDestroyBuffer(ctx.device, readback, nullptr);
    vkFreeMemory(ctx.device, readback_memory, nullptr);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
    vkDestroyShaderModule(ctx.device, vert_module, nullptr);
    vkDestroyShaderModule(ctx.device, frag_module, nullptr);
    vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, descriptor_layout, nullptr);
    vkDestroySampler(ctx.device, vk_sampler, nullptr);
    vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
    vkDestroyRenderPass(ctx.device, render_pass, nullptr);
    vkDestroyImageView(ctx.device, color_view, nullptr);
    vkDestroyImageView(ctx.device, depth_view, nullptr);
    vkDestroyImageView(ctx.device, texture_view, nullptr);
    vkDestroyImage(ctx.device, color_image, nullptr);
    vkDestroyImage(ctx.device, depth_image, nullptr);
    vkDestroyImage(ctx.device, texture_image, nullptr);
    vkFreeMemory(ctx.device, color_memory, nullptr);
    vkFreeMemory(ctx.device, depth_memory, nullptr);
    vkFreeMemory(ctx.device, texture_memory, nullptr);
    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, staging_memory, nullptr);
    if (index_buffer)
        vkDestroyBuffer(ctx.device, index_buffer, nullptr);
    if (index_memory)
        vkFreeMemory(ctx.device, index_memory, nullptr);
    vkDestroyBuffer(ctx.device, vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, vertex_memory, nullptr);
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return 0;
}
