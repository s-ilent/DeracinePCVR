// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone semantic-tape replay.
//
// This program executes a UE4-origin semantic command tape through a backend-neutral interpreter
// and renders the result with Vulkan. It is the RHI-01 milestone: the draw arguments it uses come
// from the UE4 deferred command record, not from a captured PM4 packet.
//
// What it proves: a native renderer can consume recovered Unreal RHI commands.
// What it does not prove: that the running game uses this renderer. It does not. The original GNM
// path is untouched, and this program never runs alongside the game.
//
// Dependencies: the Vulkan loader, the C++ standard library, nlohmann/json. No shadPS4 DLL, no
// emulator, no guest address space.

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

#include "tape.h"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWidth = 640;
constexpr u32 kHeight = 640;

/// The resource package the tape references. Only the geometry contents are used: the tape supplies
/// the draw arguments, the package supplies the bytes. That split is deliberate and is reported.
struct PackageResource {
    u32 id{};
    u64 clamped_size{};
    u64 content_hash{};
    u64 blob_offset{};
    u64 byte_length{};
    std::string status;
    std::vector<u8> bytes;
};

struct PackageVertexBinding {
    u32 binding{};
    u32 stride{};
    u32 resource_ref{};
    u32 semantic{};
    u32 vk_format{};
    u32 attr_offset{};
    u32 element_count{};
};

struct PackageIndexBinding {
    u32 index_size{};
    u32 index_count{};
    u32 resource_ref{};
};

struct Package {
    u32 schema_version{};
    u64 provenance_id{};
    std::vector<PackageResource> resources;
    std::vector<PackageVertexBinding> vertex_bindings;
    bool has_index{};
    PackageIndexBinding index{};
    u32 position_binding{};
    u32 position_components{};
    float bounds_min[3]{};
    float bounds_max[3]{};
    bool is_three_dimensional{};
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
    const auto json_path = std::filesystem::path{directory} / "package.json";
    std::ifstream json_file{json_path};
    if (!json_file) {
        error = "cannot open " + json_path.string();
        return false;
    }
    nlohmann::json json;
    try {
        json_file >> json;
    } catch (const std::exception& e) {
        error = std::string{"package is not valid JSON: "} + e.what();
        return false;
    }

    std::ifstream blob{std::filesystem::path{directory} / "resources.bin", std::ios::binary};
    if (!blob) {
        error = "cannot open resources.bin";
        return false;
    }
    std::vector<u8> blob_bytes{std::istreambuf_iterator<char>(blob),
                               std::istreambuf_iterator<char>()};

    try {
        out = {};
        out.schema_version = json.at("schema_version").get<u32>();
        out.provenance_id = json.at("provenance_id").get<u64>();
        for (const auto& entry : json.at("resources")) {
            PackageResource resource{};
            resource.id = entry.at("id").get<u32>();
            resource.clamped_size = entry.at("clamped_size").get<u64>();
            resource.content_hash = entry.at("content_hash").get<u64>();
            resource.blob_offset = entry.at("blob_offset").get<u64>();
            resource.byte_length = entry.at("byte_length").get<u64>();
            resource.status = entry.value("status", "");
            if (resource.byte_length > 0) {
                if (resource.blob_offset + resource.byte_length > blob_bytes.size()) {
                    error = "resource " + std::to_string(resource.id) +
                            " extends past the blob";
                    return false;
                }
                resource.bytes.assign(blob_bytes.begin() + resource.blob_offset,
                                      blob_bytes.begin() + resource.blob_offset +
                                          resource.byte_length);
                // The package's own contract: the retained bytes must hash to the declared value.
                if (Fnv1a(resource.bytes.data(), resource.bytes.size()) != resource.content_hash) {
                    error = "resource " + std::to_string(resource.id) +
                            " content hash does not match its bytes";
                    return false;
                }
            }
            out.resources.push_back(std::move(resource));
        }
        for (const auto& entry : json.at("vertex_bindings")) {
            PackageVertexBinding binding{};
            binding.binding = entry.at("binding").get<u32>();
            binding.stride = entry.at("stride").get<u32>();
            binding.resource_ref = entry.at("resource_ref").get<u32>();
            binding.semantic = entry.value("semantic", 0u);
            binding.vk_format = entry.value("vk_format", 0u);
            binding.attr_offset = entry.value("attr_offset", 0u);
            binding.element_count = entry.value("element_count", 0u);
            out.vertex_bindings.push_back(binding);
        }
        if (json.contains("index_binding")) {
            const auto& index = json.at("index_binding");
            out.index.index_size = index.at("index_size").get<u32>();
            out.index.index_count = index.at("index_count").get<u32>();
            out.index.resource_ref = index.at("resource_ref").get<u32>();
            out.has_index = true;
        }
        if (json.contains("geometry")) {
            const auto& geometry = json.at("geometry");
            out.position_binding = geometry.value("position_binding", 0u);
            out.position_components = geometry.value("position_components", 0u);
            out.is_three_dimensional = geometry.value("is_three_dimensional", false);
            if (geometry.contains("bounds_min")) {
                for (u32 i = 0; i < 3; ++i)
                    out.bounds_min[i] = geometry.at("bounds_min")[i].get<float>();
            }
            if (geometry.contains("bounds_max")) {
                for (u32 i = 0; i < 3; ++i)
                    out.bounds_max[i] = geometry.at("bounds_max")[i].get<float>();
            }
        }
    } catch (const std::exception& e) {
        error = std::string{"package is missing a required field: "} + e.what();
        return false;
    }
    error.clear();
    return true;
}

