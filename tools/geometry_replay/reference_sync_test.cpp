// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Independent GPU fixture for the reference-output synchronization contract.
//
// This program does not use the emulator. It reproduces the pattern the corrected capture uses --
// record a copy into a dedicated staging buffer, defer the CPU read until the GPU has completed it,
// then read -- and proves the properties the capture depends on:
//
//   1. A deferred read sees the copied contents, not the buffer's prior contents.
//   2. A read taken before the GPU completes sees the prior contents, which is the defect.
//   3. Two captures into separate staging buffers do not overwrite each other.
//   4. A capture that changes the image produces a different hash.
//   5. Validation layers report no errors in the corrected path.
//
// The negative control is the important one: it demonstrates that the defect is real and that the
// test can see it, rather than only asserting that the corrected path works.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kWidth = 64;
constexpr u32 kHeight = 64;
constexpr u64 kImageBytes = static_cast<u64>(kWidth) * kHeight * 4;

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
        std::fprintf(stderr, "validation: %s\n", data->pMessage != nullptr ? data->pMessage : "");
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
                  VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory,
                  void** mapped_out = nullptr) {
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
    if (vkBindBufferMemory(ctx.device, buffer, memory, 0) != VK_SUCCESS)
        return false;
    if (mapped_out != nullptr) {
        if (vkMapMemory(ctx.device, memory, 0, size, 0, mapped_out) != VK_SUCCESS)
            return false;
    }
    return true;
}

/// A simple 64-bit hash, so a difference is detectable without a crypto dependency.
u64 HashBytes(const std::vector<u8>& bytes) {
    u64 hash = 0xcbf29ce484222325ull;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// Fills the colour image with a value derived from `seed`, so two fills differ.
void FillImage(Context& ctx, VkImage image, u8 seed) {
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    void* mapped = nullptr;
    CreateBuffer(ctx, kImageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, staging_memory, &mapped);
    auto* bytes = static_cast<u8*>(mapped);
    for (u64 i = 0; i < kImageBytes; ++i) {
        bytes[i] = static_cast<u8>((i + seed) & 0xFF);
    }

    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = ctx.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &alloc, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_dst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &region);

    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_src);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);

    vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &cmd);
    vkUnmapMemory(ctx.device, staging_memory);
    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, staging_memory, nullptr);
}

/// The command buffer the last RecordCopy submitted, freed by the caller after the wait.
VkCommandBuffer pending_command = VK_NULL_HANDLE;

/// Records a copy of the image into `staging`, without waiting for it to complete.
void RecordCopy(Context& ctx, VkImage image, VkBuffer staging) {
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = ctx.command_pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &alloc, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE);
    // Deliberately no wait: the caller decides when to synchronise. The command buffer is not freed
    // here because it is still pending; the caller frees it after the wait. Freeing it now is the
    // VUID-vkFreeCommandBuffers-pCommandBuffers-00047 violation the validation layer reports.
    pending_command = cmd;
}

