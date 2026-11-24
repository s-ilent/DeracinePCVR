// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/draw_package.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "common/logging/log.h"

namespace Core::DrawPackage {

namespace {

const char* StatusName(u8 status) noexcept {
    switch (status) {
    case 0:
        return "guest_memory_observed";
    case 1:
        return "host_content_verified";
    case 2:
        return "host_content_captured";
    case 3:
        return "metadata_only";
    case 4:
        return "partial";
    case 5:
        return "unsupported";
    case 6:
        return "invalid";
    default:
        return "unknown";
    }
}

/// A resource is usable for replay only when its content was established from the host resource
/// the draw actually read. Guest-memory observation is not enough.
[[nodiscard]] std::string EscapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace

bool WritePackage(const Package& package, const std::string& directory) noexcept {
    try {
        std::string error;
        if (!ValidatePackage(package, error)) {
            LOG_WARNING(Core_Linker, "Draw package refused: {}", error);
            return false;
        }

        const auto dir = std::filesystem::path{directory};
        if (!std::filesystem::exists(dir)) {
            LOG_WARNING(Core_Linker, "Draw package refused: directory missing {}",
                        dir.string());
            return false;
        }
        const auto json_path = dir / "package.json";
        const auto blob_path = dir / "resources.bin";

        // Write the resource bytes first, so a reader that finds the JSON always finds the blob.
        std::ofstream blob{blob_path, std::ios::binary | std::ios::trunc};
        if (!blob) {
            LOG_WARNING(Core_Linker, "Draw package refused: cannot open {}", blob_path.string());
            return false;
        }
        u64 blob_offset = 0;
        std::map<u32, u64> offsets;
        for (const auto& resource : package.resources) {
            if (resource.bytes.empty())
                continue;
            offsets[resource.id] = blob_offset;
            blob.write(reinterpret_cast<const char*>(resource.bytes.data()),
                       static_cast<std::streamsize>(resource.bytes.size()));
            blob_offset += resource.bytes.size();
        }
        blob.close();

        // Texture payload goes in its own sidecar, so a reader can stream it separately and a
        // geometry-only consumer never has to skip past it.
        if (!package.texture_bytes.empty()) {
            const auto texture_path = dir / "textures.bin";
            std::ofstream texture_blob{texture_path, std::ios::binary | std::ios::trunc};
            if (!texture_blob) {
                LOG_WARNING(Core_Linker, "Draw package refused: cannot open {}",
                            texture_path.string());
                return false;
            }
            texture_blob.write(reinterpret_cast<const char*>(package.texture_bytes.data()),
                               static_cast<std::streamsize>(package.texture_bytes.size()));
        }

        std::ostringstream json;
        json << "{\n";
        json << "  \"schema_version\": " << package.schema_version << ",\n";
        json << "  \"scene\": \"" << EscapeJson(package.scene) << "\",\n";
        json << "  \"provenance_id\": " << package.provenance_id << ",\n";
        json << "  \"pipeline\": {\n";
        json << "    \"pipeline_hash\": " << package.pipeline_hash << ",\n";
        json << "    \"vs_hash\": " << package.vs_hash << ",\n";
        json << "    \"ps_hash\": " << package.ps_hash << ",\n";
        json << "    \"primitive_type\": " << package.primitive_type << ",\n";
        json << "    \"is_indexed\": " << (package.is_indexed ? "true" : "false") << ",\n";
        json << "    \"num_indices\": " << package.num_indices << ",\n";
        json << "    \"num_instances\": " << package.num_instances << "\n";
        json << "  },\n";

        json << "  \"resources\": [\n";
        for (size_t i = 0; i < package.resources.size(); ++i) {
            const auto& resource = package.resources[i];
            json << "    {\n";
            json << "      \"id\": " << resource.id << ",\n";
            json << "      \"requested_base\": " << resource.requested_base << ",\n";
            json << "      \"requested_size\": " << resource.requested_size << ",\n";
            json << "      \"clamped_base\": " << resource.clamped_base << ",\n";
            json << "      \"clamped_size\": " << resource.clamped_size << ",\n";
            json << "      \"content_hash\": " << resource.content_hash << ",\n";
            json << "      \"status\": \"" << StatusName(resource.status) << "\",\n";
            json << "      \"byte_length\": " << resource.bytes.size() << ",\n";
            json << "      \"blob_offset\": "
                 << (offsets.contains(resource.id) ? offsets[resource.id] : 0) << "\n";
            json << "    }" << (i + 1 < package.resources.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        json << "  \"shader_buffers\": [\n";
        for (size_t i = 0; i < package.bindings.size(); ++i) {
            const auto& binding = package.bindings[i];
            json << "    {\n";
            json << "      \"stage\": " << static_cast<u32>(binding.stage) << ",\n";
            json << "      \"resource_index\": " << binding.resource_index << ",\n";
            json << "      \"sharp_idx\": " << binding.sharp_idx << ",\n";
            json << "      \"vk_binding\": " << binding.vk_binding << ",\n";
            json << "      \"vk_offset\": " << binding.vk_offset << ",\n";
            json << "      \"vk_range\": " << binding.vk_range << ",\n";
            json << "      \"buffer_id\": " << binding.buffer_id << ",\n";
            json << "      \"resource_ref\": " << binding.resource_ref << ",\n";
            json << "      \"status\": \"" << StatusName(binding.status) << "\"\n";
            json << "    }" << (i + 1 < package.bindings.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        json << "  \"vertex_bindings\": [\n";
        for (size_t i = 0; i < package.vertex_bindings.size(); ++i) {
            const auto& vertex = package.vertex_bindings[i];
            json << "    {\n";
            json << "      \"binding\": " << vertex.binding << ",\n";
            json << "      \"host_buffer\": " << vertex.host_buffer << ",\n";
            json << "      \"host_offset\": " << vertex.host_offset << ",\n";
            json << "      \"guest_base\": " << vertex.guest_base << ",\n";
            json << "      \"guest_size\": " << vertex.guest_size << ",\n";
            json << "      \"stride\": " << vertex.stride << ",\n";
            json << "      \"input_rate\": " << vertex.input_rate << ",\n";
            json << "      \"divisor\": " << vertex.divisor << ",\n";
            json << "      \"upload_sequence\": " << vertex.upload_sequence << ",\n";
            json << "      \"content_hash\": " << vertex.content_hash << ",\n";
            json << "      \"resource_ref\": " << vertex.resource_ref << ",\n";
            json << "      \"semantic\": " << vertex.semantic << ",\n";
            json << "      \"vk_format\": " << vertex.vk_format << ",\n";
            json << "      \"attr_offset\": " << vertex.attr_offset << ",\n";
            json << "      \"element_count\": " << vertex.element_count << ",\n";
            json << "      \"vs_hash\": " << vertex.vs_hash << ",\n";
            json << "      \"ps_hash\": " << vertex.ps_hash << ",\n";
            json << "      \"status\": \"" << StatusName(vertex.status) << "\"\n";
            json << "    }" << (i + 1 < package.vertex_bindings.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        if (package.has_index_binding) {
            const auto& index = package.index_binding;
            json << "  \"index_binding\": {\n";
            json << "    \"host_buffer\": " << index.host_buffer << ",\n";
            json << "    \"host_offset\": " << index.host_offset << ",\n";
            json << "    \"guest_base\": " << index.guest_base << ",\n";
            json << "    \"guest_size\": " << index.guest_size << ",\n";
            json << "    \"index_size\": " << index.index_size << ",\n";
            json << "    \"index_count\": " << index.index_count << ",\n";
            json << "    \"first_index\": " << index.first_index << ",\n";
            json << "    \"base_vertex\": " << index.base_vertex << ",\n";
            json << "    \"instance_count\": " << index.instance_count << ",\n";
            json << "    \"first_instance\": " << index.first_instance << ",\n";
            json << "    \"primitive_type\": " << index.primitive_type << ",\n";
            json << "    \"index_type\": " << static_cast<u32>(index.index_type) << ",\n";
            json << "    \"swap_mode\": " << static_cast<u32>(index.swap_mode) << ",\n";
            json << "    \"upload_sequence\": " << index.upload_sequence << ",\n";
            json << "    \"content_hash\": " << index.content_hash << ",\n";
            json << "    \"resource_ref\": " << index.resource_ref << ",\n";
            json << "    \"status\": \"" << StatusName(index.status) << "\"\n";
            json << "  },\n";
        }

        const auto& geo = package.geometry;
        json << "  \"geometry\": {\n";
        json << "    \"position_binding\": " << geo.position_binding << ",\n";
        json << "    \"position_components\": " << geo.position_components << ",\n";
        json << "    \"position_evidence\": \"" << EscapeJson(geo.position_evidence) << "\",\n";
        json << "    \"bounds_min\": [" << geo.bounds_min[0] << ", " << geo.bounds_min[1] << ", "
             << geo.bounds_min[2] << "],\n";
        json << "    \"bounds_max\": [" << geo.bounds_max[0] << ", " << geo.bounds_max[1] << ", "
             << geo.bounds_max[2] << "],\n";
        json << "    \"is_three_dimensional\": "
             << (geo.is_three_dimensional ? "true" : "false") << ",\n";
        json << "    \"triangle_count\": " << geo.triangle_count << ",\n";
        json << "    \"degenerate_triangles\": " << geo.degenerate_triangles << ",\n";
        json << "    \"unique_referenced_vertices\": " << geo.unique_referenced_vertices << ",\n";
        json << "    \"nonfinite_vertices\": " << geo.nonfinite_vertices << "\n";
        json << "  },\n";

        json << "  \"textures\": [\n";
        for (size_t i = 0; i < package.textures.size(); ++i) {
            const auto& texture = package.textures[i];
            json << "    {\n";
            json << "      \"id\": " << texture.id << ",\n";
            json << "      \"vk_binding\": " << texture.vk_binding << ",\n";
            json << "      \"stage\": " << texture.stage << ",\n";
            json << "      \"stage_hash\": " << texture.stage_hash << ",\n";
            json << "      \"guest_address\": " << texture.guest_address << ",\n";
            json << "      \"image_id\": " << texture.image_id << ",\n";
            json << "      \"image_uid\": " << texture.image_uid << ",\n";
            json << "      \"image_format\": " << texture.image_format << ",\n";
            json << "      \"view_format\": " << texture.view_format << ",\n";
            json << "      \"aspect\": " << texture.aspect << ",\n";
            json << "      \"mip_first\": " << texture.mip_first << ",\n";
            json << "      \"mip_count\": " << texture.mip_count << ",\n";
            json << "      \"layer_first\": " << texture.layer_first << ",\n";
            json << "      \"layer_count\": " << texture.layer_count << ",\n";
            json << "      \"width\": " << texture.width << ",\n";
            json << "      \"height\": " << texture.height << ",\n";
            json << "      \"depth\": " << texture.depth << ",\n";
            json << "      \"samples\": " << texture.samples << ",\n";
            json << "      \"sampler_id\": " << texture.sampler_id << ",\n";
            json << "      \"status\": \"" << StatusName(texture.status) << "\",\n";
            json << "      \"limitation\": \"" << EscapeJson(texture.limitation) << "\",\n";
            json << "      \"subresources\": [\n";
            for (size_t j = 0; j < texture.subresources.size(); ++j) {
                const auto& sub = texture.subresources[j];
                json << "        {\"mip\": " << sub.mip << ", \"layer\": " << sub.layer
                     << ", \"width\": " << sub.width << ", \"height\": " << sub.height
                     << ", \"row_pitch\": " << sub.row_pitch
                     << ", \"blob_offset\": " << sub.blob_offset
                     << ", \"byte_length\": " << sub.byte_length
                     << ", \"content_hash\": " << sub.content_hash << "}"
                     << (j + 1 < texture.subresources.size() ? "," : "") << "\n";
            }
            json << "      ]\n";
            json << "    }" << (i + 1 < package.textures.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        json << "  \"samplers\": [\n";
        for (size_t i = 0; i < package.samplers.size(); ++i) {
            const auto& sampler = package.samplers[i];
            json << "    {\n";
            json << "      \"id\": " << sampler.id << ",\n";
            json << "      \"guest_hash\": " << sampler.guest_hash << ",\n";
            json << "      \"mag_filter\": " << sampler.mag_filter << ",\n";
            json << "      \"min_filter\": " << sampler.min_filter << ",\n";
            json << "      \"mip_filter\": " << sampler.mip_filter << ",\n";
            json << "      \"address_u\": " << sampler.address_u << ",\n";
            json << "      \"address_v\": " << sampler.address_v << ",\n";
            json << "      \"address_w\": " << sampler.address_w << ",\n";
            json << "      \"lod_bias\": " << sampler.lod_bias << ",\n";
            json << "      \"min_lod\": " << sampler.min_lod << ",\n";
            json << "      \"max_lod\": " << sampler.max_lod << ",\n";
            json << "      \"anisotropy_enable\": "
                 << (sampler.anisotropy_enable ? "true" : "false") << ",\n";
            json << "      \"max_anisotropy\": " << sampler.max_anisotropy << ",\n";
            json << "      \"compare_enable\": "
                 << (sampler.compare_enable ? "true" : "false") << ",\n";
            json << "      \"compare_op\": " << sampler.compare_op << ",\n";
            json << "      \"border_color\": " << sampler.border_color << ",\n";
            json << "      \"unnormalized_coordinates\": "
                 << (sampler.unnormalized_coordinates ? "true" : "false") << "\n";
            json << "    }" << (i + 1 < package.samplers.size() ? "," : "") << "\n";
        }
        json << "  ],\n";

        json << "  \"texture_coordinate\": {\n";
        json << "    \"binding\": " << package.texture_coordinate.binding << ",\n";
        json << "    \"component_u\": " << package.texture_coordinate.component_u << ",\n";
        json << "    \"component_v\": " << package.texture_coordinate.component_v << ",\n";
        json << "    \"evidence\": \"" << EscapeJson(package.texture_coordinate.evidence)
             << "\",\n";
        json << "    \"min_u\": " << package.texture_coordinate.min_u << ",\n";
        json << "    \"max_u\": " << package.texture_coordinate.max_u << ",\n";
        json << "    \"min_v\": " << package.texture_coordinate.min_v << ",\n";
        json << "    \"max_v\": " << package.texture_coordinate.max_v << ",\n";
        json << "    \"looks_like_uv\": "
             << (package.texture_coordinate.looks_like_uv ? "true" : "false") << "\n";
        json << "  },\n";

        json << "  \"readiness\": {\n";
        json << "    \"shader_buffers_ready\": "
             << (package.readiness.shader_buffers_ready ? "true" : "false") << ",\n";
        json << "    \"geometry_ready\": "
             << (package.readiness.geometry_ready ? "true" : "false") << ",\n";
        json << "    \"texture_binding_ready\": "
             << (package.readiness.texture_binding_ready ? "true" : "false") << ",\n";
        json << "    \"texture_contents_ready\": "
             << (package.readiness.texture_contents_ready ? "true" : "false") << ",\n";
        json << "    \"sampler_ready\": "
             << (package.readiness.sampler_ready ? "true" : "false") << ",\n";
        json << "    \"pipeline_state_ready\": "
             << (package.readiness.pipeline_state_ready ? "true" : "false") << ",\n";
        json << "    \"diagnostic_geometry_replay_ready\": "
             << (package.readiness.diagnostic_geometry_replay_ready ? "true" : "false") << ",\n";
        json << "    \"diagnostic_textured_replay_ready\": "
             << (package.readiness.diagnostic_textured_replay_ready ? "true" : "false") << ",\n";
        json << "    \"original_material_ready\": "
             << (package.readiness.original_material_ready ? "true" : "false") << ",\n";
        json << "    \"limitation\": \"" << EscapeJson(package.readiness.limitation) << "\"\n";
        json << "  }\n";
        json << "}\n";

        std::ofstream out{json_path, std::ios::trunc};
        if (!out) {
            LOG_WARNING(Core_Linker, "Draw package refused: cannot open {}", json_path.string());
            return false;
        }
        out << json.str();
        out.close();

        LOG_INFO(Core_Linker,
                 "DERACINE_DRAW_PACKAGE_WRITTEN provenance_id={} resources={} bindings={} "
                 "vertex_bindings={} indexed={} bytes={} geometry_ready={} diagnostic_ready={}",
                 package.provenance_id, package.resources.size(), package.bindings.size(),
                 package.vertex_bindings.size(), package.has_index_binding, blob_offset,
                 package.readiness.geometry_ready,
                 package.readiness.diagnostic_geometry_replay_ready);
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker, "Draw package write threw: {}", e.what());
        return false;
    } catch (...) {
        LOG_WARNING(Core_Linker, "Draw package write threw an unknown exception");
        return false;
    }
}

bool ReadPackage(const std::string& directory, Package& out, std::string& error) noexcept {
    try {
        const auto dir = std::filesystem::path{directory};
        const auto json_path = dir / "package.json";
        const auto blob_path = dir / "resources.bin";

        std::ifstream in{json_path};
        if (!in) {
            error = "cannot open package.json";
            return false;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();

        // A deliberately small reader: the schema is ours, so a full JSON parser would be more
        // machinery than the format needs. It extracts the fields the validator checks.
        const auto find_number = [&text](std::string_view key, u64& value) {
            const auto pos = text.find(std::string{"\""} + std::string{key} + "\":");
            if (pos == std::string::npos)
                return false;
            auto start = pos + key.size() + 3;
            while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
                ++start;
            u64 parsed = 0;
            bool any = false;
            while (start < text.size() && text[start] >= '0' && text[start] <= '9') {
                parsed = parsed * 10 + static_cast<u64>(text[start] - '0');
                ++start;
                any = true;
            }
            if (!any)
                return false;
            value = parsed;
            return true;
        };

        u64 schema = 0;
        if (!find_number("schema_version", schema)) {
            error = "package has no schema_version";
            return false;
        }
        out.schema_version = static_cast<u32>(schema);

        u64 provenance = 0;
        if (!find_number("provenance_id", provenance)) {
            error = "package has no provenance_id";
            return false;
        }
        out.provenance_id = provenance;

        // Load the blob, then attach bytes to each resource by its recorded offset and length.
        std::ifstream blob{blob_path, std::ios::binary};
        std::vector<u8> blob_bytes;
        if (blob) {
            blob_bytes.assign(std::istreambuf_iterator<char>{blob},
                              std::istreambuf_iterator<char>{});
        }

        out.resources.clear();
        out.bindings.clear();

        // Walk every resource object in order.
        size_t cursor = 0;
        while (true) {
            const auto id_pos = text.find("\"id\":", cursor);
            if (id_pos == std::string::npos)
                break;
            Resource resource{};
            const auto read_after = [&text](size_t pos, std::string_view key, u64& value) {
                const auto key_pos = text.find(std::string{"\""} + std::string{key} + "\":", pos);
                if (key_pos == std::string::npos)
                    return false;
                auto start = key_pos + key.size() + 3;
                while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
                    ++start;
                u64 parsed = 0;
                bool any = false;
                while (start < text.size() && text[start] >= '0' && text[start] <= '9') {
                    parsed = parsed * 10 + static_cast<u64>(text[start] - '0');
                    ++start;
                    any = true;
                }
                if (!any)
                    return false;
                value = parsed;
                return true;
            };
            u64 id = 0;
            if (!read_after(id_pos, "id", id))
                break;
            resource.id = static_cast<u32>(id);
            u64 value = 0;
            if (read_after(id_pos, "requested_base", value))
                resource.requested_base = value;
            if (read_after(id_pos, "requested_size", value))
                resource.requested_size = value;
            if (read_after(id_pos, "clamped_base", value))
                resource.clamped_base = value;
            if (read_after(id_pos, "clamped_size", value))
                resource.clamped_size = value;
            if (read_after(id_pos, "content_hash", value))
                resource.content_hash = value;
            u64 length = 0;
            if (read_after(id_pos, "byte_length", value))
                length = value;
            u64 offset = 0;
            if (read_after(id_pos, "blob_offset", value))
                offset = value;
            if (length != 0 && offset + length <= blob_bytes.size()) {
                resource.bytes.assign(blob_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      blob_bytes.begin() +
                                          static_cast<std::ptrdiff_t>(offset + length));
            }
            // Status is a string; map it back.
            const auto status_pos = text.find("\"status\":", id_pos);
            if (status_pos != std::string::npos) {
                const auto quote = text.find('"', status_pos + 9);
                const auto end_quote = quote == std::string::npos ? std::string::npos
                                                                  : text.find('"', quote + 1);
                if (quote != std::string::npos && end_quote != std::string::npos) {
                    const auto name = text.substr(quote + 1, end_quote - quote - 1);
                    resource.status = name == "host_content_verified"   ? 1
                                      : name == "host_content_captured" ? 2
                                      : name == "metadata_only"         ? 3
                                      : name == "partial"               ? 4
                                      : name == "unsupported"           ? 5
                                      : name == "invalid"               ? 6
                                                                        : 0;
                }
            }
            out.resources.push_back(std::move(resource));
            cursor = id_pos + 4;
        }

        error.clear();
        return true;
    } catch (...) {
        error = "read threw";
        return false;
    }
}

} // namespace Core::DrawPackage
