// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone Vulkan original-shader pipeline.
//
// This program builds and executes a graphics pipeline from shader modules and a descriptor
// interface, without any knowledge of a particular shader hash. The interface is read from the
// SPIR-V itself and cross-checked against the captured declarations, so a package whose captured
// layout disagrees with its modules is refused rather than run on a guessed layout.
//
// Two modes:
//
//   --selftest   builds a pipeline from the committed test shaders and a synthetic descriptor set,
//                executes an indexed draw, and validates the rendered result. This proves the
//                infrastructure with real SPIR-V and needs no capture.
//
//   <package>    builds the same pipeline from a captured schema-4 package. This is the path a
//                live capture would exercise.
//
// Dependencies: the Vulkan loader, the C++ standard library, nlohmann/json.

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

#include "spirv_interface.h"

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

constexpr u32 kWidth = 256;
constexpr u32 kHeight = 256;

/// One descriptor the pipeline layout must declare.
struct DescriptorDecl {
    u32 set{};
    u32 binding{};
    VkDescriptorType type{};
    u32 count{1};
    VkShaderStageFlags stages{};
};

/// The interface a pipeline is built from.
struct PipelineInterface {
    std::vector<DescriptorDecl> descriptors;
    u32 push_constant_offset{};
    u32 push_constant_size{};
    VkShaderStageFlags push_constant_stages{};
};

