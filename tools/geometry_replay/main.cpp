// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone diagnostic geometry replay for a Deracine draw package.
//
// This program is deliberately independent of shadPS4: it links only the Vulkan loader and the C++
// standard library, reads a package directory, and renders the captured geometry offscreen with a
// diagnostic shader. It does not use the emulator's renderer, scheduler, buffer cache, texture
// cache, GPU thread, or guest address space.
//
// What it proves: the captured vertex and index bytes, the effective vertex-input layout, and the
// draw arguments are sufficient to reproduce the geometry outside the PS4 command stream.
//
// What it does not prove: material correctness. There are no textures, no original shaders, and no
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
#include <sstream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

// Local aliases rather than the emulator's common/types.h: this program must not depend on
// shadPS4 headers.
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWidth = 512;
constexpr u32 kHeight = 512;

struct Resource {
    u32 id{};
    u64 clamped_size{};
    u64 content_hash{};
    u64 blob_offset{};
    u64 byte_length{};
    std::string status;
    std::vector<u8> bytes;
};

struct VertexBinding {
    u32 binding{};
    u32 stride{};
    u32 input_rate{};
    u32 divisor{};
    u32 resource_ref{};
    u64 guest_size{};
};

struct IndexBinding {
    u32 index_size{};
    u32 index_count{};
    u32 first_index{};
    u32 base_vertex{};
    u32 instance_count{};
    u32 first_instance{};
    u32 primitive_type{};
    u32 resource_ref{};
};

struct Package {
    u32 schema_version{};
    u64 provenance_id{};
    u32 num_indices{};
    u32 num_instances{};
    bool is_indexed{};
    u32 primitive_type{};
    std::vector<Resource> resources;
    std::vector<VertexBinding> vertex_bindings;
    bool has_index{};
    IndexBinding index{};
    bool geometry_ready{};
    bool diagnostic_ready{};
};

