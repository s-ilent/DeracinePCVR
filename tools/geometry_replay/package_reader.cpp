// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "package_reader.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

namespace package {

namespace {

std::vector<u8> ReadSidecar(const std::string& directory, const char* name) {
    std::ifstream in{std::filesystem::path{directory} / name, std::ios::binary};
    std::vector<u8> bytes;
    if (in) {
        bytes.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    }
    return bytes;
}

} // namespace

bool Read(const std::string& directory, Package& out, std::string& error) {
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

    const auto blob = ReadSidecar(directory, "resources.bin");
    out.texture_blob = ReadSidecar(directory, "textures.bin");

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
            out.index.first_index = entry.value("first_index", 0u);
            out.index.base_vertex = entry.value("base_vertex", 0u);
            out.index.instance_count = entry.value("instance_count", 0u);
            out.index.first_instance = entry.value("first_instance", 0u);
            out.index.resource_ref = entry.value("resource_ref", 0u);
            out.index.guest_base = entry.value("guest_base", 0ull);
            out.index.guest_size = entry.value("guest_size", 0ull);
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

        for (const auto& entry : doc.value("textures", nlohmann::json::array())) {
            Texture texture{};
            texture.id = entry.at("id").get<u32>();
            texture.vk_binding = entry.value("vk_binding", 0u);
            texture.image_format = entry.value("image_format", 0u);
            texture.view_format = entry.value("view_format", 0u);
            texture.width = entry.value("width", 0u);
            texture.height = entry.value("height", 0u);
            texture.block_columns = entry.value("block_columns", 0u);
            texture.block_rows = entry.value("block_rows", 0u);
            texture.block_texels_x = entry.value("block_texels_x", 1u);
            texture.block_texels_y = entry.value("block_texels_y", 1u);
            texture.bytes_per_block = entry.value("bytes_per_block", 0u);
            texture.transfer_id = entry.value("transfer_id", 0ull);
            texture.sampler_id = entry.value("sampler_id", 0ull);
            texture.sampler_verified = entry.value("sampler_verified", false);
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

        for (const auto& entry : doc.value("samplers", nlohmann::json::array())) {
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

        if (doc.contains("readiness")) {
            const auto& entry = doc.at("readiness");
            out.readiness.geometry_ready = entry.value("geometry_ready", false);
            out.readiness.texture_binding_ready = entry.value("texture_binding_ready", false);
            out.readiness.texture_contents_ready = entry.value("texture_contents_ready", false);
            out.readiness.sampler_ready = entry.value("sampler_ready", false);
            out.readiness.diagnostic_textured_replay_ready =
                entry.value("diagnostic_textured_replay_ready", false);
        }
    } catch (const std::exception& e) {
        error = std::string{"package.json is missing a required field: "} + e.what();
        return false;
    }

    error.clear();
    return true;
}

const Resource* FindResource(const Package& package, u32 id) {
    for (const auto& resource : package.resources) {
        if (resource.id == id)
            return &resource;
    }
    return nullptr;
}

const Texture* FindTextured(const Package& package) {
    for (const auto& texture : package.textures) {
        if (!texture.subresources.empty())
            return &texture;
    }
    return nullptr;
}

const Sampler* FindSampler(const Package& package, const Texture& texture) {
    if (texture.sampler_id == 0)
        return nullptr;
    for (const auto& sampler : package.samplers) {
        if (sampler.id == texture.sampler_id)
            return &sampler;
    }
    return nullptr;
}

} // namespace package
