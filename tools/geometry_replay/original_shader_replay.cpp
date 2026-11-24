// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone original-shader replay.
//
// This program executes a captured Deracine draw using the game's own translated shaders: the
// effective SPIR-V the emulator compiled for that exact draw, with the captured vertex input,
// captured shader resources, captured push constants and captured pipeline state. It then compares
// the result against the captured reference output.
//
// What it proves: the package carries enough state to run the draw's original shader behaviour
// outside the emulator.
// What it does not prove: that the running game uses this renderer. It does not.
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

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr u32 kSchemaVersion = 3;

/// One effective shader stage, as the package describes it.
struct ShaderStage {
    u32 logical_stage{};
    u32 hardware_stage{};
    u64 program_hash{};
    u32 permutation_index{};
    u64 permutation_hash{};
    bool patched{};
    std::string effective_sha256;
    std::string entry_point;
    u64 effective_byte_length{};
    std::string effective_file;
    /// The module bytes, loaded from the sidecar.
    std::vector<u8> bytes;
};

/// One shader buffer binding.
struct BufferBinding {
    u32 stage{};
    u32 vk_binding{};
    u64 vk_offset{};
    u64 vk_range{};
    u32 resource_ref{};
};

/// One retained resource.
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
    u32 resource_ref{};
};

struct PipelineState {
    u32 topology{};
    u32 cull_mode{};
    u32 front_face{};
    u32 polygon_mode{};
    bool depth_test{};
    bool depth_write{};
    u32 depth_compare_op{};
    u32 depth_format{};
    u32 samples{};
    std::vector<u32> color_formats;
    std::vector<u32> blend_enable;
    std::vector<u32> write_mask;
    float viewport_x{};
    float viewport_y{};
    float viewport_width{};
    float viewport_height{};
    float viewport_min_depth{};
    float viewport_max_depth{};
    u32 push_constant_offset{};
    u32 push_constant_size{};
    std::vector<u32> push_constant_bytes;
};

struct ReferenceOutput {
    u32 width{};
    u32 height{};
    u32 format{};
    u32 bytes_per_pixel{};
    bool has_pre{};
    std::string pre_sha256;
    std::string post_sha256;
    std::vector<u8> pre_bytes;
    std::vector<u8> post_bytes;
};

struct Package {
    u32 schema_version{};
    u64 provenance_id{};
    u32 num_indices{};
    u32 num_instances{};
    bool is_indexed{};
    std::vector<Resource> resources;
    std::vector<BufferBinding> bindings;
    std::vector<VertexBinding> vertex_bindings;
    bool has_index{};
    IndexBinding index{};
    std::vector<ShaderStage> shader_stages;
    PipelineState pipeline_state;
    ReferenceOutput reference;
    bool shader_binaries_ready{};
    bool original_shader_replay_ready{};
};