u64 Fnv1a(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// A deliberately small reader for the package's own JSON. The schema is ours, so a full parser
/// would be more machinery than the format needs.
bool ReadPackage(const std::string& directory, Package& out, std::string& error) {
    const auto json_path = std::filesystem::path{directory} / "package.json";
    const auto blob_path = std::filesystem::path{directory} / "resources.bin";

    std::ifstream json_in{json_path};
    if (!json_in) {
        error = "cannot open package.json";
        return false;
    }
    std::stringstream buffer;
    buffer << json_in.rdbuf();
    const std::string text = buffer.str();

    std::ifstream blob_in{blob_path, std::ios::binary};
    std::vector<u8> blob;
    if (blob_in) {
        blob.assign(std::istreambuf_iterator<char>{blob_in}, std::istreambuf_iterator<char>{});
    }

    const auto read_u64 = [&text](std::string_view key, size_t from, u64& value) -> bool {
        const auto key_pos = text.find(std::string{"\""} + std::string{key} + "\":", from);
        if (key_pos == std::string::npos)
            return false;
        size_t start = key_pos + key.size() + 3;
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

    u64 value = 0;
    if (!read_u64("schema_version", 0, value)) {
        error = "no schema_version";
        return false;
    }
    out.schema_version = static_cast<u32>(value);
    if (out.schema_version != 2) {
        error = "unsupported schema version";
        return false;
    }
    if (!read_u64("provenance_id", 0, value)) {
        error = "no provenance_id";
        return false;
    }
    out.provenance_id = value;

    // Pipeline block.
    const auto pipeline_pos = text.find("\"pipeline\":");
    if (pipeline_pos != std::string::npos) {
        if (read_u64("num_indices", pipeline_pos, value))
            out.num_indices = static_cast<u32>(value);
        if (read_u64("num_instances", pipeline_pos, value))
            out.num_instances = static_cast<u32>(value);
        if (read_u64("primitive_type", pipeline_pos, value))
            out.primitive_type = static_cast<u32>(value);
        out.is_indexed = text.find("\"is_indexed\": true", pipeline_pos) != std::string::npos;
    }

    // Resources.
    size_t cursor = 0;
    while (true) {
        const auto id_pos = text.find("\"id\":", cursor);
        if (id_pos == std::string::npos)
            break;
        Resource resource{};
        if (!read_u64("id", id_pos, value))
            break;
        resource.id = static_cast<u32>(value);
        if (read_u64("clamped_size", id_pos, value))
            resource.clamped_size = value;
        if (read_u64("content_hash", id_pos, value))
            resource.content_hash = value;
        if (read_u64("blob_offset", id_pos, value))
            resource.blob_offset = value;
        if (read_u64("byte_length", id_pos, value))
            resource.byte_length = value;
        const auto status_pos = text.find("\"status\":", id_pos);
        if (status_pos != std::string::npos) {
            const auto quote = text.find('"', status_pos + 9);
            const auto end_quote = quote == std::string::npos ? std::string::npos
                                                              : text.find('"', quote + 1);
            if (quote != std::string::npos && end_quote != std::string::npos) {
                resource.status = text.substr(quote + 1, end_quote - quote - 1);
            }
        }
        if (resource.byte_length != 0 &&
            resource.blob_offset + resource.byte_length <= blob.size()) {
            resource.bytes.assign(
                blob.begin() + static_cast<std::ptrdiff_t>(resource.blob_offset),
                blob.begin() +
                    static_cast<std::ptrdiff_t>(resource.blob_offset + resource.byte_length));
        }
        out.resources.push_back(std::move(resource));
        cursor = id_pos + 4;
    }

    // Vertex bindings.
    const auto vb_pos = text.find("\"vertex_bindings\":");
    if (vb_pos != std::string::npos) {
        size_t pos = vb_pos;
        while (true) {
            const auto b_pos = text.find("\"binding\":", pos);
            if (b_pos == std::string::npos)
                break;
            const auto next_vb = text.find("\"vertex_bindings\":", b_pos);
            if (next_vb != std::string::npos && next_vb != vb_pos)
                break;
            VertexBinding vertex{};
            if (!read_u64("binding", b_pos, value))
                break;
            vertex.binding = static_cast<u32>(value);
            if (read_u64("stride", b_pos, value))
                vertex.stride = static_cast<u32>(value);
            if (read_u64("input_rate", b_pos, value))
                vertex.input_rate = static_cast<u32>(value);
            if (read_u64("divisor", b_pos, value))
                vertex.divisor = static_cast<u32>(value);
            if (read_u64("resource_ref", b_pos, value))
                vertex.resource_ref = static_cast<u32>(value);
            if (read_u64("guest_size", b_pos, value))
                vertex.guest_size = value;
            out.vertex_bindings.push_back(vertex);
            pos = b_pos + 8;
            // Stop once the index block begins.
            const auto idx_pos = text.find("\"index_binding\":", pos);
            if (idx_pos != std::string::npos && idx_pos < text.find("\"binding\":", pos))
                break;
        }
    }

    // Index binding.
    const auto ib_pos = text.find("\"index_binding\":");
    if (ib_pos != std::string::npos) {
        out.has_index = true;
        if (read_u64("index_size", ib_pos, value))
            out.index.index_size = static_cast<u32>(value);
        if (read_u64("index_count", ib_pos, value))
            out.index.index_count = static_cast<u32>(value);
        if (read_u64("first_index", ib_pos, value))
            out.index.first_index = static_cast<u32>(value);
        if (read_u64("base_vertex", ib_pos, value))
            out.index.base_vertex = static_cast<u32>(value);
        if (read_u64("instance_count", ib_pos, value))
            out.index.instance_count = static_cast<u32>(value);
        if (read_u64("first_instance", ib_pos, value))
            out.index.first_instance = static_cast<u32>(value);
        if (read_u64("primitive_type", ib_pos, value))
            out.index.primitive_type = static_cast<u32>(value);
        if (read_u64("resource_ref", ib_pos, value))
            out.index.resource_ref = static_cast<u32>(value);
    }

    // Readiness.
    const auto ready_pos = text.find("\"readiness\":");
    if (ready_pos != std::string::npos) {
        out.geometry_ready =
            text.find("\"geometry_ready\": true", ready_pos) != std::string::npos;
        out.diagnostic_ready =
            text.find("\"diagnostic_geometry_replay_ready\": true", ready_pos) !=
            std::string::npos;
    }

    error.clear();
    return true;
}

/// Re-derives every content hash, so the replay refuses a package the exporter got wrong.
bool ValidatePackage(const Package& package, std::string& error) {
    if (package.provenance_id == 0) {
        error = "no provenance id";
        return false;
    }
    std::map<u32, const Resource*> by_id;
    for (const auto& resource : package.resources) {
        if (resource.id == 0 || by_id.contains(resource.id)) {
            error = "bad resource id";
            return false;
        }
        by_id[resource.id] = &resource;
    }
    for (const auto& resource : package.resources) {
        if (resource.byte_length == 0)
            continue;
        if (resource.bytes.size() != resource.clamped_size) {
            error = "resource length mismatch";
            return false;
        }
        if (Fnv1a(resource.bytes.data(), resource.bytes.size()) != resource.content_hash) {
            error = "resource hash mismatch";
            return false;
        }
    }
    if (!package.geometry_ready || package.vertex_bindings.empty()) {
        error = "package is not geometry-ready";
        return false;
    }
    for (const auto& vertex : package.vertex_bindings) {
        if (!by_id.contains(vertex.resource_ref)) {
            error = "vertex binding references a missing resource";
            return false;
        }
    }
    if (package.is_indexed) {
        if (!package.has_index || !by_id.contains(package.index.resource_ref)) {
            error = "indexed draw has no resolvable index binding";
            return false;
        }
    }
    error.clear();
    return true;
}

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
                  VkMemoryPropertyFlags properties, VkBuffer& buffer,
                  VkDeviceMemory& memory) {
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

/// Diagnostic shaders, compiled to SPIR-V by glslc and embedded so the replay needs no shader
/// compiler at run time. They are not the game's shaders and make no attempt to be: the vertex
/// shader reads the first two components of the captured vertex and fits them to the viewport.
///
/// Source is kept beside this file in shaders/diag.vert and shaders/diag.frag.
const u32 kVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00000021,
    0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70,
    0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f,
    0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x6f635f76, 0x00726f6c, 0x00040005, 0x0000000c, 0x705f6e69,
    0x0000736f, 0x00060005, 0x0000001f, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x0000001f, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001f, 0x00000001,
    0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000001f, 0x00000002, 0x435f6c67,
    0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000001f, 0x00000003, 0x435f6c67, 0x446c6c75,
    0x61747369, 0x0065636e, 0x00030005, 0x00000021, 0x00000000, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00040047, 0x0000000c, 0x0000001e, 0x00000000, 0x00030047, 0x0000001f, 0x00000002,
    0x00050048, 0x0000001f, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001f, 0x00000001,
    0x0000000b, 0x00000001, 0x00050048, 0x0000001f, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x0000001f, 0x00000003, 0x0000000b, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000003,
    0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x00040017, 0x0000000a, 0x00000006, 0x00000002, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000e, 0x3f000000,
    0x0004002b, 0x00000006, 0x00000012, 0x00000000, 0x0004002b, 0x00000006, 0x00000013, 0x3f800000,
    0x0004002b, 0x00000006, 0x00000017, 0x3f19999a, 0x00040017, 0x0000001b, 0x00000006, 0x00000004,
    0x00040015, 0x0000001c, 0x00000020, 0x00000000, 0x0004002b, 0x0000001c, 0x0000001d, 0x00000001,
    0x0004001c, 0x0000001e, 0x00000006, 0x0000001d, 0x0006001e, 0x0000001f, 0x0000001b, 0x00000006,
    0x0000001e, 0x0000001e, 0x00040020, 0x00000020, 0x00000003, 0x0000001f, 0x0004003b, 0x00000020,
    0x00000021, 0x00000003, 0x00040015, 0x00000022, 0x00000020, 0x00000001, 0x0004002b, 0x00000022,
    0x00000023, 0x00000000, 0x00040020, 0x00000028, 0x00000003, 0x0000001b, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d,
    0x0000000c, 0x0005008e, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x00050050, 0x0000000a,
    0x00000010, 0x0000000e, 0x0000000e, 0x00050081, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010,
    0x00050050, 0x0000000a, 0x00000014, 0x00000012, 0x00000012, 0x00050050, 0x0000000a, 0x00000015,
    0x00000013, 0x00000013, 0x0008000c, 0x0000000a, 0x00000016, 0x00000001, 0x0000002b, 0x00000011,
    0x00000014, 0x00000015, 0x00050051, 0x00000006, 0x00000018, 0x00000016, 0x00000000, 0x00050051,
    0x00000006, 0x00000019, 0x00000016, 0x00000001, 0x00060050, 0x00000007, 0x0000001a, 0x00000018,
    0x00000019, 0x00000017, 0x0003003e, 0x00000009, 0x0000001a, 0x0004003d, 0x0000000a, 0x00000024,
    0x0000000c, 0x00050051, 0x00000006, 0x00000025, 0x00000024, 0x00000000, 0x00050051, 0x00000006,
    0x00000026, 0x00000024, 0x00000001, 0x00070050, 0x0000001b, 0x00000027, 0x00000025, 0x00000026,
    0x00000012, 0x00000013, 0x00050041, 0x00000028, 0x00000029, 0x00000021, 0x00000023, 0x0003003e,
    0x00000029, 0x00000027, 0x000100fd, 0x00010038,
};

const u32 kFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000013, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000c, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f,
    0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004,
    0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x5f74756f, 0x6f6c6f63, 0x00000072,
    0x00040005, 0x0000000c, 0x6f635f76, 0x00726f6c, 0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040047, 0x0000000c, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003,
    0x00040017, 0x0000000a, 0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a,
    0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000e, 0x3f800000,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x0000000a, 0x0000000d, 0x0000000c, 0x00050051, 0x00000006, 0x0000000f, 0x0000000d, 0x00000000,
    0x00050051, 0x00000006, 0x00000010, 0x0000000d, 0x00000001, 0x00050051, 0x00000006, 0x00000011,
    0x0000000d, 0x00000002, 0x00070050, 0x00000007, 0x00000012, 0x0000000f, 0x00000010, 0x00000011,
    0x0000000e, 0x0003003e, 0x00000009, 0x00000012, 0x000100fd, 0x00010038,
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: deracine_geometry_replay <package-directory>\n";
        return 2;
    }
    const std::string directory = argv[1];

    Package package{};
    std::string error;
    if (!ReadPackage(directory, package, error)) {
        std::cerr << "package read failed: " << error << "\n";
        return 1;
    }
    if (!ValidatePackage(package, error)) {
        std::cerr << "package validation failed: " << error << "\n";
        return 1;
    }

    std::cout << "package: provenance=" << package.provenance_id
              << " resources=" << package.resources.size()
              << " vertex_bindings=" << package.vertex_bindings.size()
              << " indexed=" << (package.is_indexed ? "yes" : "no") << "\n";

    // Locate the vertex and index resources.
    const auto find_resource = [&package](u32 id) -> const Resource* {
        for (const auto& resource : package.resources) {
            if (resource.id == id)
                return &resource;
        }
        return nullptr;
    };
    const Resource* vertex_resource = find_resource(package.vertex_bindings.front().resource_ref);
    const Resource* index_resource =
        package.is_indexed ? find_resource(package.index.resource_ref) : nullptr;
    if (vertex_resource == nullptr || (package.is_indexed && index_resource == nullptr)) {
        std::cerr << "geometry resources missing\n";
        return 1;
    }

    // Decode the indices and compute the bounds the diagnostic transform needs.
    std::vector<u32> indices;
    if (package.is_indexed) {
        const u32 width = package.index.index_size;
        const u32 count = package.index.index_count;
        if (width != 2 && width != 4) {
            std::cerr << "unsupported index width\n";
            return 1;
        }
        if (count * width > index_resource->bytes.size()) {
            std::cerr << "index count exceeds captured bytes\n";
            return 1;
        }
        indices.resize(count);
        for (u32 i = 0; i < count; ++i) {
            if (width == 2) {
                u16 v = 0;
                std::memcpy(&v, index_resource->bytes.data() + i * 2, sizeof(v));
                indices[i] = v;
            } else {
                u32 v = 0;
                std::memcpy(&v, index_resource->bytes.data() + i * 4, sizeof(v));
                indices[i] = v;
            }
        }
    }

    const u32 stride = package.vertex_bindings.front().stride;
    if (stride == 0) {
        std::cerr << "zero-stride vertex binding is not supported by this diagnostic replay\n";
        return 1;
    }
    const u32 vertex_count = static_cast<u32>(vertex_resource->bytes.size() / stride);

    // The diagnostic transform. The captured vertex layout is not the game's, and the game's
    // transform is not available, so this picks the two components with the largest extent and
    // fits them to the viewport. That is an explicitly documented choice, not a claim about which
    // component is object-space position.
    const u32 components = stride / sizeof(float);
    std::array<float, 4> lo{1e30f, 1e30f, 1e30f, 1e30f};
    std::array<float, 4> hi{-1e30f, -1e30f, -1e30f, -1e30f};
    for (u32 v = 0; v < vertex_count; ++v) {
        for (u32 c = 0; c < components && c < 4; ++c) {
            float value = 0.0f;
            std::memcpy(&value, vertex_resource->bytes.data() + v * stride + c * 4, sizeof(value));
            if (!std::isfinite(value))
                continue;
            lo[c] = std::min(lo[c], value);
            hi[c] = std::max(hi[c], value);
        }
    }
    // Rank the components by extent and take the two widest.
    std::array<u32, 4> order{0, 1, 2, 3};
    std::sort(order.begin(), order.begin() + std::min<u32>(components, 4),
              [&](u32 a, u32 b) { return (hi[a] - lo[a]) > (hi[b] - lo[b]); });
    const u32 axis_u = order[0];
    const u32 axis_v = order[1];
    const float span_u = std::max(hi[axis_u] - lo[axis_u], 1e-6f);
    const float span_v = std::max(hi[axis_v] - lo[axis_v], 1e-6f);
    const float scale = 2.0f / std::max(span_u, span_v);

    std::vector<float> positions(static_cast<size_t>(vertex_count) * 2);
    for (u32 v = 0; v < vertex_count; ++v) {
        float u = 0.0f, w = 0.0f;
        std::memcpy(&u, vertex_resource->bytes.data() + v * stride + axis_u * 4, sizeof(u));
        std::memcpy(&w, vertex_resource->bytes.data() + v * stride + axis_v * 4, sizeof(w));
        positions[v * 2 + 0] = (u - (lo[axis_u] + hi[axis_u]) * 0.5f) * scale;
        positions[v * 2 + 1] = (w - (lo[axis_v] + hi[axis_v]) * 0.5f) * scale;
    }

    std::cout << "geometry: vertices=" << vertex_count << " indices=" << indices.size()
              << " stride=" << stride << " components=" << components << "\n";
    std::cout << "geometry: extents";
    for (u32 c = 0; c < components && c < 4; ++c) {
        std::cout << " c" << c << "=[" << lo[c] << "," << hi[c] << "]";
    }
    std::cout << "\n";
    std::cout << "geometry: diagnostic axes u=c" << axis_u << " v=c" << axis_v << "\n";

    // --- Vulkan setup -------------------------------------------------------------------------
    ReplayContext ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_geometry_replay";
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
    const VkDeviceSize vertex_bytes = positions.size() * sizeof(float);
    if (!CreateBuffer(ctx, vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vertex_buffer, vertex_memory)) {
        std::cerr << "vertex buffer creation failed\n";
        return 1;
    }
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(ctx.device, vertex_memory, 0, vertex_bytes, 0, &mapped));
    std::memcpy(mapped, positions.data(), static_cast<size_t>(vertex_bytes));
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

    std::cout << "replay: uploaded " << vertex_bytes << " vertex bytes and " << index_bytes
              << " index bytes\n";
    std::cout << "replay: recorded drawIndexed(" << indices.size() << ", 1, 0, 0, 0)\n";

    // --- Offscreen render target ---------------------------------------------------------------
    // A real render pass is required: a draw with no framebuffer is not a valid submission, and
    // the driver reports a device loss rather than silently doing nothing.
    VkImage color_image{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {kWidth, kHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(ctx.device, &image_info, nullptr, &color_image));

    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(ctx.device, color_image, &image_requirements);
    VkMemoryAllocateInfo image_alloc{};
    image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc.allocationSize = image_requirements.size;
    image_alloc.memoryTypeIndex = FindMemoryType(ctx, image_requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(ctx.device, &image_alloc, nullptr, &color_memory));
    VK_CHECK(vkBindImageMemory(ctx.device, color_image, color_memory, 0));

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = color_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &view_info, nullptr, &color_view));

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(ctx.device, &pass_info, nullptr, &render_pass));

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &color_view;
    fb_info.width = kWidth;
    fb_info.height = kHeight;
    fb_info.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &fb_info, nullptr, &framebuffer));

    // --- Graphics pipeline ---------------------------------------------------------------------
    // A draw with no bound pipeline is invalid, and the driver reports a device loss rather than
    // silently doing nothing, so the pipeline is required for the submission to be real.
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
    VkShaderModule frag_module = make_module(kFragSpv, sizeof(kFragSpv) / sizeof(u32));
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

    // The captured layout is one binding with the recorded stride, and a two-component float
    // attribute at offset zero. That is the diagnostic interpretation, not the game's.
    VkVertexInputBindingDescription vertex_binding_desc{};
    vertex_binding_desc.binding = 0;
    vertex_binding_desc.stride = 8;
    vertex_binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_attribute{};
    vertex_attribute.location = 0;
    vertex_attribute.binding = 0;
    vertex_attribute.format = VK_FORMAT_R32G32_SFLOAT;
    vertex_attribute.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding_desc;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions = &vertex_attribute;

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

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout pipeline_layout{};
    VK_CHECK(vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline pipeline{};
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &pipeline));

    // --- Record and submit the draw ------------------------------------------------------------
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = ctx.command_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &alloc_info, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    VkClearValue clear{};
    clear.color = {{0.05f, 0.05f, 0.08f, 1.0f}};
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea = {{0, 0}, {kWidth, kHeight}};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
    if (index_bytes > 0) {
        vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, static_cast<u32>(indices.size()), 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    // --- Read the render target back and write it as a PPM -------------------------------------
    // A PPM needs no image library, so the replay keeps its single dependency.
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    VkBuffer readback_buffer{};
    VkDeviceMemory readback_memory{};
    bool wrote_image = false;
    if (CreateBuffer(ctx, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     readback_buffer, readback_memory)) {
        VkCommandBuffer copy_cmd{};
        VkCommandBufferAllocateInfo copy_alloc{};
        copy_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        copy_alloc.commandPool = ctx.command_pool;
        copy_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        copy_alloc.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(ctx.device, &copy_alloc, &copy_cmd) == VK_SUCCESS) {
            VkCommandBufferBeginInfo copy_begin{};
            copy_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(copy_cmd, &copy_begin);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kWidth, kHeight, 1};
            vkCmdCopyImageToBuffer(copy_cmd, color_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buffer, 1,
                                   &region);
            vkEndCommandBuffer(copy_cmd);

            VkSubmitInfo copy_submit{};
            copy_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            copy_submit.commandBufferCount = 1;
            copy_submit.pCommandBuffers = &copy_cmd;
            if (vkQueueSubmit(ctx.queue, 1, &copy_submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                vkQueueWaitIdle(ctx.queue) == VK_SUCCESS) {
                void* pixels = nullptr;
                if (vkMapMemory(ctx.device, readback_memory, 0, image_bytes, 0, &pixels) ==
                    VK_SUCCESS) {
                    const auto out_path = std::filesystem::path{directory} / "replay.ppm";
                    std::ofstream ppm{out_path, std::ios::binary};
                    if (ppm) {
                        ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
                        const auto* src = static_cast<const u8*>(pixels);
                        for (u32 i = 0; i < kWidth * kHeight; ++i) {
                            ppm.put(static_cast<char>(src[i * 4 + 0]));
                            ppm.put(static_cast<char>(src[i * 4 + 1]));
                            ppm.put(static_cast<char>(src[i * 4 + 2]));
                        }
                        wrote_image = true;
                    }
                    vkUnmapMemory(ctx.device, readback_memory);
                }
            }
            vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &copy_cmd);
        }
        vkDestroyBuffer(ctx.device, readback_buffer, nullptr);
        vkFreeMemory(ctx.device, readback_memory, nullptr);
    }

    std::cout << "replay: drawIndexed(" << indices.size() << ", 1, 0, 0, 0) submitted and completed\n";
    std::cout << "replay: primitives=" << (indices.size() / 3) << "\n";
    std::cout << "replay: render target " << kWidth << "x" << kHeight << " R8G8B8A8_UNORM\n";
    if (wrote_image) {
        std::cout << "replay: wrote " << (std::filesystem::path{directory} / "replay.ppm").string()
                  << "\n";
    } else {
        std::cout << "replay: image readback failed\n";
    }
    std::cout << "note: diagnostic transform only; no textures, no original shaders\n";

    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
    vkDestroyShaderModule(ctx.device, vert_module, nullptr);
    vkDestroyShaderModule(ctx.device, frag_module, nullptr);
    vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
    vkDestroyRenderPass(ctx.device, render_pass, nullptr);
    vkDestroyImageView(ctx.device, color_view, nullptr);
    vkDestroyImage(ctx.device, color_image, nullptr);
    vkFreeMemory(ctx.device, color_memory, nullptr);
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    if (index_buffer)
        vkDestroyBuffer(ctx.device, index_buffer, nullptr);
    if (index_memory)
        vkFreeMemory(ctx.device, index_memory, nullptr);
    vkDestroyBuffer(ctx.device, vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, vertex_memory, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return 0;
}
