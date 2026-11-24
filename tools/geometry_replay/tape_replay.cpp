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
#include <atomic>
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

#include "draw_bounds.h"
#include "effective_shader.h"
#include "draw_identity.h"
#include "package_reader.h"

#include <nlohmann/json.hpp>

#include "index_bridge_reader.h"
#include "tape.h"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWidth = 640;
constexpr u32 kHeight = 640;

/// The resource pkg the tape references, read through the shared reader so the diagnostic
/// textured replay and this one cannot disagree about what a field means.
using ReplayPackage = package::Package;
using PackageResource = package::Resource;
using PackageVertexBinding = package::VertexBinding;
using PackageIndexBinding = package::IndexBinding;

u64 Fnv1a(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

const PackageResource* FindPackageResource(const ReplayPackage& pkg, u32 id) {
    for (const auto& resource : pkg.resources) {
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

const u32 kTapeTexVertSpv[] = {
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

const u32 kTapeTexFragSpv[] = {
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

const u32 kTapeInstidVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000b000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x0000000e,
    0x00000011, 0x0000001a, 0x00000025, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47,
    0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576,
    0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x76755f76, 0x00000000,
    0x00040005, 0x0000000b, 0x755f6e69, 0x00000076, 0x00040005, 0x0000000e, 0x6e695f76, 0x00007473,
    0x00070005, 0x00000011, 0x495f6c67, 0x6174736e, 0x4965636e, 0x7865646e, 0x00000000, 0x00060005,
    0x00000018, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x00000018, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x00000018, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x00000018, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x00000018, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000001a, 0x00000000, 0x00040005, 0x0000001d, 0x68737550, 0x00000000, 0x00040006,
    0x0000001d, 0x00000000, 0x0070766d, 0x00030005, 0x0000001f, 0x00006370, 0x00040005, 0x00000025,
    0x705f6e69, 0x0000736f, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000b,
    0x0000001e, 0x00000001, 0x00040047, 0x0000000e, 0x0000001e, 0x00000001, 0x00040047, 0x00000011,
    0x0000000b, 0x0000002b, 0x00030047, 0x00000018, 0x00000002, 0x00050048, 0x00000018, 0x00000000,
    0x0000000b, 0x00000000, 0x00050048, 0x00000018, 0x00000001, 0x0000000b, 0x00000001, 0x00050048,
    0x00000018, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x00000018, 0x00000003, 0x0000000b,
    0x00000004, 0x00030047, 0x0000001d, 0x00000002, 0x00040048, 0x0000001d, 0x00000000, 0x00000005,
    0x00050048, 0x0000001d, 0x00000000, 0x00000007, 0x00000010, 0x00050048, 0x0000001d, 0x00000000,
    0x00000023, 0x00000000, 0x00040047, 0x00000025, 0x0000001e, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000007, 0x0004003b, 0x0000000a,
    0x0000000b, 0x00000001, 0x00040020, 0x0000000d, 0x00000003, 0x00000006, 0x0004003b, 0x0000000d,
    0x0000000e, 0x00000003, 0x00040015, 0x0000000f, 0x00000020, 0x00000001, 0x00040020, 0x00000010,
    0x00000001, 0x0000000f, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040017, 0x00000014,
    0x00000006, 0x00000004, 0x00040015, 0x00000015, 0x00000020, 0x00000000, 0x0004002b, 0x00000015,
    0x00000016, 0x00000001, 0x0004001c, 0x00000017, 0x00000006, 0x00000016, 0x0006001e, 0x00000018,
    0x00000014, 0x00000006, 0x00000017, 0x00000017, 0x00040020, 0x00000019, 0x00000003, 0x00000018,
    0x0004003b, 0x00000019, 0x0000001a, 0x00000003, 0x0004002b, 0x0000000f, 0x0000001b, 0x00000000,
    0x00040018, 0x0000001c, 0x00000014, 0x00000004, 0x0003001e, 0x0000001d, 0x0000001c, 0x00040020,
    0x0000001e, 0x00000009, 0x0000001d, 0x0004003b, 0x0000001e, 0x0000001f, 0x00000009, 0x00040020,
    0x00000020, 0x00000009, 0x0000001c, 0x00040017, 0x00000023, 0x00000006, 0x00000003, 0x00040020,
    0x00000024, 0x00000001, 0x00000023, 0x0004003b, 0x00000024, 0x00000025, 0x00000001, 0x0004002b,
    0x00000006, 0x00000027, 0x3f800000, 0x00040020, 0x0000002d, 0x00000003, 0x00000014, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007,
    0x0000000c, 0x0000000b, 0x0003003e, 0x00000009, 0x0000000c, 0x0004003d, 0x0000000f, 0x00000012,
    0x00000011, 0x0004006f, 0x00000006, 0x00000013, 0x00000012, 0x0003003e, 0x0000000e, 0x00000013,
    0x00050041, 0x00000020, 0x00000021, 0x0000001f, 0x0000001b, 0x0004003d, 0x0000001c, 0x00000022,
    0x00000021, 0x0004003d, 0x00000023, 0x00000026, 0x00000025, 0x00050051, 0x00000006, 0x00000028,
    0x00000026, 0x00000000, 0x00050051, 0x00000006, 0x00000029, 0x00000026, 0x00000001, 0x00050051,
    0x00000006, 0x0000002a, 0x00000026, 0x00000002, 0x00070050, 0x00000014, 0x0000002b, 0x00000028,
    0x00000029, 0x0000002a, 0x00000027, 0x00050091, 0x00000014, 0x0000002c, 0x00000022, 0x0000002b,
    0x00050041, 0x0000002d, 0x0000002e, 0x0000001a, 0x0000001b, 0x0003003e, 0x0000002e, 0x0000002c,
    0x000100fd, 0x00010038,
};

const u32 kTapeInstidFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000041, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000011, 0x00000017, 0x00000037,
    0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47,
    0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576,
    0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65, 0x74636572, 0x00657669,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00040005, 0x00000009, 0x65786574, 0x0000006c,
    0x00040005, 0x0000000d, 0x65745f75, 0x00000078, 0x00040005, 0x00000011, 0x76755f76, 0x00000000,
    0x00030005, 0x00000015, 0x00000074, 0x00040005, 0x00000017, 0x6e695f76, 0x00007473, 0x00040005,
    0x0000001d, 0x746e6974, 0x00000000, 0x00050005, 0x00000037, 0x5f74756f, 0x6f6c6f63, 0x00000072,
    0x00040047, 0x0000000d, 0x00000021, 0x00000000, 0x00040047, 0x0000000d, 0x00000022, 0x00000000,
    0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00040047, 0x00000017, 0x0000001e, 0x00000001,
    0x00040047, 0x00000037, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00090019, 0x0000000a, 0x00000006, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000,
    0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f,
    0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000014, 0x00000007, 0x00000006,
    0x00040020, 0x00000016, 0x00000001, 0x00000006, 0x0004003b, 0x00000016, 0x00000017, 0x00000001,
    0x0004002b, 0x00000006, 0x00000019, 0x3ebd70a4, 0x00040017, 0x0000001b, 0x00000006, 0x00000003,
    0x00040020, 0x0000001c, 0x00000007, 0x0000001b, 0x0004002b, 0x00000006, 0x0000001e, 0x3f000000,
    0x0004002b, 0x00000006, 0x0000001f, 0x40c90fd0, 0x0004002b, 0x00000006, 0x00000021, 0x00000000,
    0x0004002b, 0x00000006, 0x00000028, 0x3ea8f5c3, 0x0004002b, 0x00000006, 0x0000002f, 0x3f2b851f,
    0x00040020, 0x00000036, 0x00000003, 0x00000007, 0x0004003b, 0x00000036, 0x00000037, 0x00000003,
    0x0004002b, 0x00000006, 0x0000003c, 0x3f800000, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b,
    0x00000014, 0x00000015, 0x00000007, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000007, 0x0004003d,
    0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f, 0x00000012, 0x00000011, 0x00050057,
    0x00000007, 0x00000013, 0x0000000e, 0x00000012, 0x0003003e, 0x00000009, 0x00000013, 0x0004003d,
    0x00000006, 0x00000018, 0x00000017, 0x00050085, 0x00000006, 0x0000001a, 0x00000018, 0x00000019,
    0x0003003e, 0x00000015, 0x0000001a, 0x0004003d, 0x00000006, 0x00000020, 0x00000015, 0x00050081,
    0x00000006, 0x00000022, 0x00000020, 0x00000021, 0x00050085, 0x00000006, 0x00000023, 0x0000001f,
    0x00000022, 0x0006000c, 0x00000006, 0x00000024, 0x00000001, 0x0000000e, 0x00000023, 0x00050085,
    0x00000006, 0x00000025, 0x0000001e, 0x00000024, 0x00050081, 0x00000006, 0x00000026, 0x0000001e,
    0x00000025, 0x0004003d, 0x00000006, 0x00000027, 0x00000015, 0x00050081, 0x00000006, 0x00000029,
    0x00000027, 0x00000028, 0x00050085, 0x00000006, 0x0000002a, 0x0000001f, 0x00000029, 0x0006000c,
    0x00000006, 0x0000002b, 0x00000001, 0x0000000e, 0x0000002a, 0x00050085, 0x00000006, 0x0000002c,
    0x0000001e, 0x0000002b, 0x00050081, 0x00000006, 0x0000002d, 0x0000001e, 0x0000002c, 0x0004003d,
    0x00000006, 0x0000002e, 0x00000015, 0x00050081, 0x00000006, 0x00000030, 0x0000002e, 0x0000002f,
    0x00050085, 0x00000006, 0x00000031, 0x0000001f, 0x00000030, 0x0006000c, 0x00000006, 0x00000032,
    0x00000001, 0x0000000e, 0x00000031, 0x00050085, 0x00000006, 0x00000033, 0x0000001e, 0x00000032,
    0x00050081, 0x00000006, 0x00000034, 0x0000001e, 0x00000033, 0x00060050, 0x0000001b, 0x00000035,
    0x00000026, 0x0000002d, 0x00000034, 0x0003003e, 0x0000001d, 0x00000035, 0x0004003d, 0x00000007,
    0x00000038, 0x00000009, 0x0008004f, 0x0000001b, 0x00000039, 0x00000038, 0x00000038, 0x00000000,
    0x00000001, 0x00000002, 0x0004003d, 0x0000001b, 0x0000003a, 0x0000001d, 0x00050085, 0x0000001b,
    0x0000003b, 0x00000039, 0x0000003a, 0x00050051, 0x00000006, 0x0000003d, 0x0000003b, 0x00000000,
    0x00050051, 0x00000006, 0x0000003e, 0x0000003b, 0x00000001, 0x00050051, 0x00000006, 0x0000003f,
    0x0000003b, 0x00000002, 0x00070050, 0x00000007, 0x00000040, 0x0000003d, 0x0000003e, 0x0000003f,
    0x0000003c, 0x0003003e, 0x00000037, 0x00000040, 0x000100fd, 0x00010038,
};

/// Counts Vulkan validation errors. The replay reports the count rather than claiming validation
/// success from the absence of a device loss, which is not the same thing.
std::atomic<u64> g_validation_errors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        g_validation_errors.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "validation error: " << (data->pMessageIdName ? data->pMessageIdName : "")
                  << " " << (data->pMessage ? data->pMessage : "") << "\n";
    }
    return VK_FALSE;
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
               u32 index_element_size, const Mat4& mvp, VkDescriptorSet descriptor_set = {},
               bool textured = false, bool bind_descriptors = false,
               std::vector<u8> push_bytes = {}, u32 push_offset = 0, u32 push_size = 0,
               u32 push_stages = 0)
        : ctx_(ctx), cmd_(cmd), pipeline_(pipeline), layout_(layout), vertex_buffer_(vertex_buffer),
          index_buffer_(index_buffer), index_element_size_(index_element_size), mvp_(mvp),
          descriptor_set_(descriptor_set), textured_(textured), bind_descriptors_(bind_descriptors),
          push_bytes_(std::move(push_bytes)), push_offset_(push_offset), push_size_(push_size),
          push_stages_(push_stages) {}

    void BeginFrame() override {
        // The tape carries no SetGraphicsPipeline operation: the bound-shader-state command is not
        // in the recovered command cluster. The diagnostic pipeline is therefore bound here, and
        // the caller reports it as substituted state rather than letting the draw silently produce
        // nothing.
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        // A descriptor set is bound whenever the pipeline layout requires one. That is a property
        // of the shaders, not of whether a diagnostic texture happens to exist: an effective shader
        // that needs only a uniform buffer has no texture at all, and gating the bind on texturing
        // would leave its set unbound.
        if (bind_descriptors_ && descriptor_set_ != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                                    &descriptor_set_, 0, nullptr);
        }
        // The push constants come from the captured state in effective mode. The diagnostic MVP is
        // a substitute and is pushed only when no captured range was supplied.
        if (push_size_ != 0) {
            vkCmdPushConstants(cmd_, layout_, push_stages_, push_offset_, push_size_,
                               push_bytes_.data());
        } else {
            vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp_.m),
                               mvp_.m);
        }
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
        // select a different framebuffer, because the pkg does not carry target contents.
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
    VkDescriptorSet descriptor_set_{};
    bool textured_{};
    /// Whether the pipeline layout requires a descriptor set, independent of diagnostic texturing.
    bool bind_descriptors_{};
    /// The captured push-constant bytes, offset, size and stage mask.
    std::vector<u8> push_bytes_;
    u32 push_offset_{};
    u32 push_size_{};
    u32 push_stages_{};
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
                     "[--shader-mode diagnostic|effective] [--substitute-viewport] "
                     "[--substitute-render-targets] [--synthetic-fixture] [--untextured] "
                     "[--instance-id]\n";
        return 2;
    }
    const std::string directory = argv[1];
    std::string prefix = "tape";
    bool substitute_viewport = false;
    bool substitute_targets = false;
    bool force_untextured = false;
    bool instance_id_mode = false;
    bool synthetic_fixture = false;
    bool shader_mode_effective = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--substitute-viewport") {
            substitute_viewport = true;
        } else if (arg == "--substitute-render-targets") {
            substitute_targets = true;
        } else if (arg == "--synthetic-fixture") {
            // The tape was built from the package rather than captured alongside it. That is a
            // legitimate way to exercise the integration path, but it is not provenance evidence,
            // so it must be declared and is reported as synthetic throughout.
            synthetic_fixture = true;
        } else if (arg == "--instance-id") {
            // A diagnostic substitution: the draw is unchanged, the fragment colour is tinted by
            // the instance index so a mutation of the instance range is visible. The captured draw
            // uses two instances of the same geometry, so without this the two are identical.
            instance_id_mode = true;
        } else if (arg == "--shader-mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "effective") {
                shader_mode_effective = true;
            } else if (mode == "diagnostic") {
                shader_mode_effective = false;
            } else {
                std::cerr << "--shader-mode must be diagnostic or effective, not " << mode << "\n";
                return 2;
            }
        } else if (arg == "--untextured") {
            // A resource mutation: the semantic tape is untouched, only the sampling is disabled.
            // The draw arguments must be identical to the textured run.
            force_untextured = true;
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

    // --- Read the original index-resource bridge -----------------------------------------------
    //
    // The bridge is the independent observation that resolves the original UE4 index-buffer object
    // into a guest allocation. It cannot be reconstructed from the package: the package carries a
    // guest address, and deriving the bridge from it would compare the package with itself.
    //
    // A missing or unreadable sidecar is not fatal here, because a synthetic fixture has none. It
    // is fatal for a verified-original claim, which the join enforces: without a comparable bridge
    // the join reports insufficient evidence and the replay refuses original-mode execution.
    std::vector<bridge::Record> bridge_records;
    {
        bridge::Problem bridge_problem{};
        std::string bridge_error;
        if (!bridge::Read(directory, bridge_records, bridge_problem, bridge_error)) {
            std::cout << "bridge: " << bridge::ProblemName(bridge_problem) << " (" << bridge_error
                      << ")\n";
        } else {
            std::cout << "bridge: records=" << bridge_records.size() << "\n";
        }
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

    // --- Read the resource pkg -------------------------------------------------------------
    ReplayPackage pkg{};
    if (!package::Read(directory, pkg, error, /*require_effective=*/shader_mode_effective)) {
        std::cerr << "pkg read failed: " << error << "\n";
        return 1;
    }
    if (pkg.provenance_id != tape.provenance_id) {
        std::cerr << "pkg provenance " << pkg.provenance_id
                  << " does not match tape provenance " << tape.provenance_id << "\n";
        return 1;
    }
    std::cout << "package: provenance=" << pkg.provenance_id
              << " resources=" << pkg.resources.size()
              << " vertex_bindings=" << pkg.vertex_bindings.size() << "\n";

    // The tape determines the draw; the pkg supplies the texture. When the pkg carries a
    // captured texture with a verified sampler and a usable UV attribute, the replay samples it.
    // Otherwise it falls back to the position-only diagnostic, and says which it did.
    const package::Texture* textured = package::FindTextured(pkg);
    const package::Sampler* sampler =
        textured != nullptr ? package::FindSampler(pkg, *textured) : nullptr;
    const bool textured_mode = !force_untextured && textured != nullptr && sampler != nullptr &&
                               pkg.uv.looks_like_uv && pkg.readiness.geometry_ready;

    // --- Effective shader resolution -----------------------------------------------------------
    // In effective mode the package's own captured modules are the shaders. Every check is a
    // refusal rather than a fallback: substituting a diagnostic shader here would present a test
    // render as original shader execution, which is the one thing this mode must never do.
    effective::Resolution effective_shaders{};
    if (shader_mode_effective) {
        effective_shaders = effective::Resolve(pkg);
        std::cout << "shader_mode: effective\n";
        if (effective_shaders.problem != effective::Problem::None) {
            std::cerr << "effective shader resolution failed: "
                      << effective::ProblemName(effective_shaders.problem) << "\n";
            std::cerr << "  " << effective_shaders.reason << "\n";
            std::cerr << "  effective mode does not fall back to a diagnostic shader\n";
            return 1;
        }
        for (const auto& stage : effective_shaders.stages) {
            std::cout << "stage: logical=" << stage.logical_stage
                      << " program_hash=0x" << std::hex << stage.program_hash << std::dec
                      << " perm_idx=" << stage.permutation_index
                      << " perm_hash=0x" << std::hex << stage.permutation_hash << std::dec
                      << " patched=" << (stage.patched ? "yes" : "no")
                      << " bytes=" << stage.bytes.size()
                      << " descriptors=" << stage.declared.size() << "\n";
        }
        std::cout << "bindings: " << effective_shaders.bindings.size()
                  << " push_constant=" << effective_shaders.push_constant_size << "\n";
        for (const auto& binding : effective_shaders.bindings) {
            std::cout << "binding: set=" << binding.set << " binding=" << binding.binding
                      << " type=" << binding.descriptor_type << " count=" << binding.count
                      << " stages=" << binding.stage_flags
                      << " resource=" << binding.resource_ref
                      << " range=" << binding.range << "\n";
        }
    } else {
        std::cout << "shader_mode: diagnostic\n";
    }

    // --- Resolve the tape's resources against the pkg --------------------------------------
    // The tape's resource ids are backend-neutral. The pkg reference is what connects them to
    // bytes, and it is recorded rather than assumed.
    u32 resolved = 0;
    u32 unresolved = 0;
    for (const auto& resource : tape.resources) {
        if (resource.package_resource_ref == 0) {
            ++unresolved;
            continue;
        }
        if (FindPackageResource(pkg, resource.package_resource_ref) == nullptr) {
            std::cerr << "tape resource " << resource.id << " references pkg resource "
                      << resource.package_resource_ref << " which is absent\n";
            return 1;
        }
        ++resolved;
    }
    std::cout << "resources: resolved_from_package=" << resolved
              << " without_package_reference=" << unresolved << "\n";

    // --- Same-draw identity -------------------------------------------------------------------
    // The tape and the package are produced by different subsystems. Joining them because both are
    // valid would be the defect this check exists to prevent, so the join is explicit and its
    // outcome is reported. A mismatch is refused before any GPU work.
    {
        tape::TapeIdentity tape_identity{};
        tape_identity.provenance_id = tape.provenance_id;
        for (const auto& resource : tape.resources) {
            if (resource.kind == tape::ResourceKind::IndexBuffer) {
                tape_identity.index_guest_base = resource.guest_base;
                tape_identity.index_guest_size = resource.guest_size;
                tape_identity.index_element_size = resource.element_size;
                // The tape declares which namespace its index value belongs to. A real capture
                // records the RHI invocation argument, an original UE4 object pointer, which is not
                // a guest GPU address and must not be compared as one.
                if (resource.index_domain == "guest_address") {
                    tape_identity.index_domain = tape::IndexDomain::GuestAddress;
                } else if (resource.index_domain == "host_buffer") {
                    tape_identity.index_domain = tape::IndexDomain::HostBuffer;
                } else if (resource.index_domain == "ue4_object") {
                    tape_identity.index_domain = tape::IndexDomain::Ue4Object;
                } else {
                    tape_identity.index_domain = tape::IndexDomain::Unknown;
                }
                break;
            }
        }
        for (const auto& operation : tape.operations) {
            if (operation.kind != tape::OpKind::DrawIndexed)
                continue;
            tape_identity.primitive_type = operation.draw.primitive_type;
            tape_identity.num_primitives = operation.draw.num_primitives;
            tape_identity.num_instances = operation.draw.num_instances;
            tape_identity.num_vertices = operation.draw.num_vertices;
            tape_identity.start_index = operation.draw.start_index;
            tape_identity.base_vertex_index = operation.draw.base_vertex_index;
            tape_identity.first_instance = operation.draw.first_instance;
            tape_identity.derived_index_count = operation.derived.index_count;
            break;
        }

        tape::PackageIdentity package_identity{};
        package_identity.provenance_id = pkg.provenance_id;
        package_identity.index_guest_base = pkg.index.guest_base;
        package_identity.index_guest_size = pkg.index.guest_size;
        // The package records the guest GPU address the draw consumed.
        package_identity.index_domain = tape::IndexDomain::GuestAddress;
        package_identity.index_element_size = pkg.index.index_size;
        package_identity.index_count = pkg.index.index_count;
        package_identity.first_index = pkg.index.first_index;
        package_identity.base_vertex = pkg.index.base_vertex;
        package_identity.instance_count = pkg.index.instance_count;
        package_identity.first_instance = pkg.index.first_instance;
        package_identity.has_index_binding = pkg.has_index;

        // The bridge-aware join. The bridge is read from the sidecar the identity observer wrote;
        // it cannot be reconstructed from the package, because the package carries a guest address
        // and the bridge is the independent observation that resolves the original UE4 object into
        // one.
        bridge::Record bridge_record{};
        bool bridge_ambiguous = false;
        const bool have_bridge =
            bridge::FindByProvenance(bridge_records, tape_identity.provenance_id, bridge_record,
                                     bridge_ambiguous);

        tape::IndexBridge index_bridge{};
        if (have_bridge) {
            index_bridge.execution_id = bridge_record.execution_id;
            index_bridge.original_rhi_object = bridge_record.original_rhi_object;
            index_bridge.has_original_rhi_object = bridge_record.has_original_rhi_object;
            index_bridge.guest_allocation_base = bridge_record.selected_guest_address;
            index_bridge.has_guest_allocation_base =
                bridge_record.has_selected_guest_address;
            index_bridge.guest_allocation_size = bridge_record.allocation_size;
            index_bridge.has_guest_allocation_size = bridge_record.has_allocation_size;
            index_bridge.allocation_offset = bridge_record.allocation_offset;
            index_bridge.has_allocation_offset = bridge_record.has_allocation_offset;
            index_bridge.index_element_width = bridge_record.index_element_width;
            index_bridge.has_index_element_width = bridge_record.has_index_element_width;
            index_bridge.resource_generation = bridge_record.resource_generation;
            index_bridge.has_resource_generation = bridge_record.has_resource_generation;
            index_bridge.evidence = bridge_record.address_evidence;
        }

        // The bridge must name the same original object the tape recorded. A bridge that resolved
        // a different object is not this draw's bridge, however well the addresses agree.
        if (have_bridge && index_bridge.has_original_rhi_object &&
            tape_identity.index_guest_base != 0 &&
            index_bridge.original_rhi_object != tape_identity.index_guest_base) {
            std::cout << "identity: bridge_object_mismatch bridge="
                      << index_bridge.original_rhi_object
                      << " tape=" << tape_identity.index_guest_base << "\n";
        }

        const tape::JoinResult join =
            tape::JoinTapeToPackageWithBridge(tape_identity, package_identity, index_bridge);
        std::cout << "identity: " << tape::JoinOutcomeName(join.outcome) << "\n";
        for (const auto& field : join.fields) {
            std::cout << "identity:   " << field.name << " tape=" << field.tape_value
                      << " package=" << field.package_value
                      << (field.comparable ? (field.agrees ? " agree" : " DISAGREE")
                                           : " not_comparable")
                      << "\n";
        }
        if (join.derived_consistent) {
            std::cout << "identity: derived_consistent=yes\n";
        } else {
            std::cout << "identity: derived_consistent=no (mutated:";
            for (const auto name : join.derived_disagreements) {
                std::cout << " " << name;
            }
            std::cout << ")\n";
        }
        // The provenance classification. A synthetic fixture can execute, but it can never be
        // promoted to a verified original draw: the label is what carries that distinction.
        const char* provenance_class = nullptr;
        if (join.outcome == tape::JoinOutcome::Joined) {
            provenance_class = synthetic_fixture ? "synthetic_fixture" : "original_tape_matched";
        } else if (join.outcome == tape::JoinOutcome::Mismatched) {
            provenance_class = "rejected";
        } else if (join.outcome == tape::JoinOutcome::InsufficientEvidence) {
            provenance_class = "insufficient_evidence";
        } else {
            provenance_class = "rejected";
        }
        std::cout << "provenance: " << provenance_class << "\n";

        if (join.outcome != tape::JoinOutcome::Joined) {
            // A mismatch is always fatal, synthetic or not. The synthetic label relaxes only the
            // requirement for an original identity to exist; it never permits two captures that
            // positively disagree to be executed together.
            if (join.outcome == tape::JoinOutcome::Mismatched) {
                std::cerr << "identity rejected: " << join.reason << "\n";
                std::cerr << "  the tape and the package positively disagree, so executing them "
                             "together would be a fabricated match\n";
                return 1;
            }
            if (synthetic_fixture) {
                // A synthetic fixture has no original UE4 object, so the index resource cannot
                // establish the join. Running it is still useful, but only under the synthetic
                // label, and only when the derived draw agrees so the fixture is at least
                // self-consistent.
                if (!join.derived_consistent) {
                    std::cerr << "synthetic fixture is not self-consistent: " << join.reason
                              << "\n";
                    return 1;
                }
                std::cout << "identity: synthetic fixture accepted under the synthetic label "
                             "(no original UE4 identity to compare)\n";
            } else {
                std::cerr << "identity rejected: " << join.reason << "\n";
                std::cerr << "  the tape and the package do not describe the same draw, so "
                             "executing them together would be a fabricated match\n";
                std::cerr << "  pass --synthetic-fixture only when the tape was built from the "
                             "package rather than captured alongside it\n";
                return 1;
            }
        }
    }

    // --- Geometry from the pkg -------------------------------------------------------------
    const PackageVertexBinding* position_binding = nullptr;
    for (const auto& vertex : pkg.vertex_bindings) {
        if (vertex.binding == pkg.geometry.position_binding) {
            position_binding = &vertex;
            break;
        }
    }
    if (position_binding == nullptr) {
        std::cerr << "pkg has no position binding\n";
        return 1;
    }
    const PackageResource* position_resource =
        FindPackageResource(pkg, position_binding->resource_ref);
    if (position_resource == nullptr || position_binding->stride == 0) {
        std::cerr << "pkg position resource is missing\n";
        return 1;
    }

    const u32 stride = position_binding->stride;
    const u32 vertex_count = static_cast<u32>(position_resource->bytes.size() / stride);

    const float* lo = pkg.geometry.bounds_min;
    const float* hi = pkg.geometry.bounds_max;
    float centre[3] = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float extent = 0.0f;
    for (u32 c = 0; c < 3; ++c)
        extent = std::max(extent, hi[c] - lo[c]);
    const float scale = extent > 1e-6f ? 2.0f / extent : 1.0f;

    // In textured mode the vertex stream is interleaved position and UV, because the diagnostic
    // shader samples the captured texture with the package's attributed UV binding. The UV comes
    // from that binding rather than from an assumption about which attribute holds it.
    const u32 floats_per_vertex = textured_mode ? 5u : 3u;
    std::vector<float> positions(static_cast<size_t>(vertex_count) * floats_per_vertex);
    const PackageVertexBinding* uv_binding = nullptr;
    const PackageResource* uv_resource = nullptr;
    if (textured_mode) {
        for (const auto& vertex : pkg.vertex_bindings) {
            if (vertex.binding == pkg.uv.binding) {
                uv_binding = &vertex;
                break;
            }
        }
        if (uv_binding != nullptr) {
            uv_resource = FindPackageResource(pkg, uv_binding->resource_ref);
        }
        if (uv_binding == nullptr || uv_resource == nullptr) {
            std::cerr << "textured mode needs a UV binding and resource\n";
            return 1;
        }
    }

    for (u32 v = 0; v < vertex_count; ++v) {
        for (u32 c = 0; c < 3; ++c) {
            float value = 0.0f;
            if (c < position_binding->element_count) {
                std::memcpy(&value, position_resource->bytes.data() + v * stride + c * 4,
                            sizeof(value));
            }
            positions[v * floats_per_vertex + c] = (value - centre[c]) * scale;
        }
        if (textured_mode) {
            // The UV stream is half-float in the captured layout, so each component is converted
            // rather than reinterpreted as a float.
            for (u32 c = 0; c < 2; ++c) {
                const u32 component = c == 0 ? pkg.uv.component_u : pkg.uv.component_v;
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
                positions[v * floats_per_vertex + 3 + c] = value;
            }
        }
    }

    // The index bytes come from the pkg; the count and offset come from the tape.
    const PackageResource* index_resource =
        pkg.has_index ? FindPackageResource(pkg, pkg.index.resource_ref) : nullptr;
    if (index_resource == nullptr) {
        std::cerr << "pkg has no index resource\n";
        return 1;
    }
    const u32 index_width = pkg.index.index_size;
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

    // --- Draw bounds ---------------------------------------------------------------------------
    // A mutation that reads past the end of a buffer proves nothing: with robust buffer access the
    // draw still executes and returns zeros, so the test would appear to pass while exercising an
    // invalid draw. The range is checked against the resources the draw actually binds, before any
    // GPU work.
    {
        bounds::DrawRange range{};
        range.index_elements = indices.size();
        range.vertex_count = vertex_count;
        for (const auto& operation : tape.operations) {
            if (operation.kind != tape::OpKind::DrawIndexed)
                continue;
            range.first_index = operation.derived.first_index;
            range.index_count = operation.derived.index_count;
            range.base_vertex = operation.derived.vertex_offset;
            break;
        }
        const bounds::Check check = bounds::CheckDraw(range, indices);
        std::cout << "bounds: first_index=" << range.first_index
                  << " index_count=" << range.index_count
                  << " base_vertex=" << range.base_vertex
                  << " index_elements=" << range.index_elements
                  << " vertex_count=" << range.vertex_count << "\n";
        if (!check.valid) {
            std::cerr << "draw is out of bounds: " << check.reason << "\n";
            std::cerr << "  refusing before GPU execution rather than relying on robust buffer "
                         "access to hide an invalid fetch\n";
            return 1;
        }
        std::cout << "bounds: ok max_index=" << check.max_index
                  << " vertex_range=[" << check.min_vertex << "," << check.max_vertex << "]\n";
    }

    // --- Vulkan setup --------------------------------------------------------------------------
    ReplayContext ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_tape_replay";
    app.apiVersion = VK_API_VERSION_1_1;

    // Validation is opt-in through the environment, so a normal run is unaffected. When it is on,
    // the layer and the debug-utils extension are both requested and the messenger is installed
    // after instance creation.
    const char* validation_env = std::getenv("DERACINE_REPLAY_VALIDATION");
    const bool want_validation = validation_env != nullptr && validation_env[0] == '1';
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (want_validation) {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = layers;
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = extensions;
    }
    if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed\n";
        return 1;
    }

    VkDebugUtilsMessengerEXT messenger{};
    if (want_validation) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger == nullptr) {
            std::cerr << "validation requested but vkCreateDebugUtilsMessengerEXT is absent\n";
            return 1;
        }
        VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
        messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger_info.pfnUserCallback = DebugCallback;
        if (create_messenger(ctx.instance, &messenger_info, nullptr, &messenger) != VK_SUCCESS) {
            std::cerr << "vkCreateDebugUtilsMessengerEXT failed\n";
            return 1;
        }
        std::cout << "validation: enabled\n";
    } else {
        std::cout << "validation: disabled (set DERACINE_REPLAY_VALIDATION=1 to enable)\n";
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

    // The captured sampler requests anisotropy, so the feature must be enabled on the device.
    // Requesting it without enabling it is a validation error and the sampler is invalid.
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &enabled_features;
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
        // A depth-only format must be viewed through the depth aspect. Using the colour aspect is a
        // validation error and the attachment cannot be used as a depth target.
        const bool is_depth = format == VK_FORMAT_D32_SFLOAT ||
                              format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                              format == VK_FORMAT_D24_UNORM_S8_UINT;
        const VkImageAspectFlags aspect =
            is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange = {aspect, 0, 1, 0, 1};
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
    const bool instid = textured_mode && instance_id_mode;

    // In effective mode the modules come from the package. They are the shaders this run executes,
    // and the diagnostic pair is not a fallback: resolution already refused a package that could
    // not supply them.
    const auto make_module_from_bytes = [&ctx](const std::vector<u8>& bytes) -> VkShaderModule {
        if (bytes.empty() || bytes.size() % 4 != 0) {
            return VK_NULL_HANDLE;
        }
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes.size();
        info.pCode = reinterpret_cast<const u32*>(bytes.data());
        VkShaderModule module{};
        if (vkCreateShaderModule(ctx.device, &info, nullptr, &module) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return module;
    };

    VkShaderModule vert_module{};
    VkShaderModule frag_module{};
    if (shader_mode_effective) {
        for (const auto& stage : effective_shaders.stages) {
            if (stage.logical_stage == 0) {
                vert_module = make_module_from_bytes(stage.bytes);
            } else if (stage.logical_stage == 4) {
                frag_module = make_module_from_bytes(stage.bytes);
            }
        }
    } else {
        vert_module =
            make_module(instid ? kTapeInstidVertSpv : (textured_mode ? kTapeTexVertSpv : kVertSpv),
                        instid ? sizeof(kTapeInstidVertSpv) / sizeof(u32)
                               : (textured_mode ? sizeof(kTapeTexVertSpv) / sizeof(u32)
                                                : sizeof(kVertSpv) / sizeof(u32)));
        frag_module =
            make_module(instid ? kTapeInstidFragSpv : (textured_mode ? kTapeTexFragSpv : kFragSpv),
                        instid ? sizeof(kTapeInstidFragSpv) / sizeof(u32)
                               : (textured_mode ? sizeof(kTapeTexFragSpv) / sizeof(u32)
                                                : sizeof(kFragSpv) / sizeof(u32)));
    }
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

    // In textured mode the vertex stream is interleaved position and UV, so the stride and the
    // attribute count both change. The UV comes from the package's attributed binding, not from a
    // fixed assumption about which attribute holds it.
    VkVertexInputAttributeDescription uv_attribute{};
    if (textured_mode) {
        vertex_binding_desc.stride = 5 * sizeof(float);
        uv_attribute.location = 1;
        uv_attribute.binding = 0;
        uv_attribute.format = VK_FORMAT_R32G32_SFLOAT;
        uv_attribute.offset = 3 * sizeof(float);
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding_desc;
    VkVertexInputAttributeDescription attributes[2] = {vertex_attribute, uv_attribute};
    vertex_input.vertexAttributeDescriptionCount = textured_mode ? 2u : 1u;
    vertex_input.pVertexAttributeDescriptions = attributes;

    // --- Captured pipeline state ---------------------------------------------------------------
    // In effective mode the fixed-function state comes from the package where it was captured. A
    // field the package did not capture keeps the diagnostic default, and the run reports which
    // fields those were, so a reader can tell captured state from substituted state.
    const package::PipelineState& captured = pkg.pipeline_state;
    std::vector<const char*> substituted_state;
    const auto captured_or = [&substituted_state](bool present, auto value, auto fallback,
                                                  const char* name) {
        if (present) {
            return value;
        }
        substituted_state.push_back(name);
        return fallback;
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology =
        shader_mode_effective
            ? static_cast<VkPrimitiveTopology>(captured_or(captured.has_topology,
                                                           captured.topology,
                                                           static_cast<u32>(
                                                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
                                                           "topology"))
            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
    raster.polygonMode =
        shader_mode_effective
            ? static_cast<VkPolygonMode>(captured_or(captured.polygon_mode != 0,
                                                     captured.polygon_mode,
                                                     static_cast<u32>(VK_POLYGON_MODE_FILL),
                                                     "polygon_mode"))
            : VK_POLYGON_MODE_FILL;
    // Cull mode zero is a legitimate value, so presence is carried by the package having captured
    // any pipeline state at all rather than by the value being non-zero.
    raster.cullMode = shader_mode_effective
                          ? static_cast<VkCullModeFlags>(captured.cull_mode)
                          : VK_CULL_MODE_NONE;
    raster.frontFace = shader_mode_effective
                           ? static_cast<VkFrontFace>(captured.front_face)
                           : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples =
        shader_mode_effective
            ? static_cast<VkSampleCountFlagBits>(captured_or(captured.samples != 0,
                                                             captured.samples, 1u, "samples"))
            : VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable =
        shader_mode_effective ? (captured.depth_test ? VK_TRUE : VK_FALSE) : VK_TRUE;
    depth_state.depthWriteEnable =
        shader_mode_effective ? (captured.depth_write ? VK_TRUE : VK_FALSE) : VK_TRUE;
    depth_state.depthCompareOp =
        shader_mode_effective
            ? static_cast<VkCompareOp>(captured_or(captured.depth_compare_op != 0,
                                                   captured.depth_compare_op,
                                                   static_cast<u32>(VK_COMPARE_OP_LESS_OR_EQUAL),
                                                   "depth_compare_op"))
            : VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    // The write mask is captured per attachment. A package that did not capture one keeps the
    // diagnostic all-channels mask, and says so.
    if (shader_mode_effective && !captured.write_mask.empty()) {
        blend_attachment.colorWriteMask = captured.write_mask[0];
    } else {
        if (shader_mode_effective) {
            substituted_state.push_back("write_mask");
        }
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    // Blend factors and operations are not captured by the producer, so blending stays disabled
    // rather than being invented. A package that captured blend enable is reported as needing a
    // field the schema does not carry.
    if (shader_mode_effective && !captured.blend_enable.empty() && captured.blend_enable[0] != 0) {
        std::cerr << "the package captured blend enable for attachment 0, but the schema does not "
                     "carry blend factors or operations, so the blend state cannot be reproduced\n";
        return 1;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    // The push-constant range is a property of the pipeline layout. In effective mode it comes from
    // what the modules declare, and the captured bytes must cover it; a package that did not
    // capture enough is refused rather than pushed with a substitute.
    VkPushConstantRange push_range{};
    if (shader_mode_effective && effective_shaders.push_constant_size != 0) {
        push_range.stageFlags = effective_shaders.push_constant_stages;
        push_range.offset = effective_shaders.push_constant_offset;
        push_range.size = effective_shaders.push_constant_size;
        if (effective_shaders.push_constant_bytes.size() <
            static_cast<size_t>(push_range.offset) + push_range.size) {
            std::cerr << "the modules declare a push-constant range of " << push_range.size
                      << " bytes at offset " << push_range.offset << ", but the package captured "
                      << effective_shaders.push_constant_bytes.size() << " bytes\n";
            return 1;
        }
    } else {
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(float) * 16;
    }

    VkDescriptorSetLayout descriptor_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet descriptor_set{};
    VkSampler vk_sampler{};
    VkImage texture_image{};
    VkDeviceMemory texture_memory{};
    VkImageView texture_view{};
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};

    if (textured_mode) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo set_layout{};
        set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout.bindingCount = 1;
        set_layout.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(ctx.device, &set_layout, nullptr, &descriptor_layout) !=
            VK_SUCCESS) {
            std::cerr << "vkCreateDescriptorSetLayout failed\n";
            return 1;
        }
    }

    // In effective mode the descriptor set layout comes from the resolved bindings, which came from
    // the modules themselves and the captured production bindings. It is not a fixed layout: the
    // modules declare what they need, and a package whose captured interface disagrees with them
    // was already refused.
    VkDescriptorSetLayout effective_set_layout{};
    std::vector<VkBuffer> effective_buffers;
    std::vector<VkDeviceMemory> effective_memories;
    std::vector<VkDescriptorSetLayoutBinding> effective_bindings;
    if (shader_mode_effective) {
        for (const auto& binding : effective_shaders.bindings) {
            VkDescriptorSetLayoutBinding entry{};
            entry.binding = binding.binding;
            entry.descriptorType = static_cast<VkDescriptorType>(binding.descriptor_type);
            entry.descriptorCount = binding.count;
            entry.stageFlags = binding.stage_flags;
            effective_bindings.push_back(entry);
        }
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = static_cast<u32>(effective_bindings.size());
        set_info.pBindings = effective_bindings.data();
        if (vkCreateDescriptorSetLayout(ctx.device, &set_info, nullptr, &effective_set_layout) !=
            VK_SUCCESS) {
            std::cerr << "effective descriptor set layout creation failed\n";
            return 1;
        }
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    if (shader_mode_effective) {
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &effective_set_layout;
    } else if (textured_mode) {
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_layout;
    }
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
        // --- Texture upload and descriptor set -------------------------------------------------
        // The image is created at the captured texel extent and uploaded from the package's
        // texture sidecar. The sampler is built from the captured state, so the sampling behaviour
        // is the one the production renderer used rather than a diagnostic default.
        if (textured_mode) {
            const auto& sub = textured->subresources.front();
            VkFormat image_format = VK_FORMAT_UNDEFINED;
            switch (textured->image_format) {
            case 9:
                image_format = VK_FORMAT_R8_UNORM;
                break;
            case 37:
                image_format = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            case 43:
                image_format = VK_FORMAT_R8G8B8A8_SRGB;
                break;
            case 44:
                image_format = VK_FORMAT_B8G8R8A8_UNORM;
                break;
            case 131:
                image_format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                break;
            case 137:
                image_format = VK_FORMAT_BC3_UNORM_BLOCK;
                break;
            case 138:
                image_format = VK_FORMAT_BC3_SRGB_BLOCK;
                break;
            case 139:
                image_format = VK_FORMAT_BC4_UNORM_BLOCK;
                break;
            case 145:
                image_format = VK_FORMAT_BC5_UNORM_BLOCK;
                break;
            case 147:
                image_format = VK_FORMAT_BC7_UNORM_BLOCK;
                break;
            case 148:
                image_format = VK_FORMAT_BC7_SRGB_BLOCK;
                break;
            default:
                std::cerr << "unsupported captured image format " << textured->image_format
                          << "\n";
                return 1;
            }

            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = image_format;
            image_info.extent = {sub.width, sub.height, 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = 1;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(ctx.device, &image_info, nullptr, &texture_image) != VK_SUCCESS) {
                std::cerr << "texture image creation failed\n";
                return 1;
            }
            VkMemoryRequirements image_requirements{};
            vkGetImageMemoryRequirements(ctx.device, texture_image, &image_requirements);
            VkMemoryAllocateInfo image_alloc{};
            image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            image_alloc.allocationSize = image_requirements.size;
            image_alloc.memoryTypeIndex = FindMemoryType(ctx, image_requirements.memoryTypeBits,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(ctx.device, &image_alloc, nullptr, &texture_memory) !=
                    VK_SUCCESS ||
                vkBindImageMemory(ctx.device, texture_image, texture_memory, 0) != VK_SUCCESS) {
                std::cerr << "texture image memory failed\n";
                return 1;
            }

            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = texture_image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = image_format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(ctx.device, &view_info, nullptr, &texture_view) != VK_SUCCESS) {
                std::cerr << "texture view creation failed\n";
                return 1;
            }

            if (!CreateBuffer(ctx, sub.byte_length, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              staging, staging_memory)) {
                std::cerr << "texture staging buffer failed\n";
                return 1;
            }
            void* mapped = nullptr;
            if (vkMapMemory(ctx.device, staging_memory, 0, sub.byte_length, 0, &mapped) !=
                VK_SUCCESS) {
                std::cerr << "texture staging map failed\n";
                return 1;
            }
            std::memcpy(mapped, pkg.texture_blob.data() + sub.blob_offset,
                        static_cast<size_t>(sub.byte_length));
            vkUnmapMemory(ctx.device, staging_memory);

            VkSamplerCreateInfo sampler_info{};
            sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sampler_info.magFilter = sampler->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sampler_info.minFilter = sampler->min_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sampler_info.mipmapMode = sampler->mip_filter == 1
                                          ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            const auto address = [](u32 value) {
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
            };
            sampler_info.addressModeU = address(sampler->address_u);
            sampler_info.addressModeV = address(sampler->address_v);
            sampler_info.addressModeW = address(sampler->address_w);
            sampler_info.mipLodBias = sampler->lod_bias;
            sampler_info.anisotropyEnable =
                sampler->anisotropy_enable ? VK_TRUE : VK_FALSE;
            sampler_info.maxAnisotropy =
                sampler->max_anisotropy >= 1.0f ? sampler->max_anisotropy : 1.0f;
            sampler_info.compareEnable = sampler->compare_enable ? VK_TRUE : VK_FALSE;
            sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
            sampler_info.minLod = sampler->min_lod;
            sampler_info.maxLod = sampler->max_lod;
            sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            sampler_info.unnormalizedCoordinates = VK_FALSE;
            if (vkCreateSampler(ctx.device, &sampler_info, nullptr, &vk_sampler) != VK_SUCCESS) {
                std::cerr << "sampler creation failed\n";
                return 1;
            }

            VkDescriptorPoolSize pool_size{};
            pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            pool_size.descriptorCount = 1;
            VkDescriptorPoolCreateInfo pool_ci{};
            pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_ci.maxSets = 1;
            pool_ci.poolSizeCount = 1;
            pool_ci.pPoolSizes = &pool_size;
            if (vkCreateDescriptorPool(ctx.device, &pool_ci, nullptr, &descriptor_pool) !=
                VK_SUCCESS) {
                std::cerr << "descriptor pool creation failed\n";
                return 1;
            }
            VkDescriptorSetAllocateInfo set_alloc{};
            set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            set_alloc.descriptorPool = descriptor_pool;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts = &descriptor_layout;
            if (vkAllocateDescriptorSets(ctx.device, &set_alloc, &descriptor_set) != VK_SUCCESS) {
                std::cerr << "descriptor set allocation failed\n";
                return 1;
            }
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
        }

        // --- Effective-mode descriptor set -----------------------------------------------------
        // Every binding the modules declare is written from the resource the captured binding
        // named. A buffer binding whose resource the package does not carry was already refused, so
        // there is no path here that binds a dummy to make the draw succeed.
        if (shader_mode_effective) {
            std::map<u32, u32> type_counts;
            for (const auto& binding : effective_shaders.bindings) {
                type_counts[binding.descriptor_type] += binding.count;
            }
            std::vector<VkDescriptorPoolSize> pool_sizes;
            for (const auto& [type, count] : type_counts) {
                pool_sizes.push_back({static_cast<VkDescriptorType>(type), count});
            }
            VkDescriptorPoolCreateInfo pool_ci{};
            pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pool_ci.maxSets = 1;
            pool_ci.poolSizeCount = static_cast<u32>(pool_sizes.size());
            pool_ci.pPoolSizes = pool_sizes.data();
            if (vkCreateDescriptorPool(ctx.device, &pool_ci, nullptr, &descriptor_pool) !=
                VK_SUCCESS) {
                std::cerr << "effective descriptor pool creation failed\n";
                return 1;
            }
            VkDescriptorSetAllocateInfo set_alloc{};
            set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            set_alloc.descriptorPool = descriptor_pool;
            set_alloc.descriptorSetCount = 1;
            set_alloc.pSetLayouts = &effective_set_layout;
            if (vkAllocateDescriptorSets(ctx.device, &set_alloc, &descriptor_set) != VK_SUCCESS) {
                std::cerr << "effective descriptor set allocation failed\n";
                return 1;
            }

            // The buffer infos must outlive the update call, so they are held in a vector sized to
            // the binding count rather than in a loop-local.
            std::vector<VkDescriptorBufferInfo> buffer_infos;
            buffer_infos.reserve(effective_shaders.bindings.size());
            std::vector<VkDescriptorImageInfo> image_infos;
            image_infos.reserve(effective_shaders.bindings.size());
            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(effective_shaders.bindings.size());
            for (const auto& binding : effective_shaders.bindings) {
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = descriptor_set;
                write.dstBinding = binding.binding;
                write.descriptorCount = binding.count;
                write.descriptorType = static_cast<VkDescriptorType>(binding.descriptor_type);

                if (binding.descriptor_type == 6 || binding.descriptor_type == 7) {
                    const package::Resource* resource =
                        package::FindResource(pkg, binding.resource_ref);
                    if (resource == nullptr) {
                        std::cerr << "binding " << binding.binding << " names resource "
                                  << binding.resource_ref << ", which is absent\n";
                        return 1;
                    }
                    VkBuffer buffer{};
                    VkDeviceMemory memory{};
                    if (!CreateBuffer(ctx, resource->bytes.size(),
                                      binding.descriptor_type == 6
                                          ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                                          : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      buffer, memory)) {
                        std::cerr << "effective buffer creation failed\n";
                        return 1;
                    }
                    void* mapped = nullptr;
                    vkMapMemory(ctx.device, memory, 0, resource->bytes.size(), 0, &mapped);
                    std::memcpy(mapped, resource->bytes.data(), resource->bytes.size());
                    vkUnmapMemory(ctx.device, memory);
                    effective_buffers.push_back(buffer);
                    effective_memories.push_back(memory);

                    VkDescriptorBufferInfo info{};
                    info.buffer = buffer;
                    info.offset = binding.offset;
                    info.range = binding.range != 0 ? binding.range : resource->bytes.size();
                    buffer_infos.push_back(info);
                    write.pBufferInfo = &buffer_infos.back();
                } else if (binding.descriptor_type == 1 || binding.descriptor_type == 2) {
                    // A sampled image is written from the package's captured texture. Binding a
                    // placeholder would present a test render as a captured one, so a package
                    // without a usable texture is refused rather than satisfied.
                    if (texture_view == VK_NULL_HANDLE || vk_sampler == VK_NULL_HANDLE) {
                        std::cerr << "binding " << binding.binding
                                  << " needs a sampled image, and this package carries no captured "
                                     "texture with a verified sampler\n";
                        return 1;
                    }
                    VkDescriptorImageInfo info{};
                    info.sampler = binding.descriptor_type == 1 ? vk_sampler : VK_NULL_HANDLE;
                    info.imageView = texture_view;
                    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    image_infos.push_back(info);
                    write.pImageInfo = &image_infos.back();
                } else {
                    std::cerr << "binding " << binding.binding << " has descriptor type "
                              << binding.descriptor_type
                              << ", which this renderer cannot bind\n";
                    return 1;
                }
                writes.push_back(write);
            }
            vkUpdateDescriptorSets(ctx.device, static_cast<u32>(writes.size()), writes.data(), 0,
                                   nullptr);
        }

        // The texture upload is recorded before the render pass begins. A buffer-to-image copy and
        // its layout barriers cannot be issued inside a subpass, so doing it there is a validation
        // error and the image would not be in a sampled layout when the shader reads it.
        if (textured_mode) {
            const auto& sub = textured->subresources.front();
            VkImageMemoryBarrier to_dst{};
            to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_dst.image = texture_image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                 &to_dst);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {sub.width, sub.height, 1};
            vkCmdCopyBufferToImage(cmd, staging, texture_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            VkImageMemoryBarrier to_shader = to_dst;
            to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &to_shader);
        }

        rp_begin.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        // A descriptor set is bound when the pipeline layout declares one, which in effective mode
        // is whenever the modules declare any binding. Diagnostic texturing is a separate concern.
        const bool bind_descriptors =
            shader_mode_effective ? !effective_shaders.bindings.empty() : textured_mode;
        std::vector<u8> push_bytes;
        u32 push_offset = 0;
        u32 push_size = 0;
        u32 push_stages = 0;
        if (shader_mode_effective && effective_shaders.push_constant_size != 0) {
            push_bytes = effective_shaders.push_constant_bytes;
            push_offset = effective_shaders.push_constant_offset;
            push_size = effective_shaders.push_constant_size;
            push_stages = effective_shaders.push_constant_stages;
        }
        VulkanSink sink{ctx, cmd, pipeline, pipeline_layout, vertex_buffer, index_buffer,
                        index_width, mvp, descriptor_set, textured_mode, bind_descriptors,
                        std::move(push_bytes), push_offset, push_size, push_stages};
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
                // The image identity is what a mutation test compares. A hash alone would not
                // distinguish "the draw changed" from "the same draw rendered the same way", so
                // the distinct-colour count and the covered-pixel count are reported too.
                u64 image_hash = 0xcbf29ce484222325ull;
                u32 covered = 0;
                std::vector<u32> seen;
                seen.reserve(4096);
                for (u32 i = 0; i < kWidth * kHeight; ++i) {
                    const u8* pixel = src + i * 4;
                    for (u32 c = 0; c < 4; ++c) {
                        image_hash ^= pixel[c];
                        image_hash *= 0x100000001b3ull;
                    }
                    const u32 packed = (static_cast<u32>(pixel[0]) << 16) |
                                       (static_cast<u32>(pixel[1]) << 8) |
                                       static_cast<u32>(pixel[2]);
                    // The clear colour is the background; anything else is covered.
                    if (packed != 0x0D0D14u) {
                        ++covered;
                    }
                    if (std::find(seen.begin(), seen.end(), packed) == seen.end()) {
                        if (seen.size() < 4096) {
                            seen.push_back(packed);
                        }
                    }
                }
                std::cout << "image: hash=" << image_hash << " distinct=" << seen.size()
                          << " covered=" << covered << "\n";
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
    // What the replay actually did, so a partial execution is never presented as a complete one.
    std::cout << "mode: " << (textured_mode ? "textured" : "position_only") << "\n";
    if (textured_mode) {
        const auto& sub = textured->subresources.front();
        std::cout << "texture: id=" << textured->id << " binding=" << textured->vk_binding << " "
                  << sub.width << "x" << sub.height << " texels, " << textured->block_columns
                  << "x" << textured->block_rows << " blocks, " << textured->bytes_per_block
                  << " bytes/block, format=" << textured->image_format
                  << " bytes=" << sub.byte_length << " transfer=" << textured->transfer_id << "\n";
        std::cout << "texture: payload hash=" << sub.content_hash << "\n";
        std::cout << "sampler: id=" << sampler->id << " (verified association)"
                  << " mag=" << sampler->mag_filter << " min=" << sampler->min_filter
                  << " mip=" << sampler->mip_filter << " aniso=" << sampler->max_anisotropy
                  << " lod=[" << sampler->min_lod << "," << sampler->max_lod << "]\n";
        std::cout << "uv: binding=" << pkg.uv.binding << " components=" << pkg.uv.component_u << ","
                  << pkg.uv.component_v << "\n";
    } else {
        std::cout << "texture: none (the package carries no captured texture with a verified "
                     "sampler and a usable UV attribute)\n";
    }
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
    // The pipeline-state fields the package did not capture, so a reader can tell captured state
    // from substituted state without reading the source.
    if (!substituted_state.empty()) {
        std::cout << "pipeline_state_substituted:";
        for (const char* name : substituted_state) {
            std::cout << " " << name;
        }
        std::cout << "\n";
    } else if (shader_mode_effective) {
        std::cout << "pipeline_state_substituted: none\n";
    }

    // Validation is reported as a count. Claiming success from the absence of a device loss would
    // not be the same claim.
    if (want_validation) {
        std::cout << "validation: errors=" << g_validation_errors.load(std::memory_order_relaxed)
                  << "\n";
    }

    std::cout << "\n-- provenance separation --\n";
    std::cout << "provenance: " << (synthetic_fixture ? "synthetic_fixture" : "original_tape")
              << "\n";
    std::cout << "shader_mode: " << (shader_mode_effective ? "effective" : "diagnostic") << "\n";
    std::cout << "semantic_commands: recovered from the UE4 deferred command record\n";
    std::cout << "resource_contents: supplied by the SC-09H pkg (shadPS4-exported)\n";
    if (shader_mode_effective) {
        std::cout << "shaders: the package's own effective modules, "
                  << effective_shaders.stages.size() << " stages\n";
        std::cout << "shaders: these are captured-style modules from a synthetic fixture, not a "
                     "verified original-game capture\n";
    } else {
        std::cout << "shaders: diagnostic substitute, not the game's\n";
    }
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