const PackageResource* FindPackageResource(const Package& package, u32 id) {
    for (const auto& resource : package.resources) {
        if (resource.id == id)
            return &resource;
    }
    return nullptr;
}

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

/// Diagnostic shaders. Not the game's shaders, and the report says so.
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
    0x4470696c, 0x6174736e, 0x0065636e, 0x00070006, 0x0000001a, 0x00000003, 0x435f6c67, 0x446c6c75,
    0x6174736e, 0x0065636e, 0x00030005, 0x0000001c, 0x00000000, 0x00040005, 0x00000020, 0x68737550,
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

struct Mat4 {
    float m[16]{};

    static Mat4 Perspective(float fov_y, float aspect, float near_plane, float far_plane) {
        Mat4 out{};
        const float f = 1.0f / std::tan(fov_y * 0.5f);
        out.m[0] = f / aspect;
        out.m[5] = f;
        out.m[10] = far_plane / (near_plane - far_plane);
        out.m[11] = -1.0f;
        out.m[14] = (far_plane * near_plane) / (near_plane - far_plane);
        return out;
    }

    static Mat4 LookAt(const float eye[3], const float target[3], const float up[3]) {
        float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl > 1e-8f) {
            f[0] /= fl;
            f[1] /= fl;
            f[2] /= fl;
        }
        float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                      f[0] * up[1] - f[1] * up[0]};
        const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (sl > 1e-8f) {
            s[0] /= sl;
            s[1] /= sl;
            s[2] /= sl;
        }
        const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                            s[0] * f[1] - s[1] * f[0]};
        Mat4 out{};
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
        out.m[15] = 1.0f;
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

/// The Vulkan backend for the semantic command sink.
///
/// It records the operations the tape asks for into a command buffer. It never decides what to
/// draw: the draw arguments arrive from the tape.
class VulkanSink final : public tape::CommandSink {
public:
    VulkanSink(ReplayContext& ctx, VkCommandBuffer cmd, VkPipeline pipeline,
               VkPipelineLayout layout, VkBuffer vertex_buffer, VkBuffer index_buffer,
               u32 index_element_size, const Mat4& mvp)
        : ctx_(ctx), cmd_(cmd), pipeline_(pipeline), layout_(layout), vertex_buffer_(vertex_buffer),
          index_buffer_(index_buffer), index_element_size_(index_element_size), mvp_(mvp) {}