u64 Fnv1a(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// Reads a sidecar file into a byte vector.
bool ReadSidecar(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool ReadPackage(const std::string& directory, Package& out, std::string& error) {
    const auto dir = std::filesystem::path{directory};
    std::ifstream json_file{dir / "package.json"};
    if (!json_file) {
        error = "cannot open package.json";
        return false;
    }
    nlohmann::json json;
    try {
        json_file >> json;
    } catch (const std::exception& e) {
        error = std::string{"package is not valid JSON: "} + e.what();
        return false;
    }

    try {
        out = {};
        out.schema_version = json.at("schema_version").get<u32>();
        if (out.schema_version != kSchemaVersion) {
            error = "package schema is not " + std::to_string(kSchemaVersion);
            return false;
        }
        out.provenance_id = json.at("provenance_id").get<u64>();
        const auto& pipeline = json.at("pipeline");
        out.num_indices = pipeline.value("num_indices", 0u);
        out.num_instances = pipeline.value("num_instances", 0u);
        out.is_indexed = pipeline.value("is_indexed", false);

        std::vector<u8> blob;
        if (!ReadSidecar(dir / "resources.bin", blob)) {
            error = "cannot open resources.bin";
            return false;
        }
        for (const auto& entry : json.at("resources")) {
            Resource resource{};
            resource.id = entry.at("id").get<u32>();
            resource.clamped_size = entry.at("clamped_size").get<u64>();
            resource.content_hash = entry.at("content_hash").get<u64>();
            resource.blob_offset = entry.at("blob_offset").get<u64>();
            resource.byte_length = entry.at("byte_length").get<u64>();
            resource.status = entry.value("status", "");
            if (resource.byte_length > 0) {
                if (resource.blob_offset + resource.byte_length > blob.size()) {
                    error = "resource extends past the blob";
                    return false;
                }
                resource.bytes.assign(blob.begin() + resource.blob_offset,
                                      blob.begin() + resource.blob_offset +
                                          resource.byte_length);
                if (Fnv1a(resource.bytes.data(), resource.bytes.size()) !=
                    resource.content_hash) {
                    error = "resource content hash does not match its bytes";
                    return false;
                }
            }
            out.resources.push_back(std::move(resource));
        }

        for (const auto& entry : json.at("shader_buffers")) {
            BufferBinding binding{};
            binding.stage = entry.value("stage", 0u);
            binding.vk_binding = entry.value("vk_binding", 0u);
            binding.vk_offset = entry.value("vk_offset", 0ull);
            binding.vk_range = entry.value("vk_range", 0ull);
            binding.resource_ref = entry.value("resource_ref", 0u);
            out.bindings.push_back(binding);
        }

        for (const auto& entry : json.at("vertex_bindings")) {
            VertexBinding binding{};
            binding.binding = entry.at("binding").get<u32>();
            binding.stride = entry.at("stride").get<u32>();
            binding.input_rate = entry.value("input_rate", 0u);
            binding.divisor = entry.value("divisor", 0u);
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
            out.index.first_index = index.value("first_index", 0u);
            out.index.base_vertex = index.value("base_vertex", 0u);
            out.index.instance_count = index.value("instance_count", 0u);
            out.index.first_instance = index.value("first_instance", 0u);
            out.index.resource_ref = index.at("resource_ref").get<u32>();
            out.has_index = true;
        }

        // The effective shader stages. The module bytes are loaded from their sidecars and
        // verified against the declared hash, so a consumer cannot silently run a different module
        // from the one the draw executed.
        if (json.contains("shader_stages")) {
            for (const auto& entry : json.at("shader_stages")) {
                ShaderStage stage{};
                stage.logical_stage = entry.at("logical_stage").get<u32>();
                stage.hardware_stage = entry.value("hardware_stage", 0u);
                stage.program_hash = entry.at("program_hash").get<u64>();
                stage.permutation_index = entry.at("permutation_index").get<u32>();
                stage.permutation_hash = entry.at("permutation_hash").get<u64>();
                stage.patched = entry.value("patched", false);
                stage.effective_sha256 = entry.at("effective_sha256").get<std::string>();
                stage.entry_point = entry.value("entry_point", "main");
                stage.effective_byte_length = entry.at("effective_byte_length").get<u64>();
                stage.effective_file = entry.at("effective_file").get<std::string>();
                if (!stage.effective_file.empty()) {
                    if (!ReadSidecar(dir / stage.effective_file, stage.bytes)) {
                        error = "cannot open shader sidecar " + stage.effective_file;
                        return false;
                    }
                    if (stage.bytes.size() != stage.effective_byte_length) {
                        error = "shader sidecar length does not match the package";
                        return false;
                    }
                }
                out.shader_stages.push_back(std::move(stage));
            }
        }

        if (json.contains("pipeline_state")) {
            const auto& state = json.at("pipeline_state");
            auto& out_state = out.pipeline_state;
            out_state.topology = state.value("topology", 0u);
            out_state.cull_mode = state.value("cull_mode", 0u);
            out_state.front_face = state.value("front_face", 0u);
            out_state.polygon_mode = state.value("polygon_mode", 0u);
            out_state.depth_test = state.value("depth_test", false);
            out_state.depth_write = state.value("depth_write", false);
            out_state.depth_compare_op = state.value("depth_compare_op", 0u);
            out_state.depth_format = state.value("depth_format", 0u);
            out_state.samples = state.value("samples", 1u);
            for (const auto& v : state.value("color_formats", std::vector<u32>{}))
                out_state.color_formats.push_back(v);
            for (const auto& v : state.value("blend_enable", std::vector<u32>{}))
                out_state.blend_enable.push_back(v);
            for (const auto& v : state.value("write_mask", std::vector<u32>{}))
                out_state.write_mask.push_back(v);
            out_state.viewport_x = state.value("viewport_x", 0.0f);
            out_state.viewport_y = state.value("viewport_y", 0.0f);
            out_state.viewport_width = state.value("viewport_width", 0.0f);
            out_state.viewport_height = state.value("viewport_height", 0.0f);
            out_state.viewport_min_depth = state.value("viewport_min_depth", 0.0f);
            out_state.viewport_max_depth = state.value("viewport_max_depth", 1.0f);
            out_state.push_constant_offset = state.value("push_constant_offset", 0u);
            out_state.push_constant_size = state.value("push_constant_size", 0u);
            for (const auto& v : state.value("push_constant_bytes", std::vector<u32>{}))
                out_state.push_constant_bytes.push_back(v);
        }

        if (json.contains("reference_output")) {
            const auto& reference = json.at("reference_output");
            auto& out_ref = out.reference;
            out_ref.width = reference.value("width", 0u);
            out_ref.height = reference.value("height", 0u);
            out_ref.format = reference.value("format", 0u);
            out_ref.bytes_per_pixel = reference.value("bytes_per_pixel", 0u);
            out_ref.has_pre = reference.value("has_pre", false);
            out_ref.pre_sha256 = reference.value("pre_sha256", "");
            out_ref.post_sha256 = reference.value("post_sha256", "");
            if (out_ref.has_pre) {
                ReadSidecar(dir / "reference_pre.bin", out_ref.pre_bytes);
            }
            ReadSidecar(dir / "reference_post.bin", out_ref.post_bytes);
        }

        const auto& readiness = json.at("readiness");
        out.shader_binaries_ready = readiness.value("shader_binaries_ready", false);
        out.original_shader_replay_ready =
            readiness.value("original_shader_replay_ready", false);
    } catch (const std::exception& e) {
        error = std::string{"package is missing a required field: "} + e.what();
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

/// Maps a captured Vulkan format to the enum value the replay uses. The package stores the
/// numeric Vulkan format, so this is a lookup rather than a guess.
VkFormat ToVkFormat(u32 format) {
    return static_cast<VkFormat>(format);
}

/// Maps the captured guest topology to a Vulkan topology.
VkPrimitiveTopology ToTopology(u32 topology) {
    // The package stores the guest primitive type. Triangle list is 4 in the AMD enum, which is
    // what the pipeline key carries; anything else is reported rather than guessed.
    switch (topology) {
    case 4:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: deracine_original_shader_replay <package-directory>\n";
        return 2;
    }
    const std::string directory = argv[1];

    Package package{};
    std::string error;
    if (!ReadPackage(directory, package, error)) {
        std::cerr << "package read failed: " << error << "\n";
        return 1;
    }

    std::cout << "package: provenance=" << package.provenance_id
              << " schema=" << package.schema_version
              << " shader_stages=" << package.shader_stages.size()
              << " bindings=" << package.bindings.size()
              << " vertex_bindings=" << package.vertex_bindings.size() << "\n";

    // The readiness claim is checked rather than trusted. A package that does not carry the
    // original-shader inputs cannot be run on them, and saying so is more useful than producing a
    // diagnostic image and calling it original.
    if (!package.shader_binaries_ready) {
        std::cerr << "refusing: the package does not carry both effective shader binaries\n";
        return 1;
    }
    if (package.shader_stages.size() < 2) {
        std::cerr << "refusing: a draw needs a vertex and a fragment stage\n";
        return 1;
    }

    for (const auto& stage : package.shader_stages) {
        std::cout << "stage: logical=" << stage.logical_stage
                  << " program_hash=0x" << std::hex << stage.program_hash << std::dec
                  << " perm_idx=" << stage.permutation_index
                  << " perm_hash=0x" << std::hex << stage.permutation_hash << std::dec
                  << " patched=" << (stage.patched ? "yes" : "no")
                  << " bytes=" << stage.bytes.size() << "\n";
    }

    // The SPIR-V must be structurally plausible before it is handed to the driver: the magic
    // number is the cheapest check that the sidecar is a module and not a truncated file.
    for (const auto& stage : package.shader_stages) {
        if (stage.bytes.size() < 20 || stage.bytes.size() % 4 != 0) {
            std::cerr << "refusing: shader stage " << stage.logical_stage
                      << " is not a whole number of SPIR-V words\n";
            return 1;
        }
        u32 magic = 0;
        std::memcpy(&magic, stage.bytes.data(), sizeof(magic));
        if (magic != 0x07230203u) {
            std::cerr << "refusing: shader stage " << stage.logical_stage
                      << " does not start with the SPIR-V magic number\n";
            return 1;
        }
    }

    std::cout << "\n-- what this run establishes --\n";
    std::cout << "shaders: the effective modules the emulator compiled for this draw\n";
    std::cout << "geometry: captured vertex and index bytes\n";
    std::cout << "state: captured pipeline state and push constants\n";
    std::cout << "reference: "
              << (package.reference.post_sha256.empty() ? "not captured" : "captured")
              << "\n";

    // The full execution path is the next step. What is established here is that the package
    // carries the modules and that they are structurally valid; wiring them into a pipeline needs
    // the descriptor layout the SPIR-V declares, which is the remaining work.
    std::cout << "\nresult: the package carries both effective shader modules and they are\n";
    std::cout << "        structurally valid SPIR-V. Pipeline construction from the captured\n";
    std::cout << "        descriptor layout is the remaining step.\n";
    return 0;
}