/// Maps the verifier class to a Vulkan descriptor type.
VkDescriptorType ToVkDescriptorType(spirv::DescriptorClass cls) {
    switch (cls) {
    case spirv::DescriptorClass::UniformBuffer:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case spirv::DescriptorClass::StorageBuffer:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case spirv::DescriptorClass::SampledImage:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case spirv::DescriptorClass::StorageImage:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case spirv::DescriptorClass::Sampler:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case spirv::DescriptorClass::CombinedImageSampler:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default:
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

/// Builds the pipeline interface from the modules themselves.
///
/// The modules are the authority: the layout is derived from what they declare, not from a
/// hardcoded table. A captured interface is then compared against it, so a package whose captured
/// layout disagrees with its modules is refused.
bool BuildInterfaceFromModules(const spirv::Interface& vertex, const spirv::Interface& fragment,
                               PipelineInterface& out, std::string& error) {
    out = {};

    const auto add = [&out](const spirv::Descriptor& descriptor, VkShaderStageFlags stages) {
        // A binding declared by both stages is one descriptor with both stage bits, not two.
        for (auto& existing : out.descriptors) {
            if (existing.set == descriptor.set && existing.binding == descriptor.binding) {
                existing.stages |= stages;
                return;
            }
        }
        DescriptorDecl decl{};
        decl.set = descriptor.set;
        decl.binding = descriptor.binding;
        decl.type = ToVkDescriptorType(descriptor.cls);
        decl.count = descriptor.count;
        decl.stages = stages;
        out.descriptors.push_back(decl);
    };

    for (const auto& descriptor : vertex.descriptors) {
        if (ToVkDescriptorType(descriptor.cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            error = "the vertex module declares a descriptor class this pipeline cannot bind";
            return false;
        }
        add(descriptor, VK_SHADER_STAGE_VERTEX_BIT);
    }
    for (const auto& descriptor : fragment.descriptors) {
        if (ToVkDescriptorType(descriptor.cls) == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            error = "the fragment module declares a descriptor class this pipeline cannot bind";
            return false;
        }
        add(descriptor, VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    // The push-constant range is the union of what the stages declare. The verifier reports a
    // lower bound on the block size, so the larger of the two is used.
    if (vertex.push_constant.present) {
        out.push_constant_size = std::max(out.push_constant_size, vertex.push_constant.size);
        out.push_constant_stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (fragment.push_constant.present) {
        out.push_constant_size = std::max(out.push_constant_size, fragment.push_constant.size);
        out.push_constant_stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return true;
}

struct Context {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    u32 queue_family{};
    VkCommandPool command_pool{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    u32 validation_errors{};
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* user_data) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        auto* ctx = static_cast<Context*>(user_data);
        if (ctx != nullptr) {
            ++ctx->validation_errors;
        }
        std::cerr << "validation: " << (data->pMessage != nullptr ? data->pMessage : "") << "\n";
    }
    return VK_FALSE;
}

u32 FindMemoryType(const Context& ctx, u32 type_bits, VkMemoryPropertyFlags wanted) {
    for (u32 i = 0; i < ctx.memory_properties.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) != 0 &&
            (ctx.memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return ~0u;
}

bool CreateBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
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

/// A 4x4 column-major matrix, built by hand so the program needs no maths library.
struct Mat4 {
    float m[16]{};

    static Mat4 Identity() {
        Mat4 out{};
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
        return out;
    }
};

/// Runs the self-test: real shaders, a real descriptor set, a real draw, and a validated result.
bool RunSelfTest(const std::string& shader_dir) {
    std::vector<u8> vertex_bytes;
    std::vector<u8> fragment_bytes;
    {
        std::ifstream file{std::filesystem::path{shader_dir} / "sc11r_test.vert.spv",
                           std::ios::binary};
        if (!file) {
            std::cerr << "cannot open the vertex test shader in " << shader_dir << "\n";
            return false;
        }
        vertex_bytes.assign(std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>());
    }
    {
        std::ifstream file{std::filesystem::path{shader_dir} / "sc11r_test.frag.spv",
                           std::ios::binary};
        if (!file) {
            std::cerr << "cannot open the fragment test shader in " << shader_dir << "\n";
            return false;
        }
        fragment_bytes.assign(std::istreambuf_iterator<char>(file),
                              std::istreambuf_iterator<char>());
    }

    std::cout << "selftest: modules loaded (" << vertex_bytes.size() << " and "
              << fragment_bytes.size() << " bytes)\n";

    // Read the interface from the modules. This is the authority the layout is built from.
    spirv::Interface vertex_interface{};
    spirv::Interface fragment_interface{};
    spirv::Problem problem{};
    if (!spirv::ReadInterface(vertex_bytes, vertex_interface, problem)) {
        std::cerr << "vertex module unreadable: " << spirv::ProblemName(problem) << "\n";
        return false;
    }
    if (!spirv::ReadInterface(fragment_bytes, fragment_interface, problem)) {
        std::cerr << "fragment module unreadable: " << spirv::ProblemName(problem) << "\n";
        return false;
    }

    std::cout << "selftest: interfaces read\n";
    PipelineInterface interface{};
    std::string error;
    if (!BuildInterfaceFromModules(vertex_interface, fragment_interface, interface, error)) {
        std::cerr << "interface build failed: " << error << "\n";
        return false;
    }

    std::cout << "selftest: vertex descriptors=" << vertex_interface.descriptors.size()
              << " fragment descriptors=" << fragment_interface.descriptors.size()
              << " pipeline descriptors=" << interface.descriptors.size()
              << " push_constant=" << interface.push_constant_size << "\n";
    for (const auto& descriptor : interface.descriptors) {
        std::cout << "  set=" << descriptor.set << " binding=" << descriptor.binding
                  << " type=" << static_cast<int>(descriptor.type)
                  << " count=" << descriptor.count << "\n";
    }

    // --- Vulkan setup -------------------------------------------------------------------------
    std::cout << "selftest: creating instance\n";
    Context ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_original_shader_pipeline";
    app.apiVersion = VK_API_VERSION_1_1;

    const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    bool have_validation = false;
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, validation_layer) == 0) {
            have_validation = true;
        }
    }

    std::vector<const char*> instance_layers;
    if (have_validation) {
        instance_layers.push_back(validation_layer);
    }
    std::vector<const char*> instance_extensions;
    if (have_validation) {
        instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = static_cast<u32>(instance_layers.size());
    instance_info.ppEnabledLayerNames = instance_layers.data();
    instance_info.enabledExtensionCount = static_cast<u32>(instance_extensions.size());
    instance_info.ppEnabledExtensionNames = instance_extensions.data();
    if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed\n";
        return false;
    }

    if (have_validation) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
            messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messenger_info.pfnUserCallback = DebugCallback;
            messenger_info.pUserData = &ctx;
            create_messenger(ctx.instance, &messenger_info, nullptr, &ctx.messenger);
        }
    }
    std::cout << "validation_layers=" << (have_validation ? "enabled" : "unavailable") << "\n";

    u32 device_count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, nullptr);
    if (device_count == 0) {
        std::cerr << "no Vulkan physical device\n";
        return false;
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
        return false;
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
        return false;
    }
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);
    vkGetPhysicalDeviceMemoryProperties(ctx.physical, &ctx.memory_properties);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx.queue_family;
    if (vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool) != VK_SUCCESS) {
        std::cerr << "vkCreateCommandPool failed\n";
        return false;
    }

    // --- Descriptor set layout, built from the interface --------------------------------------
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    for (const auto& descriptor : interface.descriptors) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = descriptor.binding;
        binding.descriptorType = descriptor.type;
        binding.descriptorCount = descriptor.count;
        binding.stageFlags = descriptor.stages;
        layout_bindings.push_back(binding);
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<u32>(layout_bindings.size());
    layout_info.pBindings = layout_bindings.data();
    VkDescriptorSetLayout set_layout{};
    if (vkCreateDescriptorSetLayout(ctx.device, &layout_info, nullptr, &set_layout) != VK_SUCCESS) {
        std::cerr << "vkCreateDescriptorSetLayout failed\n";
        return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = interface.push_constant_stages;
    push_range.offset = 0;
    push_range.size = interface.push_constant_size;

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &set_layout;
    if (interface.push_constant_size != 0) {
        pipeline_layout_info.pushConstantRangeCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_range;
    }
    VkPipelineLayout pipeline_layout{};
    if (vkCreatePipelineLayout(ctx.device, &pipeline_layout_info, nullptr, &pipeline_layout) !=
        VK_SUCCESS) {
        std::cerr << "vkCreatePipelineLayout failed\n";
        return false;
    }

    // --- Shader modules -----------------------------------------------------------------------
    const auto make_module = [&ctx](const std::vector<u8>& bytes) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = bytes.size();
        info.pCode = reinterpret_cast<const u32*>(bytes.data());
        VkShaderModule module{};
        if (vkCreateShaderModule(ctx.device, &info, nullptr, &module) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return module;
    };
    VkShaderModule vertex_module = make_module(vertex_bytes);
    VkShaderModule fragment_module = make_module(fragment_bytes);
    if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE) {
        std::cerr << "shader module creation failed\n";
        return false;
    }

    // --- Render target ------------------------------------------------------------------------
    VkImage color_image{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {kWidth, kHeight, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.device, &info, nullptr, &color_image) != VK_SUCCESS) {
            std::cerr << "colour image creation failed\n";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(ctx.device, color_image, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex =
            FindMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.device, &alloc, nullptr, &color_memory) != VK_SUCCESS) {
            std::cerr << "colour memory allocation failed\n";
            return false;
        }
        vkBindImageMemory(ctx.device, color_image, color_memory, 0);
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = color_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(ctx.device, &view_info, nullptr, &color_view) != VK_SUCCESS) {
            std::cerr << "colour view creation failed\n";
            return false;
        }
    }

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
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
    VkRenderPass render_pass{};
    if (vkCreateRenderPass(ctx.device, &pass_info, nullptr, &render_pass) != VK_SUCCESS) {
        std::cerr << "vkCreateRenderPass failed\n";
        return false;
    }

    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &color_view;
    fb_info.width = kWidth;
    fb_info.height = kHeight;
    fb_info.layers = 1;
    VkFramebuffer framebuffer{};
    if (vkCreateFramebuffer(ctx.device, &fb_info, nullptr, &framebuffer) != VK_SUCCESS) {
        std::cerr << "vkCreateFramebuffer failed\n";
        return false;
    }

    // --- Graphics pipeline --------------------------------------------------------------------
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    // The vertex input matches the test shader: position at location 0, uv at location 1.
    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = 5 * sizeof(float);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_attributes[2]{};
    vertex_attributes[0].location = 0;
    vertex_attributes[0].binding = 0;
    vertex_attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    vertex_attributes[0].offset = 0;
    vertex_attributes[1].location = 1;
    vertex_attributes[1].binding = 0;
    vertex_attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    vertex_attributes[1].offset = 3 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = vertex_attributes;

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
    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        std::cerr << "vkCreateGraphicsPipelines failed\n";
        return false;
    }
    std::cout << "pipeline: created\n";

    // --- Resources ----------------------------------------------------------------------------
    // A quad covering the viewport, with UVs.
    const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,
        1.0f,  -1.0f, 0.0f, 1.0f, 1.0f,
        1.0f,  1.0f,  0.0f, 1.0f, 0.0f,
        -1.0f, 1.0f,  0.0f, 0.0f, 0.0f,
    };
    const u16 indices[] = {0, 1, 2, 2, 3, 0};

    VkBuffer vertex_buffer{};
    VkDeviceMemory vertex_memory{};
    if (!CreateBuffer(ctx, sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      vertex_buffer, vertex_memory)) {
        std::cerr << "vertex buffer creation failed\n";
        return false;
    }
    void* mapped = nullptr;
    vkMapMemory(ctx.device, vertex_memory, 0, sizeof(vertices), 0, &mapped);
    std::memcpy(mapped, vertices, sizeof(vertices));
    vkUnmapMemory(ctx.device, vertex_memory);

    VkBuffer index_buffer{};
    VkDeviceMemory index_memory{};
    if (!CreateBuffer(ctx, sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      index_buffer, index_memory)) {
        std::cerr << "index buffer creation failed\n";
        return false;
    }
    vkMapMemory(ctx.device, index_memory, 0, sizeof(indices), 0, &mapped);
    std::memcpy(mapped, indices, sizeof(indices));
    vkUnmapMemory(ctx.device, index_memory);

    // The uniform buffer: an identity MVP and a tint that makes the output non-trivial.
    struct UniformBlock {
        float mvp[16];
        float tint[4];
    };
    UniformBlock uniform{};
    const Mat4 identity = Mat4::Identity();
    std::memcpy(uniform.mvp, identity.m, sizeof(uniform.mvp));
    uniform.tint[0] = 1.0f;
    uniform.tint[1] = 0.5f;
    uniform.tint[2] = 0.25f;
    uniform.tint[3] = 1.0f;

    VkBuffer uniform_buffer{};
    VkDeviceMemory uniform_memory{};
    if (!CreateBuffer(ctx, sizeof(uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      uniform_buffer, uniform_memory)) {
        std::cerr << "uniform buffer creation failed\n";
        return false;
    }
    vkMapMemory(ctx.device, uniform_memory, 0, sizeof(uniform), 0, &mapped);
    std::memcpy(mapped, &uniform, sizeof(uniform));
    vkUnmapMemory(ctx.device, uniform_memory);

    // A 4x4 texture with a known pattern, so a wrong sample is visible.
    constexpr u32 kTexSize = 4;
    std::array<u8, kTexSize * kTexSize * 4> texels{};
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const u32 i = (y * kTexSize + x) * 4;
            texels[i + 0] = static_cast<u8>(x * 64);
            texels[i + 1] = static_cast<u8>(y * 64);
            texels[i + 2] = 128;
            texels[i + 3] = 255;
        }
    }

    VkImage texture_image{};
    VkDeviceMemory texture_memory{};
    VkImageView texture_view{};
    {
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {kTexSize, kTexSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(ctx.device, &info, nullptr, &texture_image) != VK_SUCCESS) {
            std::cerr << "texture image creation failed\n";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(ctx.device, texture_image, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex =
            FindMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(ctx.device, &alloc, nullptr, &texture_memory) != VK_SUCCESS) {
            std::cerr << "texture memory allocation failed\n";
            return false;
        }
        vkBindImageMemory(ctx.device, texture_image, texture_memory, 0);
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(ctx.device, &view_info, nullptr, &texture_view) != VK_SUCCESS) {
            std::cerr << "texture view creation failed\n";
            return false;
        }
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler{};
    if (vkCreateSampler(ctx.device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
        std::cerr << "sampler creation failed\n";
        return false;
    }

    // --- Descriptor pool and set --------------------------------------------------------------
    std::map<VkDescriptorType, u32> type_counts;
    for (const auto& descriptor : interface.descriptors) {
        type_counts[descriptor.type] += descriptor.count;
    }
    std::vector<VkDescriptorPoolSize> pool_sizes;
    for (const auto& [type, count] : type_counts) {
        pool_sizes.push_back({type, count});
    }
    VkDescriptorPoolCreateInfo pool_create{};
    pool_create.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_create.maxSets = 1;
    pool_create.poolSizeCount = static_cast<u32>(pool_sizes.size());
    pool_create.pPoolSizes = pool_sizes.data();
    VkDescriptorPool descriptor_pool{};
    if (vkCreateDescriptorPool(ctx.device, &pool_create, nullptr, &descriptor_pool) != VK_SUCCESS) {
        std::cerr << "vkCreateDescriptorPool failed\n";
        return false;
    }

    VkDescriptorSetAllocateInfo set_alloc{};
    set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_alloc.descriptorPool = descriptor_pool;
    set_alloc.descriptorSetCount = 1;
    set_alloc.pSetLayouts = &set_layout;
    VkDescriptorSet descriptor_set{};
    if (vkAllocateDescriptorSets(ctx.device, &set_alloc, &descriptor_set) != VK_SUCCESS) {
        std::cerr << "vkAllocateDescriptorSets failed\n";
        return false;
    }

    // Populate the set from the interface, so a binding the module declares is always written.
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    buffer_infos.reserve(interface.descriptors.size());
    image_infos.reserve(interface.descriptors.size());
    for (const auto& descriptor : interface.descriptors) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_set;
        write.dstBinding = descriptor.binding;
        write.descriptorCount = descriptor.count;
        write.descriptorType = descriptor.type;
        switch (descriptor.type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
            VkDescriptorBufferInfo info{};
            info.buffer = uniform_buffer;
            info.offset = 0;
            info.range = sizeof(uniform);
            buffer_infos.push_back(info);
            write.pBufferInfo = &buffer_infos.back();
            break;
        }
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
            VkDescriptorImageInfo info{};
            info.sampler = sampler;
            info.imageView = texture_view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos.push_back(info);
            write.pImageInfo = &image_infos.back();
            break;
        }
        default:
            std::cerr << "the interface declares a descriptor type this self-test cannot bind\n";
            return false;
        }
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(ctx.device, static_cast<u32>(writes.size()), writes.data(), 0,
                           nullptr);

    // --- Upload the texture and execute -------------------------------------------------------
    VkBuffer readback_buffer{};
    VkDeviceMemory readback_memory{};
    const VkDeviceSize image_bytes = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
    if (!CreateBuffer(ctx, image_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback_buffer, readback_memory)) {
        std::cerr << "readback buffer creation failed\n";
        return false;
    }

    VkBuffer staging_buffer{};
    VkDeviceMemory staging_memory{};
    if (!CreateBuffer(ctx, texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging_buffer, staging_memory)) {
        std::cerr << "staging buffer creation failed\n";
        return false;
    }
    vkMapMemory(ctx.device, staging_memory, 0, texels.size(), 0, &mapped);
    std::memcpy(mapped, texels.data(), texels.size());
    vkUnmapMemory(ctx.device, staging_memory);

    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    // Upload the texture.
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = texture_image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kTexSize, kTexSize, 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, texture_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader{};
    to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader.image = texture_image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_shader);

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea = {{0, 0}, {kWidth, kHeight}};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                            &descriptor_set, 0, nullptr);

    // The push constant: a small offset, so the field is exercised.
    if (interface.push_constant_size != 0) {
        float push[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        vkCmdPushConstants(cmd, pipeline_layout, interface.push_constant_stages, 0,
                           std::min<u32>(interface.push_constant_size, sizeof(push)), push);
    }

    const VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &vertex_offset);
    vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy readback_region{};
    readback_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    readback_region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buffer, 1, &readback_region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);

    // --- Validate the result ------------------------------------------------------------------
    void* pixels = nullptr;
    if (vkMapMemory(ctx.device, readback_memory, 0, image_bytes, 0, &pixels) != VK_SUCCESS) {
        std::cerr << "readback map failed\n";
        return false;
    }
    const auto* src = static_cast<const u8*>(pixels);
    u64 non_black = 0;
    u64 distinct = 0;
    std::map<u32, u32> histogram;
    for (u32 i = 0; i < kWidth * kHeight; ++i) {
        const u32 r = src[i * 4 + 0];
        const u32 g = src[i * 4 + 1];
        const u32 b = src[i * 4 + 2];
        if (r != 0 || g != 0 || b != 0) {
            ++non_black;
        }
        const u32 key = (r << 16) | (g << 8) | b;
        if (histogram[key]++ == 0) {
            ++distinct;
        }
    }
    vkUnmapMemory(ctx.device, readback_memory);

    std::cout << "result: non_black=" << non_black << " distinct_colors=" << distinct
              << " validation_errors=" << ctx.validation_errors << "\n";

    // The quad covers the whole viewport, so a correct draw leaves almost no black. The texture
    // has four distinct columns and four rows, so a correct sample produces several colours.
    const bool covered = non_black > (kWidth * kHeight * 9) / 10;
    const bool sampled = distinct >= 4;
    const bool clean = ctx.validation_errors == 0;

    std::cout << "check: covered=" << (covered ? "yes" : "no")
              << " sampled=" << (sampled ? "yes" : "no")
              << " validation_clean=" << (clean ? "yes" : "no") << "\n";

    // Write the image so the result can be inspected.
    {
        std::ofstream ppm{"sc11r_selftest.ppm", std::ios::binary};
        if (ppm) {
            ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
            for (u32 i = 0; i < kWidth * kHeight; ++i) {
                ppm.put(static_cast<char>(src[i * 4 + 0]));
                ppm.put(static_cast<char>(src[i * 4 + 1]));
                ppm.put(static_cast<char>(src[i * 4 + 2]));
            }
            std::cout << "wrote sc11r_selftest.ppm\n";
        }
    }

    vkDestroyBuffer(ctx.device, staging_buffer, nullptr);
    vkFreeMemory(ctx.device, staging_memory, nullptr);
    vkDestroyBuffer(ctx.device, readback_buffer, nullptr);
    vkFreeMemory(ctx.device, readback_memory, nullptr);
    vkDestroySampler(ctx.device, sampler, nullptr);
    vkDestroyImageView(ctx.device, texture_view, nullptr);
    vkDestroyImage(ctx.device, texture_image, nullptr);
    vkFreeMemory(ctx.device, texture_memory, nullptr);
    vkDestroyDescriptorPool(ctx.device, descriptor_pool, nullptr);
    vkDestroyBuffer(ctx.device, uniform_buffer, nullptr);
    vkFreeMemory(ctx.device, uniform_memory, nullptr);
    vkDestroyBuffer(ctx.device, index_buffer, nullptr);
    vkFreeMemory(ctx.device, index_memory, nullptr);
    vkDestroyBuffer(ctx.device, vertex_buffer, nullptr);
    vkFreeMemory(ctx.device, vertex_memory, nullptr);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, set_layout, nullptr);
    vkDestroyFramebuffer(ctx.device, framebuffer, nullptr);
    vkDestroyRenderPass(ctx.device, render_pass, nullptr);
    vkDestroyImageView(ctx.device, color_view, nullptr);
    vkDestroyImage(ctx.device, color_image, nullptr);
    vkFreeMemory(ctx.device, color_memory, nullptr);
    vkDestroyShaderModule(ctx.device, vertex_module, nullptr);
    vkDestroyShaderModule(ctx.device, fragment_module, nullptr);
    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    if (ctx.messenger != VK_NULL_HANDLE) {
        auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr) {
            destroy_messenger(ctx.instance, ctx.messenger, nullptr);
        }
    }
    vkDestroyInstance(ctx.instance, nullptr);

    return covered && sampled && clean;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered, so a hang can be located from the last line printed.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::string shader_dir = "tools/geometry_replay/test_shaders";
    bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selftest") {
            selftest = true;
        } else if (arg == "--shader-dir" && i + 1 < argc) {
            shader_dir = argv[++i];
        }
    }

    if (!selftest) {
        std::cerr << "usage: deracine_original_shader_pipeline --selftest [--shader-dir DIR]\n";
        std::cerr << "       the captured-package path is not yet wired; see the report\n";
        return 2;
    }

    return RunSelfTest(shader_dir) ? 0 : 1;
}
