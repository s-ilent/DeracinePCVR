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

namespace {

/// SHA-256 of a byte range, as lowercase hex.
///
/// The package declares a hash for each sidecar, and checking it is what makes a substituted or
/// truncated file detectable. A small implementation is used rather than a new dependency, because
/// this reader is deliberately linkable without one.
std::string Sha256Hex(const std::vector<u8>& bytes) {
    static constexpr u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};

    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    std::vector<u8> message = bytes;
    const u64 bit_length = static_cast<u64>(bytes.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<u8>((bit_length >> (i * 8)) & 0xFF));
    }

    const auto rotr = [](u32 value, u32 count) {
        return (value >> count) | (value << (32 - count));
    };

    for (size_t offset = 0; offset < message.size(); offset += 64) {
        u32 w[64]{};
        for (u32 i = 0; i < 16; ++i) {
            w[i] = (static_cast<u32>(message[offset + i * 4]) << 24) |
                   (static_cast<u32>(message[offset + i * 4 + 1]) << 16) |
                   (static_cast<u32>(message[offset + i * 4 + 2]) << 8) |
                   static_cast<u32>(message[offset + i * 4 + 3]);
        }
        for (u32 i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        u32 e = h[4], f = h[5], g = h[6], hh = h[7];
        for (u32 i = 0; i < 64; ++i) {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 temp1 = hh + s1 + ch + k[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const u32 value : h) {
        for (int i = 3; i >= 0; --i) {
            const u8 byte = static_cast<u8>((value >> (i * 8)) & 0xFF);
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        }
    }
    return out;
}

/// Reads a sidecar and checks its length against what the package declares.
///
/// A sidecar that is shorter than declared would be read past its end, and one that is longer
/// would mean the package and the file disagree about what was captured. Both are refused rather
/// than tolerated.
bool ReadCheckedSidecar(const std::string& directory, const std::string& name, u64 expected_length,
                        std::vector<u8>& out, std::string& error) {
    if (name.empty()) {
        error = "a sidecar reference is empty";
        return false;
    }
    // A sidecar name is a bare filename. A path separator or a parent reference would let a package
    // read outside its own directory, so it is refused rather than resolved.
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos) {
        error = "sidecar reference is not a bare filename: " + name;
        return false;
    }
    std::ifstream in{std::filesystem::path{directory} / name, std::ios::binary};
    if (!in) {
        error = "cannot open sidecar " + name;
        return false;
    }
    out.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    if (expected_length != 0 && out.size() != expected_length) {
        error = "sidecar " + name + " is " + std::to_string(out.size()) + " bytes but the package "
                "declares " + std::to_string(expected_length);
        return false;
    }
    return true;
}

} // namespace