    void BeginFrame() override {
        // The tape carries no SetGraphicsPipeline operation: the bound-shader-state command is not
        // in the recovered command cluster. The diagnostic pipeline is therefore bound here, and
        // the caller reports it as substituted state rather than letting the draw silently produce
        // nothing.
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp_.m), mvp_.m);
        // The tape carries no BindIndexBuffer operation: the recovered command cluster has no
        // index-buffer binding command, because the PS4 RHI receives the index buffer as an argument
        // to the draw rather than as separate state. The buffer is bound here from the tape's index
        // resource, and the width comes from that resource rather than a guess. The draw arguments
        // themselves still come from the tape.
        const VkIndexType type =
            index_element_size_ == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkCmdBindIndexBuffer(cmd_, index_buffer_, 0, type);
    }

    void SetViewport(const tape::Viewport& viewport) override {
        VkViewport vk{};
        vk.x = viewport.min_x;
        vk.y = viewport.min_y;
        vk.width = viewport.max_x - viewport.min_x;
        vk.height = viewport.max_y - viewport.min_y;
        vk.minDepth = viewport.min_z;
        vk.maxDepth = viewport.max_z;
        // A degenerate viewport would make the draw invisible for a reason unrelated to the tape,
        // so it is reported rather than silently applied.
        if (vk.width <= 0.0f || vk.height <= 0.0f) {
            std::cerr << "warning: tape viewport is degenerate ("
                      << vk.width << "x" << vk.height << "); using the render target extent\n";
            vk.x = 0.0f;
            vk.y = 0.0f;
            vk.width = static_cast<float>(kWidth);
            vk.height = static_cast<float>(kHeight);
            vk.minDepth = 0.0f;
            vk.maxDepth = 1.0f;
        }
        vkCmdSetViewport(cmd_, 0, 1, &vk);
        VkRect2D scissor{{static_cast<int32_t>(vk.x), static_cast<int32_t>(vk.y)},
                         {static_cast<u32>(vk.width), static_cast<u32>(vk.height)}};
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        applied_viewport_ = vk;
    }

    void SetRenderTargets(const u32* color_targets, u32 color_target_count,
                          u32 depth_target) override {
        // The render pass and framebuffer are already bound for this offscreen target. The tape's
        // target identities are recorded so the report can compare them; they are not used to
        // select a different framebuffer, because the package does not carry target contents.
        color_target_count_ = color_target_count;
        for (u32 i = 0; i < color_target_count && i < 8; ++i)
            color_targets_[i] = color_targets[i];
        depth_target_ = depth_target;
    }

    void BindVertexBuffers(const u32* resources, u32 count) override {
        // The tape's vertex resource identities are UE4 interface pointers, not portable ids, so
        // the geometry bytes come from the package. The binding is recorded, not fabricated.
        (void)resources;
        (void)count;
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd_, 0, 1, &vertex_buffer_, &offset);
    }

    void BindIndexBuffer(u32 resource, u32 element_size) override {
        bound_index_resource_ = resource;
        bound_index_element_size_ = element_size;
        const VkIndexType type = element_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkCmdBindIndexBuffer(cmd_, index_buffer_, 0, type);
    }

    void SetGraphicsPipeline(u32 pipeline_id) override {
        (void)pipeline_id;
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp_.m), mvp_.m);
    }

    void BindShaderResources(const u32* resources, u32 count) override {
        (void)resources;
        (void)count;
    }

    void DrawIndexed(const tape::DrawPayload& payload, const tape::DerivedDraw& derived) override {
        // Every argument here comes from the tape. Nothing is read from a captured PM4 packet.
        //
        // vertexOffset is int32_t in Vulkan's signature, and BaseVertexIndex is signed in UE4. It is
        // passed through as a signed value: routing it through an unsigned intermediate would turn a
        // negative base vertex into a huge positive offset.
        vkCmdDrawIndexed(cmd_, derived.index_count, derived.instance_count, derived.first_index,
                         derived.vertex_offset, payload.first_instance);
        last_draw_ = derived;
        ++draws_;
    }

    void EndFrame() override {}

    void Opaque(u64 sequence, std::string_view note) override {
        std::cout << "tape: opaque operation sequence=" << sequence << " note=\"" << note << "\"\n";
    }

    [[nodiscard]] u32 draws() const { return draws_; }
    [[nodiscard]] const tape::DerivedDraw& last_draw() const { return last_draw_; }
    [[nodiscard]] VkViewport applied_viewport() const { return applied_viewport_; }
    [[nodiscard]] u32 bound_index_resource() const { return bound_index_resource_; }
    [[nodiscard]] u32 bound_index_element_size() const { return bound_index_element_size_; }
    [[nodiscard]] u32 color_target_count() const { return color_target_count_; }
    [[nodiscard]] u32 depth_target() const { return depth_target_; }