bool Run() {
    Context ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "sc11r_reference_sync_test";
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
    std::vector<const char*> instance_extensions;
    if (have_validation) {
        instance_layers.push_back(validation_layer);
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
        std::fprintf(stderr, "vkCreateInstance failed\n");
        return false;
    }

    if (have_validation) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = DebugCallback;
            info.pUserData = &ctx;
            create_messenger(ctx.instance, &info, nullptr, &ctx.messenger);
        }
    }

    u32 device_count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, nullptr);
    if (device_count == 0) {
        std::fprintf(stderr, "no Vulkan device\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, devices.data());
    ctx.physical = devices.front();

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
        std::fprintf(stderr, "no graphics queue\n");
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
        std::fprintf(stderr, "vkCreateDevice failed\n");
        return false;
    }
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);
    vkGetPhysicalDeviceMemoryProperties(ctx.physical, &ctx.memory_properties);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx.queue_family;
    vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool);

    // The colour image the capture reads.
    VkImage image{};
    VkDeviceMemory image_memory{};
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
        info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(ctx.device, &info, nullptr, &image);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(ctx.device, image, &requirements);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex =
            FindMemoryType(ctx, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(ctx.device, &alloc, nullptr, &image_memory);
        vkBindImageMemory(ctx.device, image, image_memory, 0);
    }

    // Two separate staging buffers, as the corrected capture uses one per position.
    VkBuffer pre_staging{};
    VkDeviceMemory pre_memory{};
    void* pre_mapped = nullptr;
    VkBuffer post_staging{};
    VkDeviceMemory post_memory{};
    void* post_mapped = nullptr;
    CreateBuffer(ctx, kImageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 pre_staging, pre_memory, &pre_mapped);
    CreateBuffer(ctx, kImageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 post_staging, post_memory, &post_mapped);

    // Poison both staging buffers, so a read that happens before the copy sees the poison rather
    // than a plausible zero.
    std::memset(pre_mapped, 0xAA, kImageBytes);
    std::memset(post_mapped, 0xBB, kImageBytes);

    bool ok = true;

    // --- 1. Fill the image with a known pattern ----------------------------------------------
    FillImage(ctx, image, 0x11);
    std::vector<u8> expected(kImageBytes);
    for (u64 i = 0; i < kImageBytes; ++i) {
        expected[i] = static_cast<u8>((i + 0x11) & 0xFF);
    }
    const u64 expected_hash = HashBytes(expected);

    // --- 2. Negative control: read before the GPU completes ----------------------------------
    RecordCopy(ctx, image, pre_staging);
    {
        std::vector<u8> premature(kImageBytes);
        std::memcpy(premature.data(), pre_mapped, kImageBytes);
        const u64 premature_hash = HashBytes(premature);
        const bool saw_poison = premature_hash != expected_hash;
        std::printf("negative control (read before completion): %s\n",
                    saw_poison ? "PASS  saw the staging buffer's prior contents"
                               : "FAIL  the read already saw the copy");
        if (!saw_poison) {
            ok = false;
        }
    }

    // --- 3. Corrected path: wait, then read --------------------------------------------------
    vkQueueWaitIdle(ctx.queue);
    if (pending_command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &pending_command);
        pending_command = VK_NULL_HANDLE;
    }
    {
        std::vector<u8> deferred(kImageBytes);
        std::memcpy(deferred.data(), pre_mapped, kImageBytes);
        const u64 deferred_hash = HashBytes(deferred);
        const bool matched = deferred_hash == expected_hash;
        std::printf("deferred read after completion: %s\n",
                    matched ? "PASS  the read saw the copied contents"
                            : "FAIL  the read did not match the image");
        if (!matched) {
            ok = false;
        }
    }

    // --- 4. A second capture into its own buffer does not disturb the first ------------------
    FillImage(ctx, image, 0x77);
    RecordCopy(ctx, image, post_staging);
    vkQueueWaitIdle(ctx.queue);
    if (pending_command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &pending_command);
        pending_command = VK_NULL_HANDLE;
    }
    {
        std::vector<u8> post(kImageBytes);
        std::memcpy(post.data(), post_mapped, kImageBytes);
        std::vector<u8> pre_again(kImageBytes);
        std::memcpy(pre_again.data(), pre_mapped, kImageBytes);
        const bool post_differs = HashBytes(post) != expected_hash;
        const bool pre_intact = HashBytes(pre_again) == expected_hash;
        std::printf("separate staging buffers: %s\n",
                    (post_differs && pre_intact)
                        ? "PASS  the second capture did not overwrite the first"
                        : "FAIL  a capture overwrote another");
        if (!post_differs || !pre_intact) {
            ok = false;
        }
    }

    // --- 5. Validation layers ----------------------------------------------------------------
    std::printf("validation errors: %u\n", ctx.validation_errors);
    if (ctx.validation_errors != 0) {
        ok = false;
    }

    vkUnmapMemory(ctx.device, pre_memory);
    vkUnmapMemory(ctx.device, post_memory);
    vkDestroyBuffer(ctx.device, pre_staging, nullptr);
    vkFreeMemory(ctx.device, pre_memory, nullptr);
    vkDestroyBuffer(ctx.device, post_staging, nullptr);
    vkFreeMemory(ctx.device, post_memory, nullptr);
    vkDestroyImage(ctx.device, image, nullptr);
    vkFreeMemory(ctx.device, image_memory, nullptr);
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
    return ok;
}

} // namespace

int main() {
    std::printf("reference sync test: start\n");
    const bool ok = Run();
    std::printf("reference sync test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