bool Read(const std::string& directory, Package& out, std::string& error,
          bool require_effective) {
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
        // A version outside the accepted set is refused rather than reinterpreted. A schema-4 field
        // read from a schema-2 package would be absent, and an absent field read as a default is
        // not a captured value.
        if (out.schema_version != SchemaResourceOnly &&
            out.schema_version != SchemaEffectiveShader) {
            error = "schema version " + std::to_string(out.schema_version) +
                    " is not " + std::to_string(SchemaResourceOnly) + " or " +
                    std::to_string(SchemaEffectiveShader);
            return false;
        }
        if (require_effective && out.schema_version != SchemaEffectiveShader) {
            error = "effective-shader mode requires schema " +
                    std::to_string(SchemaEffectiveShader) + ", but the package is schema " +
                    std::to_string(out.schema_version);
            return false;
        }
        out.provenance_id = doc.at("provenance_id").get<u64>();
        // The pipeline block is optional: a resource-only package need not carry one, and its
        // absence means the draw was not indexed rather than that the package is malformed.
        out.is_indexed =
            doc.contains("pipeline") && doc.at("pipeline").value("is_indexed", false);

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
            out.readiness.shader_binaries_ready = entry.value("shader_binaries_ready", false);
            out.readiness.shader_bindings_ready = entry.value("shader_bindings_ready", false);
            out.readiness.shader_parameters_ready = entry.value("shader_parameters_ready", false);
            out.readiness.pipeline_state_ready_original =
                entry.value("pipeline_state_ready_original", false);
            out.readiness.reference_output_ready = entry.value("reference_output_ready", false);
            out.readiness.original_shader_replay_ready =
                entry.value("original_shader_replay_ready", false);
        }

        out.session_id = doc.value("session_id", 0ull);
        out.execution_id = doc.value("execution_id", 0ull);

        // --- Schema 4: effective shader stages -------------------------------------------------
        for (const auto& entry : doc.value("shader_stages", nlohmann::json::array())) {
            ShaderStage stage{};
            stage.logical_stage = entry.value("logical_stage", 0u);
            stage.hardware_stage = entry.value("hardware_stage", 0u);
            stage.program_hash = entry.value("program_hash", 0ull);
            stage.permutation_index = entry.value("permutation_index", 0u);
            stage.permutation_hash = entry.value("permutation_hash", 0ull);
            stage.patched = entry.value("patched", false);
            stage.effective_sha256 = entry.value("effective_sha256", std::string{});
            stage.generated_sha256 = entry.value("generated_sha256", std::string{});
            stage.entry_point = entry.value("entry_point", std::string{});
            stage.effective_byte_length = entry.value("effective_byte_length", 0ull);
            stage.generated_byte_length = entry.value("gcn_byte_length", 0ull);
            stage.effective_file = entry.value("effective_file", std::string{});
            stage.generated_file = entry.value("gcn_file", std::string{});
            out.shader_stages.push_back(std::move(stage));
        }

        // --- Schema 4: descriptor interfaces ---------------------------------------------------
        for (const auto& entry : doc.value("shader_interfaces", nlohmann::json::array())) {
            ShaderInterface interface{};
            interface.program_hash = entry.value("program_hash", 0ull);
            interface.permutation_index = entry.value("permutation_index", 0u);
            interface.permutation_hash = entry.value("permutation_hash", 0ull);
            interface.hardware_stage = entry.value("hardware_stage", 0u);
            interface.logical_stage = entry.value("logical_stage", 0u);
            interface.patched = entry.value("patched", false);
            interface.push_constant_offset = entry.value("push_constant_offset", 0u);
            interface.push_constant_size = entry.value("push_constant_size", 0u);
            interface.push_constant_stage_flags = entry.value("push_constant_stage_flags", 0u);
            interface.unified_binding_count = entry.value("unified_binding_count", 0u);
            for (const auto& d : entry.value("descriptors", nlohmann::json::array())) {
                ShaderDescriptor descriptor{};
                descriptor.set = d.value("set", 0u);
                descriptor.binding = d.value("binding", 0u);
                descriptor.descriptor_class = d.value("descriptor_class", 0u);
                descriptor.count = d.value("count", 1u);
                descriptor.stage_flags = d.value("stage_flags", 0u);
                descriptor.guest_resource_index = d.value("guest_resource_index", 0xFFFFFFFFu);
                descriptor.written = d.value("written", false);
                interface.descriptors.push_back(descriptor);
            }
            out.shader_interfaces.push_back(std::move(interface));
        }

        // --- Schema 4: pipeline state ----------------------------------------------------------
        if (doc.contains("pipeline_state")) {
            const auto& entry = doc.at("pipeline_state");
            auto& state = out.pipeline_state;
            state.has_topology = entry.value("has_topology", false);
            state.topology = entry.value("topology", 0u);
            state.cull_mode = entry.value("cull_mode", 0u);
            state.front_face = entry.value("front_face", 0u);
            state.polygon_mode = entry.value("polygon_mode", 0u);
            state.depth_test = entry.value("depth_test", false);
            state.depth_write = entry.value("depth_write", false);
            state.depth_compare_op = entry.value("depth_compare_op", 0u);
            state.depth_format = entry.value("depth_format", 0u);
            state.samples = entry.value("samples", 0u);
            state.viewport_x = entry.value("viewport_x", 0.0f);
            state.viewport_y = entry.value("viewport_y", 0.0f);
            state.viewport_width = entry.value("viewport_width", 0.0f);
            state.viewport_height = entry.value("viewport_height", 0.0f);
            state.viewport_min_depth = entry.value("viewport_min_depth", 0.0f);
            state.viewport_max_depth = entry.value("viewport_max_depth", 0.0f);
            state.push_constant_offset = entry.value("push_constant_offset", 0u);
            state.push_constant_size = entry.value("push_constant_size", 0u);
            for (const auto& value : entry.value("color_formats", nlohmann::json::array())) {
                state.color_formats.push_back(value.get<u32>());
            }
            for (const auto& value : entry.value("blend_enable", nlohmann::json::array())) {
                state.blend_enable.push_back(value.get<u32>());
            }
            for (const auto& value : entry.value("write_mask", nlohmann::json::array())) {
                state.write_mask.push_back(value.get<u32>());
            }
            for (const auto& value : entry.value("push_constant_bytes", nlohmann::json::array())) {
                state.push_constant_bytes.push_back(static_cast<u8>(value.get<u32>()));
            }
        }

        // --- Schema 4: reference output --------------------------------------------------------
        if (doc.contains("reference_output")) {
            const auto& entry = doc.at("reference_output");
            auto& reference = out.reference;
            reference.width = entry.value("width", 0u);
            reference.height = entry.value("height", 0u);
            reference.format = entry.value("format", 0u);
            reference.bytes_per_pixel = entry.value("bytes_per_pixel", 0u);
            reference.pre_sha256 = entry.value("pre_sha256", std::string{});
            reference.post_sha256 = entry.value("post_sha256", std::string{});
            reference.pre_file = entry.value("pre_file", std::string{});
            reference.post_file = entry.value("post_file", std::string{});
            reference.has_pre = entry.value("has_pre", false);
        }
    } catch (const std::exception& e) {
        error = std::string{"package.json is missing a required field: "} + e.what();
        return false;
    }

    // --- Sidecars ------------------------------------------------------------------------------
    // A schema-4 package names its shader and reference sidecars. Each is loaded and checked
    // against the length and hash the package declares, so a truncated or substituted file is
    // refused rather than used.
    for (auto& stage : out.shader_stages) {
        if (stage.effective_file.empty()) {
            continue;
        }
        if (!ReadCheckedSidecar(directory, stage.effective_file, stage.effective_byte_length,
                                stage.effective_bytes, error)) {
            return false;
        }
        if (!stage.effective_sha256.empty()) {
            const std::string actual = Sha256Hex(stage.effective_bytes);
            if (actual != stage.effective_sha256) {
                error = "shader sidecar " + stage.effective_file + " hashes to " + actual +
                        " but the package declares " + stage.effective_sha256;
                return false;
            }
        }
    }
    if (!out.reference.post_file.empty()) {
        const u64 expected = static_cast<u64>(out.reference.width) * out.reference.height *
                             out.reference.bytes_per_pixel;
        if (!ReadCheckedSidecar(directory, out.reference.post_file, expected,
                                out.reference.post_bytes, error)) {
            return false;
        }
        if (!out.reference.post_sha256.empty()) {
            const std::string actual = Sha256Hex(out.reference.post_bytes);
            if (actual != out.reference.post_sha256) {
                error = "reference sidecar " + out.reference.post_file + " hashes to " + actual +
                        " but the package declares " + out.reference.post_sha256;
                return false;
            }
        }
    }
    if (out.reference.has_pre && !out.reference.pre_file.empty()) {
        const u64 expected = static_cast<u64>(out.reference.width) * out.reference.height *
                             out.reference.bytes_per_pixel;
        if (!ReadCheckedSidecar(directory, out.reference.pre_file, expected,
                                out.reference.pre_bytes, error)) {
            return false;
        }
        if (!out.reference.pre_sha256.empty()) {
            const std::string actual = Sha256Hex(out.reference.pre_bytes);
            if (actual != out.reference.pre_sha256) {
                error = "reference sidecar " + out.reference.pre_file + " hashes to " + actual +
                        " but the package declares " + out.reference.pre_sha256;
                return false;
            }
        }
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