private:
    ReplayContext& ctx_;
    VkCommandBuffer cmd_{};
    VkPipeline pipeline_{};
    VkPipelineLayout layout_{};
    VkBuffer vertex_buffer_{};
    VkBuffer index_buffer_{};
    u32 index_element_size_{2};
    Mat4 mvp_{};
    VkViewport applied_viewport_{};
    u32 draws_{};
    tape::DerivedDraw last_draw_{};
    u32 bound_index_resource_{};
    u32 bound_index_element_size_{};
    u32 color_target_count_{};
    u32 color_targets_[8]{};
    u32 depth_target_{};
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: deracine_tape_replay <tape-and-package-directory> [output-prefix] "
                     "[--substitute-viewport] [--substitute-render-targets]\n";
        return 2;
    }
    const std::string directory = argv[1];
    std::string prefix = "tape";
    bool substitute_viewport = false;
    bool substitute_targets = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--substitute-viewport") {
            substitute_viewport = true;
        } else if (arg == "--substitute-render-targets") {
            substitute_targets = true;
        } else {
            prefix = arg;
        }
    }

    // --- Read the tape -------------------------------------------------------------------------
    tape::Tape tape{};
    std::string error;
    if (!tape::ReadTape(directory, tape, error)) {
        std::cerr << "tape read failed: " << error << "\n";
        return 1;
    }
    // A tape missing state can still be executed when the caller explicitly supplies that state.
    // The substitution is opt-in and is reported, so a partial execution is never presented as a
    // complete one.
    tape::Substitutions substitutions{};
    if (substitute_viewport) {
        substitutions.has_viewport = true;
        substitutions.viewport.min_x = 0.0f;
        substitutions.viewport.min_y = 0.0f;
        substitutions.viewport.min_z = 0.0f;
        substitutions.viewport.max_x = static_cast<float>(kWidth);
        substitutions.viewport.max_y = static_cast<float>(kHeight);
        substitutions.viewport.max_z = 1.0f;
    }
    if (substitute_targets) {
        substitutions.has_render_targets = true;
        substitutions.color_target_count = 1;
        substitutions.depth_target = 0;
    }

    const tape::Problem problem = tape::CheckExecutable(tape);
    const bool covered = (problem == tape::Problem::NoViewport && substitutions.has_viewport) ||
                         (problem == tape::Problem::NoRenderTargets &&
                          substitutions.has_render_targets);
    if (problem != tape::Problem::None && !covered) {
        std::cerr << "tape is not executable: " << tape::ProblemName(problem) << "\n";
        for (const auto& dependency : tape.unresolved)
            std::cerr << "  unresolved: " << dependency.what << " (owner " << dependency.owner
                      << "): " << dependency.reason << "\n";
        std::cerr << "  pass --substitute-viewport / --substitute-render-targets to supply the "
                     "missing state explicitly\n";
        return 1;
    }

    std::cout << "tape: provenance=" << tape.provenance_id << " schema=" << tape.schema_version
              << " operations=" << tape.operations.size()
              << " resources=" << tape.resources.size()
              << " unresolved=" << tape.unresolved.size() << "\n";
    std::cout << "tape: submission=" << tape.submission_id << " packet_offset=0x" << std::hex
              << tape.packet_offset << std::dec
              << " offset_known=" << (tape.packet_offset_known ? "yes" : "no") << "\n";

    // --- Read the resource package -------------------------------------------------------------
    Package package{};
    if (!ReadPackage(directory, package, error)) {
        std::cerr << "package read failed: " << error << "\n";
        return 1;
    }
    if (package.provenance_id != tape.provenance_id) {
        std::cerr << "package provenance " << package.provenance_id
                  << " does not match tape provenance " << tape.provenance_id << "\n";
        return 1;
    }
    std::cout << "package: provenance=" << package.provenance_id
              << " resources=" << package.resources.size()
              << " vertex_bindings=" << package.vertex_bindings.size() << "\n";

    // --- Resolve the tape's resources against the package --------------------------------------
    // The tape's resource ids are backend-neutral. The package reference is what connects them to
    // bytes, and it is recorded rather than assumed.
    u32 resolved = 0;
    u32 unresolved = 0;
    for (const auto& resource : tape.resources) {
        if (resource.package_resource_ref == 0) {
            ++unresolved;
            continue;
        }
        if (FindPackageResource(package, resource.package_resource_ref) == nullptr) {
            std::cerr << "tape resource " << resource.id << " references package resource "
                      << resource.package_resource_ref << " which is absent\n";
            return 1;
        }
        ++resolved;
    }
    std::cout << "resources: resolved_from_package=" << resolved
              << " without_package_reference=" << unresolved << "\n";

    // --- Geometry from the package -------------------------------------------------------------
    const PackageVertexBinding* position_binding = nullptr;
    for (const auto& vertex : package.vertex_bindings) {
        if (vertex.binding == package.position_binding) {
            position_binding = &vertex;
            break;
        }
    }
    if (position_binding == nullptr) {
        std::cerr << "package has no position binding\n";
        return 1;
    }
    const PackageResource* position_resource =
        FindPackageResource(package, position_binding->resource_ref);
    if (position_resource == nullptr || position_binding->stride == 0) {
        std::cerr << "package position resource is missing\n";
        return 1;
    }

    const u32 stride = position_binding->stride;
    const u32 vertex_count = static_cast<u32>(position_resource->bytes.size() / stride);

    const float* lo = package.bounds_min;
    const float* hi = package.bounds_max;
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (u32 c = 0; c < 3; ++c)
        extent = std::max(extent, hi[c] - lo[c]);
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

    // The index bytes come from the package; the count and offset come from the tape.
    const PackageResource* index_resource =
        package.has_index ? FindPackageResource(package, package.index.resource_ref) : nullptr;
    if (index_resource == nullptr) {
        std::cerr << "package has no index resource\n";
        return 1;
    }
    const u32 index_width = package.index.index_size;
    std::vector<u32> indices;
    {
        const u32 count = static_cast<u32>(index_resource->bytes.size() / index_width);
        indices.resize(count);
        for (u32 i = 0; i < count; ++i) {
            if (index_width == 2) {
                u16 v = 0;
                std::memcpy(&v, index_resource->bytes.data() + i * 2, sizeof(v));
                indices[i] = v;
            } else {
                std::memcpy(&indices[i], index_resource->bytes.data() + i * 4, sizeof(u32));
            }
        }
    }

    std::cout << "geometry: vertices=" << vertex_count << " index_elements=" << indices.size()
              << " stride=" << stride << " scale=" << scale << "\n";

    // --- Vulkan setup --------------------------------------------------------------------------
    ReplayContext ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_tape_replay";
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
    if (vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool) != VK_SUCCESS) {
        std::cerr << "vkCreateCommandPool failed\n";
        return 1;
    }

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
    vkMapMemory(ctx.device, vertex_memory, 0, vertex_bytes, 0, &mapped);
    std::memcpy(mapped, positions.data(), static_cast<size_t>(vertex_bytes));
    vkUnmapMemory(ctx.device, vertex_memory);

    // The index buffer is uploaded at the width the tape's index resource declares, so the binding
    // the sink performs and the bytes it points at agree. Uploading 32-bit elements and binding them
    // as 16-bit reads every index at half its offset, which produces a draw that runs and renders
    // nothing.
    const VkDeviceSize index_bytes = indices.size() * index_width;
    if (!CreateBuffer(ctx, index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      index_buffer, index_memory)) {
        std::cerr << "index buffer creation failed\n";
        return 1;
    }
    vkMapMemory(ctx.device, index_memory, 0, index_bytes, 0, &mapped);
    if (index_width == 2) {
        std::vector<u16> narrow(indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
            narrow[i] = static_cast<u16>(indices[i]);
        std::memcpy(mapped, narrow.data(), static_cast<size_t>(index_bytes));
    } else {
        std::memcpy(mapped, indices.data(), static_cast<size_t>(index_bytes));
    }
    vkUnmapMemory(ctx.device, index_memory);

    // --- Offscreen targets ---------------------------------------------------------------------
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
    if (vkCreateRenderPass(ctx.device, &pass_info, nullptr, &render_pass) != VK_SUCCESS) {
        std::cerr << "vkCreateRenderPass failed\n";
        return 1;
    }

    VkImageView fb_views[2] = {color_view, depth_view};
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 2;
    fb_info.pAttachments = fb_views;
    fb_info.width = kWidth;
    fb_info.height = kHeight;
    fb_info.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS) {
        std::cerr << "vkCreateFramebuffer failed\n";
        return 1;
    }

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

    // The viewport is dynamic: the tape supplies it, so the pipeline must not bake one in.
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

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
    push_range.offset = 0;
    push_range.size = sizeof(float) * 16;

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    VkPipelineLayout pipeline_layout{};
    if (vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
        std::cerr << "vkCreatePipelineLayout failed\n";
        return 1;
    }

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
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    VkPipeline pipeline{};
    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        std::cerr << "vkCreateGraphicsPipelines failed\n";
        return 1;
    }

    // --- Execute the tape ----------------------------------------------------------------------
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    VkBuffer readback_buffer{};
    VkDeviceMemory readback_memory{};
    if (!CreateBuffer(ctx, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback_buffer, readback_memory)) {
        std::cerr << "readback buffer creation failed\n";
        return 1;
    }

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
    tape::ExecutionReport last_report{};
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
        vkAllocateCommandBuffers(ctx.device, &alloc_info, &cmd);

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin);

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

        VulkanSink sink{ctx, cmd, pipeline, pipeline_layout, vertex_buffer, index_buffer,
                        index_width, mvp};
        tape::ExecutionReport report{};
        std::string exec_error;
        if (!tape::Execute(tape, sink, report, exec_error, substitutions)) {
            std::cerr << "tape execution failed: " << exec_error << "\n";
            return 1;
        }
        last_report = report;

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.queue);

        VkCommandBuffer copy_cmd{};
        VkCommandBufferAllocateInfo copy_alloc{};
        copy_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        copy_alloc.commandPool = ctx.command_pool;
        copy_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        copy_alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(ctx.device, &copy_alloc, &copy_cmd);
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
        vkQueueSubmit(ctx.queue, 1, &copy_submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(ctx.queue);

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

    // --- Report --------------------------------------------------------------------------------
    std::cout << "execution: operations=" << last_report.operations_executed
              << " opaque=" << last_report.opaque_operations << " draws=" << last_report.draws
              << "\n";
    std::cout << "draw_from_tape: index_count=" << last_report.derived.index_count
              << " first_index=" << last_report.derived.first_index
              << " vertex_offset=" << last_report.derived.vertex_offset
              << " instance_count=" << last_report.derived.instance_count
              << " first_instance=" << last_report.draw_payload.first_instance << "\n";
    std::cout << "draw_semantic: primitive_type=" << last_report.draw_payload.primitive_type
              << " num_vertices=" << last_report.draw_payload.num_vertices
              << " start_index=" << last_report.draw_payload.start_index
              << " num_primitives=" << last_report.draw_payload.num_primitives
              << " num_instances=" << last_report.draw_payload.num_instances << "\n";
    std::cout << "viewport_from_tape: min=(" << last_report.viewport.min_x << ","
              << last_report.viewport.min_y << "," << last_report.viewport.min_z << ") max=("
              << last_report.viewport.max_x << "," << last_report.viewport.max_y << ","
              << last_report.viewport.max_z << ")\n";
    std::cout << "replay: views=" << rendered << " depth=yes perspective=yes\n";
    if (!last_report.substituted.empty()) {
        std::cout << "substituted_state:";
        for (const auto& item : last_report.substituted)
            std::cout << " " << item;
        std::cout << "\n";
    }

    std::cout << "\n-- provenance separation --\n";
    std::cout << "semantic_commands: recovered from the UE4 deferred command record\n";
    std::cout << "resource_contents: supplied by the SC-09H package (shadPS4-exported)\n";
    std::cout << "shaders: diagnostic substitute, not the game's\n";
    if (!tape.unresolved.empty()) {
        std::cout << "unresolved_dependencies:\n";
        for (const auto& dependency : tape.unresolved) {
            std::cout << "  - " << dependency.what << " (owner: " << dependency.owner
                      << "): " << dependency.reason << "\n";
        }
    }

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
    vkDestroyBuffer(ctx.device, index_buffer, nullptr);
    vkFreeMemory(ctx.device, index_memory, nullptr);
    vkDestroyBuffer(ctx.device, vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, vertex_memory, nullptr);
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return rendered == 3 ? 0 : 1;
}
