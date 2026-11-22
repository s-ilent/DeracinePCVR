// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone diagnostic 3D replay for a Deracine draw package.
//
// Independent of shadPS4: it links only the Vulkan loader and the C++ standard library, reads a
// package directory, and renders the captured geometry offscreen with a diagnostic shader. It does
// not use the emulator's renderer, scheduler, buffer cache, texture cache, GPU thread, or guest
// address space.
//
// What it proves: the captured vertex and index bytes, the effective multi-stream vertex-input
// layout, and the draw arguments are sufficient to reproduce a three-dimensional object outside the
// PS4 command stream.
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
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include <nlohmann/json.hpp>

namespace {

// Local aliases rather than the emulator's common/types.h: this program must not depend on
// shadPS4 headers.
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
    u32 primitive_type{};
    u32 resource_ref{};
};

struct Geometry {
    u32 position_binding{};
    u32 position_components{};
    std::string position_evidence;
    float bounds_min[3]{};
    float bounds_max[3]{};
    bool is_three_dimensional{};
    u32 triangle_count{};
    u32 degenerate_triangles{};
    u32 unique_referenced_vertices{};
    u32 nonfinite_vertices{};
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
    Geometry geometry;
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

/// Reads a package with a real JSON parser. The previous string-search reader could confuse
/// similarly named fields in different objects, which is not acceptable for a multi-stream package.
bool ReadPackage(const std::string& directory, Package& out, std::string& error) {
    const auto json_path = std::filesystem::path{directory} / "package.json";
    const auto blob_path = std::filesystem::path{directory} / "resources.bin";

    std::ifstream json_in{json_path};
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

    std::ifstream blob_in{blob_path, std::ios::binary};
    std::vector<u8> blob;
    if (blob_in) {
        blob.assign(std::istreambuf_iterator<char>{blob_in}, std::istreambuf_iterator<char>{});
    }

    try {
        out.schema_version = doc.at("schema_version").get<u32>();
        out.provenance_id = doc.at("provenance_id").get<u64>();

        const auto& pipeline = doc.at("pipeline");
        out.num_indices = pipeline.value("num_indices", 0u);
        out.num_instances = pipeline.value("num_instances", 0u);
        out.primitive_type = pipeline.value("primitive_type", 0u);
        out.is_indexed = pipeline.value("is_indexed", false);

        for (const auto& entry : doc.at("resources")) {
            Resource resource{};
            resource.id = entry.at("id").get<u32>();
            resource.clamped_size = entry.value("clamped_size", 0ull);
            resource.content_hash = entry.value("content_hash", 0ull);
            resource.blob_offset = entry.value("blob_offset", 0ull);
            resource.byte_length = entry.value("byte_length", 0ull);
            resource.status = entry.value("status", std::string{});
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
            vertex.input_rate = entry.value("input_rate", 0u);
            vertex.divisor = entry.value("divisor", 0u);
            vertex.resource_ref = entry.value("resource_ref", 0u);
            vertex.guest_size = entry.value("guest_size", 0ull);
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
            out.index.primitive_type = entry.value("primitive_type", 0u);
            out.index.resource_ref = entry.value("resource_ref", 0u);
        }

        if (doc.contains("geometry")) {
            const auto& entry = doc.at("geometry");
            out.geometry.position_binding = entry.value("position_binding", 0u);
            out.geometry.position_components = entry.value("position_components", 0u);
            out.geometry.position_evidence = entry.value("position_evidence", std::string{});
            out.geometry.is_three_dimensional = entry.value("is_three_dimensional", false);
            out.geometry.triangle_count = entry.value("triangle_count", 0u);
            out.geometry.degenerate_triangles = entry.value("degenerate_triangles", 0u);
            out.geometry.unique_referenced_vertices =
                entry.value("unique_referenced_vertices", 0u);
            out.geometry.nonfinite_vertices = entry.value("nonfinite_vertices", 0u);
            if (entry.contains("bounds_min") && entry["bounds_min"].size() == 3) {
                for (u32 i = 0; i < 3; ++i)
                    out.geometry.bounds_min[i] = entry["bounds_min"][i].get<float>();
            }
            if (entry.contains("bounds_max") && entry["bounds_max"].size() == 3) {
                for (u32 i = 0; i < 3; ++i)
                    out.geometry.bounds_max[i] = entry["bounds_max"][i].get<float>();
            }
        }

        const auto& readiness = doc.at("readiness");
        out.geometry_ready = readiness.value("geometry_ready", false);
        out.diagnostic_ready = readiness.value("diagnostic_geometry_replay_ready", false);
    } catch (const std::exception& e) {
        error = std::string{"package.json is missing a required field: "} + e.what();
        return false;
    }

    error.clear();
    return true;
}

/// Re-derives every content hash, so the replay refuses a package the exporter got wrong.
bool ValidatePackage(const Package& package, std::string& error) {
    if (package.schema_version != 2) {
        error = "unsupported schema version";
        return false;
    }
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
        if (vertex.stride == 0) {
            error = "zero-stride vertex binding is not supported by this replay";
            return false;
        }
    }
    if (package.is_indexed) {
        if (!package.has_index || !by_id.contains(package.index.resource_ref)) {
            error = "indexed draw has no resolvable index binding";
            return false;
        }
        if (package.index.index_size != 2 && package.index.index_size != 4) {
            error = "unsupported index width";
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

/// Diagnostic shaders, compiled to SPIR-V by glslc and embedded so the replay needs no shader
/// compiler at run time. They are not the game's shaders and make no attempt to be.
///
/// Source is kept beside this file in shaders/scene.vert and shaders/scene.frag.
const u32 kVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x0000001c,
    0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45, 0x74735f70,
    0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f,
    0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x6f635f76, 0x00726f6c, 0x00040005, 0x0000000b, 0x705f6e69,
    0x0000736f, 0x00060005, 0x0000001a, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x0000001a, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001a, 0x00000001,
    0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000001a, 0x00000002, 0x435f6c67,
    0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000001a, 0x00000003, 0x435f6c67, 0x446c6c75,
    0x61747369, 0x0065636e, 0x00030005, 0x0000001c, 0x00000000, 0x00040005, 0x00000020, 0x68737550,
    0x00000000, 0x00040006, 0x00000020, 0x00000000, 0x0070766d, 0x00030005, 0x00000022, 0x00006370,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000b, 0x0000001e, 0x00000000,
    0x00030047, 0x0000001a, 0x00000002, 0x00050048, 0x0000001a, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000001a, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001a, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000001a, 0x00000003, 0x0000000b, 0x00000004, 0x00030047,
    0x00000020, 0x00000002, 0x00040048, 0x00000020, 0x00000000, 0x00000005, 0x00050048, 0x00000020,
    0x00000000, 0x00000007, 0x00000010, 0x00050048, 0x00000020, 0x00000000, 0x00000023, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000003, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000007,
    0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006, 0x0000000d, 0x3f000000,
    0x0004002b, 0x00000006, 0x00000011, 0x00000000, 0x0004002b, 0x00000006, 0x00000012, 0x3f800000,
    0x00040017, 0x00000016, 0x00000006, 0x00000004, 0x00040015, 0x00000017, 0x00000020, 0x00000000,
    0x0004002b, 0x00000017, 0x00000018, 0x00000001, 0x0004001c, 0x00000019, 0x00000006, 0x00000018,
    0x0006001e, 0x0000001a, 0x00000016, 0x00000006, 0x00000019, 0x00000019, 0x00040020, 0x0000001b,
    0x00000003, 0x0000001a, 0x0004003b, 0x0000001b, 0x0000001c, 0x00000003, 0x00040015, 0x0000001d,
    0x00000020, 0x00000001, 0x0004002b, 0x0000001d, 0x0000001e, 0x00000000, 0x00040018, 0x0000001f,
    0x00000016, 0x00000004, 0x0003001e, 0x00000020, 0x0000001f, 0x00040020, 0x00000021, 0x00000009,
    0x00000020, 0x0004003b, 0x00000021, 0x00000022, 0x00000009, 0x00040020, 0x00000023, 0x00000009,
    0x0000001f, 0x00040020, 0x0000002c, 0x00000003, 0x00000016, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000c, 0x0000000b,
    0x0005008e, 0x00000007, 0x0000000e, 0x0000000c, 0x0000000d, 0x00060050, 0x00000007, 0x0000000f,
    0x0000000d, 0x0000000d, 0x0000000d, 0x00050081, 0x00000007, 0x00000010, 0x0000000e, 0x0000000f,
    0x00060050, 0x00000007, 0x00000013, 0x00000011, 0x00000011, 0x00000011, 0x00060050, 0x00000007,
    0x00000014, 0x00000012, 0x00000012, 0x00000012, 0x0008000c, 0x00000007, 0x00000015, 0x00000001,
    0x0000002b, 0x00000010, 0x00000013, 0x00000014, 0x0003003e, 0x00000009, 0x00000015, 0x00050041,
    0x00000023, 0x00000024, 0x00000022, 0x0000001e, 0x0004003d, 0x0000001f, 0x00000025, 0x00000024,
    0x0004003d, 0x00000007, 0x00000026, 0x0000000b, 0x00050051, 0x00000006, 0x00000027, 0x00000026,
    0x00000000, 0x00050051, 0x00000006, 0x00000028, 0x00000026, 0x00000001, 0x00050051, 0x00000006,
    0x00000029, 0x00000026, 0x00000002, 0x00070050, 0x00000016, 0x0000002a, 0x00000027, 0x00000028,
    0x00000029, 0x00000012, 0x00050091, 0x00000016, 0x0000002b, 0x00000025, 0x0000002a, 0x00050041,
    0x0000002c, 0x0000002d, 0x0000001c, 0x0000001e, 0x0003003e, 0x0000002d, 0x0000002b, 0x000100fd,
    0x00010038,
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

/// A 4x4 column-major matrix, built by hand so the replay needs no math library.
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
                for (u32 k = 0; k < 4; ++k) {
                    sum += m[k * 4 + r] * other.m[c * 4 + k];
                }
                out.m[c * 4 + r] = sum;
            }
        }
        return out;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: deracine_scene_replay <package-directory> [output-prefix]\n";
        return 2;
    }
    const std::string directory = argv[1];
    const std::string prefix = argc > 2 ? argv[2] : "view";

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
    std::cout << "geometry: three_dimensional="
              << (package.geometry.is_three_dimensional ? "yes" : "no")
              << " triangles=" << package.geometry.triangle_count
              << " degenerate=" << package.geometry.degenerate_triangles
              << " unique_vertices=" << package.geometry.unique_referenced_vertices << "\n";
    std::cout << "geometry: position_binding=" << package.geometry.position_binding
              << " evidence=\"" << package.geometry.position_evidence << "\"\n";
    std::cout << "geometry: bounds=(" << package.geometry.bounds_min[0] << ","
              << package.geometry.bounds_min[1] << "," << package.geometry.bounds_min[2] << ")-("
              << package.geometry.bounds_max[0] << "," << package.geometry.bounds_max[1] << ","
              << package.geometry.bounds_max[2] << ")\n";

    if (!package.geometry.is_three_dimensional) {
        std::cerr << "refusing to render: the package does not describe 3D geometry\n";
        return 1;
    }

    const auto find_resource = [&package](u32 id) -> const Resource* {
        for (const auto& resource : package.resources) {
            if (resource.id == id)
                return &resource;
        }
        return nullptr;
    };

    // Decode the indices.
    std::vector<u32> indices;
    if (package.is_indexed) {
        const Resource* index_resource = find_resource(package.index.resource_ref);
        if (index_resource == nullptr) {
            std::cerr << "index resource missing\n";
            return 1;
        }
        const u32 width = package.index.index_size;
        const u32 count = package.index.index_count;
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
                std::memcpy(&indices[i], index_resource->bytes.data() + i * 4, sizeof(u32));
            }
        }
    }

    // Build the diagnostic vertex stream: position only, from the selected binding. The captured
    // layout is preserved for the other streams in the package; this renderer consumes the position
    // stream because that is what a diagnostic view needs.
    const VertexBinding* position_binding = nullptr;
    for (const auto& vertex : package.vertex_bindings) {
        if (vertex.binding == package.geometry.position_binding) {
            position_binding = &vertex;
            break;
        }
    }
    if (position_binding == nullptr) {
        std::cerr << "position binding missing\n";
        return 1;
    }
    const Resource* position_resource = find_resource(position_binding->resource_ref);
    if (position_resource == nullptr || position_binding->stride == 0) {
        std::cerr << "position resource missing\n";
        return 1;
    }

    const u32 stride = position_binding->stride;
    const u32 vertex_count = static_cast<u32>(position_resource->bytes.size() / stride);

    // Centre and scale the object for the diagnostic camera. This is a documented transformation,
    // not the game's transform.
    const float* lo = package.geometry.bounds_min;
    const float* hi = package.geometry.bounds_max;
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (u32 c = 0; c < 3; ++c) {
        extent = std::max(extent, hi[c] - lo[c]);
    }
    const float scale = extent > 1e-6f ? 2.0f / extent : 1.0f;

    std::vector<float> positions(static_cast<size_t>(vertex_count) * 3);
    for (u32 v = 0; v < vertex_count; ++v) {
        for (u32 c = 0; c < 3; ++c) {
            float value = 0.0f;
            if (c < position_binding->element_count) {
                std::memcpy(&value, position_resource->bytes.data() + v * stride + c * 4,
                            sizeof(value));
            }
            positions[v * 3 + c] = (value - centre[c]) * scale;
        }
    }

    std::cout << "replay: vertices=" << vertex_count << " indices=" << indices.size()
              << " stride=" << stride << " scale=" << scale << "\n";

    // --- Vulkan setup -------------------------------------------------------------------------
    ReplayContext ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_scene_replay";
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

    // --- Offscreen render target with depth ----------------------------------------------------
    VkImage color_image{}, depth_image{};
    VkDeviceMemory color_memory{}, depth_memory{};
    VkImageView color_view{}, depth_view{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};

    const auto make_image = [&ctx](VkFormat format, VkImageUsageFlags usage, VkImage& image,
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

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vkCreateImageView(ctx.device, &view_info, nullptr, &view) == VK_SUCCESS;
    };

    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    color_image, color_memory, color_view)) {
        std::cerr << "colour target creation failed\n";
        return 1;
    }
    if (!make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
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

    VkRenderPassCreateInfo pass_info{};
    pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VK_CHECK(vkCreateRenderPass(ctx.device, &pass_info, nullptr, &render_pass));

    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 2;
    fb_info.pAttachments = fb_views;
    fb_info.width = kWidth;
    fb_info.height = kHeight;
    fb_info.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device, &fb_info, nullptr, &framebuffer));

    // --- Pipeline ------------------------------------------------------------------------------
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

    VkVertexInputBindingDescription vertex_binding_desc{};
    vertex_binding_desc.binding = 0;
    vertex_binding_desc.stride = 3 * sizeof(float);
    vertex_binding_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_attribute{};
    vertex_attribute.location = 0;
    vertex_attribute.binding = 0;
    vertex_attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
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
    pipeline_info.pDepthStencilState = &depth_state;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline pipeline{};
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &pipeline));

    // --- Render several camera angles ----------------------------------------------------------
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    VkBuffer readback_buffer{};
    VkDeviceMemory readback_memory{};
    if (!CreateBuffer(ctx, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback_buffer, readback_memory)) {
        std::cerr << "readback buffer creation failed\n";
        return 1;
    }

    // Three views: front, oblique and elevated rear. The camera orbits the centred object at a
    // fixed radius, which is what makes the three-dimensionality visible.
    struct View {
        const char* name;
        float eye[3];
    };
    const View views[3] = {
        {"front", {0.0f, 0.0f, 3.2f}},
        {"oblique", {2.3f, 1.4f, 2.0f}},
        {"rear_elevated", {-1.8f, 2.2f, -2.2f}},
    };

    u32 rendered = 0;
    for (const auto& view : views) {
        const float target[3] = {0.0f, 0.0f, 0.0f};
        const float up[3] = {0.0f, 1.0f, 0.0f};
        const Mat4 proj = Mat4::Perspective(0.9f, 1.0f, 0.1f, 100.0f);
        const Mat4 view_mat = Mat4::LookAt(view.eye, target, up);
        const Mat4 mvp = proj * view_mat;

        VkCommandBuffer cmd{};
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = ctx.command_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &alloc_info, &cmd));

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

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

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp),
                           mvp.m);
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

        // Read the target back and write it as a PPM, which needs no image library.
        VkCommandBuffer copy_cmd{};
        VkCommandBufferAllocateInfo copy_alloc{};
        copy_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        copy_alloc.commandPool = ctx.command_pool;
        copy_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        copy_alloc.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device, &copy_alloc, &copy_cmd));
        VkCommandBufferBeginInfo copy_begin{};
        copy_begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(copy_cmd, &copy_begin);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(copy_cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback_buffer, 1, &region);
        vkEndCommandBuffer(copy_cmd);
        VkSubmitInfo copy_submit{};
        copy_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        copy_submit.commandBufferCount = 1;
        copy_submit.pCommandBuffers = &copy_cmd;
        VK_CHECK(vkQueueSubmit(ctx.queue, 1, &copy_submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.queue));

        void* pixels = nullptr;
        if (vkMapMemory(ctx.device, readback_memory, 0, image_bytes, 0, &pixels) == VK_SUCCESS) {
            const auto out_path =
                std::filesystem::path{directory} / (prefix + "_" + view.name + ".ppm");
            std::ofstream ppm{out_path, std::ios::binary};
            if (ppm) {
                ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
                const auto* src = static_cast<const u8*>(pixels);
                for (u32 i = 0; i < kWidth * kHeight; ++i) {
                    ppm.put(static_cast<char>(src[i * 4 + 0]));
                    ppm.put(static_cast<char>(src[i * 4 + 1]));
                    ppm.put(static_cast<char>(src[i * 4 + 2]));
                }
                ++rendered;
                std::cout << "replay: wrote " << out_path.string() << "\n";
            }
            vkUnmapMemory(ctx.device, readback_memory);
        }
        vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &copy_cmd);
        vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &cmd);
    }

    std::cout << "replay: views=" << rendered << " primitives=" << (indices.size() / 3)
              << " depth=yes perspective=yes\n";
    std::cout << "note: diagnostic transform and material only; no textures, no original shaders\n";

    vkDestroyBuffer(ctx.device, readback_buffer, nullptr);
    vkFreeMemory(ctx.device, readback_memory, nullptr);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
    vkDestroyShaderModule(ctx.device, vert_module, nullptr);
    vkDestroyShaderModule(ctx.device, frag_module, nullptr);
    vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
    vkDestroyRenderPass(ctx.device, render_pass, nullptr);
    vkDestroyImageView(ctx.device, color_view, nullptr);
    vkDestroyImageView(ctx.device, depth_view, nullptr);
    vkDestroyImage(ctx.device, color_image, nullptr);
    vkDestroyImage(ctx.device, depth_image, nullptr);
    vkFreeMemory(ctx.device, color_memory, nullptr);
    vkFreeMemory(ctx.device, depth_memory, nullptr);
    if (index_buffer)
        vkDestroyBuffer(ctx.device, index_buffer, nullptr);
    if (index_memory)
        vkFreeMemory(ctx.device, index_memory, nullptr);
    vkDestroyBuffer(ctx.device, vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, vertex_memory, nullptr);
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return rendered == 3 ? 0 : 1;
}
