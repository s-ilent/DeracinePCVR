// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Independent Vulkan regression test for compressed-image readback.
//
// This program does not use shadPS4, Deracine, or any emulator code. It builds a Vulkan image with
// known contents, uploads a known payload, transitions it to a sampled layout, reads it back
// through the same copy-region construction the capture uses, and compares the entire downloaded
// range byte for byte against the original.
//
// The point is to prove the copy is complete, not merely that a copy command succeeded. A copy that
// covers a quarter of the image still succeeds, and a hash of the bytes it did copy still matches
// itself. Only a byte-for-byte comparison against the source payload catches that.
//
// Usage: deracine_readback_test [--verbose]

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// The storage geometry of a format. This mirrors the production helper in
/// src/src/video_core/texture_cache/format_block_info.cpp; the test keeps its own copy so it does
/// not depend on the emulator, and the two are checked against each other by the shared cases.
struct BlockInfo {
    u32 block_width{1};
    u32 block_height{1};
    u32 bytes_per_block{};
    bool compressed{};

    u32 BlocksFor(u32 texels) const {
        return (texels + block_width - 1) / block_width;
    }
    u32 BlockRowsFor(u32 texels) const {
        return (texels + block_height - 1) / block_height;
    }
    u64 RowPitchFor(u32 texel_width) const {
        return static_cast<u64>(BlocksFor(texel_width)) * bytes_per_block;
    }
    u64 ByteSizeFor(u32 texel_width, u32 texel_height) const {
        return RowPitchFor(texel_width) * BlockRowsFor(texel_height);
    }
};

BlockInfo InfoFor(VkFormat format) {
    switch (format) {
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
        return {4, 4, 16, true};
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        return {4, 4, 8, true};
    case VK_FORMAT_R8G8B8A8_UNORM:
        return {1, 1, 4, false};
    default:
        return {};
    }
}

#define VK_CHECK(call)                                                                          \
    do {                                                                                        \
        const VkResult result_ = (call);                                                        \
        if (result_ != VK_SUCCESS) {                                                            \
            std::cerr << "Vulkan call failed: " #call " -> " << result_ << "\n";                \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    u32 queue_family{};
    VkCommandPool command_pool{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    bool validation_enabled{};
};

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

/// One readback case: an image of a given format and extent, at a given mip and layer.
struct Case {
    const char* name;
    VkFormat format;
    u32 width;
    u32 height;
    u32 mip_level;
    u32 layer;
    u32 mip_count;
    u32 layer_count;
};

/// A deterministic payload: every byte is a function of its index, so a missing or misplaced write
/// cannot coincidentally match. The first bytes are a recognisable marker.
std::vector<u8> MakePayload(u64 size) {
    std::vector<u8> bytes(size);
    for (u64 i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>((i * 37u + 11u) & 0xFFu);
    }
    if (size >= 4) {
        bytes[0] = 0xDE;
        bytes[1] = 0xAD;
        bytes[2] = 0xBE;
        bytes[3] = 0xEF;
    }
    return bytes;
}

/// Runs one case. Returns true when the readback matches the payload byte for byte.
///
/// `use_block_extent` reproduces the SC-10 defect: passing the block count where Vulkan expects a
/// texel extent. It exists so the test can be shown to fail on the defect rather than only to pass
/// on the fix.
bool RunCase(Context& ctx, const Case& test, bool verbose, bool use_block_extent = false) {
    const BlockInfo block = InfoFor(test.format);
    if (block.bytes_per_block == 0) {
        std::cerr << "  " << test.name << ": unsupported format\n";
        return false;
    }

    // The mip extent, in texels.
    const u32 texel_width = std::max(1u, test.width >> test.mip_level);
    const u32 texel_height = std::max(1u, test.height >> test.mip_level);

    // Storage geometry, in blocks, and the byte size that follows from it.
    const u32 block_columns = block.BlocksFor(texel_width);
    const u32 block_rows = block.BlockRowsFor(texel_height);
    const u64 row_pitch = static_cast<u64>(block_columns) * block.bytes_per_block;
    const u64 layer_bytes = row_pitch * block_rows;
    const u64 total_bytes = layer_bytes * test.layer_count;

    const auto payload = MakePayload(total_bytes);

    // --- Image --------------------------------------------------------------------------------
    VkImage image{};
    VkDeviceMemory image_memory{};
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = test.format;
    image_info.extent = {test.width, test.height, 1};
    image_info.mipLevels = test.mip_count;
    image_info.arrayLayers = test.layer + test.layer_count;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(ctx.device, &image_info, nullptr, &image) != VK_SUCCESS) {
        std::cerr << "  " << test.name << ": image creation failed\n";
        return false;
    }
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(ctx.device, image, &image_requirements);
    VkMemoryAllocateInfo image_alloc{};
    image_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    image_alloc.allocationSize = image_requirements.size;
    image_alloc.memoryTypeIndex =
        FindMemoryType(ctx, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(ctx.device, &image_alloc, nullptr, &image_memory) != VK_SUCCESS ||
        vkBindImageMemory(ctx.device, image, image_memory, 0) != VK_SUCCESS) {
        std::cerr << "  " << test.name << ": image memory failed\n";
        return false;
    }

    // --- Staging in, and staging out ----------------------------------------------------------
    VkBuffer upload{};
    VkDeviceMemory upload_memory{};
    if (!CreateBuffer(ctx, total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      upload, upload_memory)) {
        std::cerr << "  " << test.name << ": upload buffer failed\n";
        return false;
    }
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(ctx.device, upload_memory, 0, total_bytes, 0, &mapped));
    std::memcpy(mapped, payload.data(), static_cast<size_t>(total_bytes));
    vkUnmapMemory(ctx.device, upload_memory);

    VkBuffer download{};
    VkDeviceMemory download_memory{};
    if (!CreateBuffer(ctx, total_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      download, download_memory)) {
        std::cerr << "  " << test.name << ": download buffer failed\n";
        return false;
    }
    // Fill the download buffer with a pattern that cannot be mistaken for a successful copy, so a
    // copy that writes nothing is visible rather than passing as valid data.
    VK_CHECK(vkMapMemory(ctx.device, download_memory, 0, total_bytes, 0, &mapped));
    std::memset(mapped, 0xA5, static_cast<size_t>(total_bytes));
    vkUnmapMemory(ctx.device, download_memory);

    // --- Record -------------------------------------------------------------------------------
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cmd_alloc, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    const VkImageSubresourceRange range{
        VK_IMAGE_ASPECT_COLOR_BIT, test.mip_level, 1, test.layer, test.layer_count};

    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);

    // The upload region uses the same texel-space extent the production copy uses.
    VkBufferImageCopy upload_copy{};
    upload_copy.bufferOffset = 0;
    upload_copy.bufferRowLength = 0;
    upload_copy.bufferImageHeight = 0;
    upload_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, test.mip_level, test.layer,
                                    test.layer_count};
    upload_copy.imageExtent = {texel_width, texel_height, 1};
    vkCmdCopyBufferToImage(cmd, upload, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &upload_copy);

    // Transition to a sampled layout, which is the state the capture finds the image in.
    VkImageMemoryBarrier to_shader = to_dst;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_shader);

    // The readback region: the same construction the capture uses. The extent is in texels and the
    // row length is left zero for a tightly packed transfer.
    VkImageMemoryBarrier to_src = to_shader;
    to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_src);

    VkBufferImageCopy readback_copy{};
    readback_copy.bufferOffset = 0;
    readback_copy.bufferRowLength = 0;
    readback_copy.bufferImageHeight = 0;
    readback_copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, test.mip_level, test.layer,
                                      test.layer_count};
    readback_copy.imageExtent = use_block_extent
                                    ? VkExtent3D{block_columns, block_rows, 1}
                                    : VkExtent3D{texel_width, texel_height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, download, 1,
                           &readback_copy);

    VkBufferMemoryBarrier host_visible{};
    host_visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    host_visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    host_visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_visible.buffer = download;
    host_visible.offset = 0;
    host_visible.size = total_bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &host_visible, 0, nullptr);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE));
    // Wait for this specific submission, so the comparison happens after the transfer completed.
    VK_CHECK(vkQueueWaitIdle(ctx.queue));

    // --- Compare ------------------------------------------------------------------------------
    VK_CHECK(vkMapMemory(ctx.device, download_memory, 0, total_bytes, 0, &mapped));
    const auto* actual = static_cast<const u8*>(mapped);

    u64 mismatched = 0;
    u64 first_mismatch = ~0ull;
    u64 untouched = 0;
    for (u64 i = 0; i < total_bytes; ++i) {
        if (actual[i] != payload[i]) {
            if (first_mismatch == ~0ull)
                first_mismatch = i;
            ++mismatched;
        }
        // A byte still holding the pre-fill pattern was never written by the copy.
        if (actual[i] == 0xA5 && payload[i] != 0xA5)
            ++untouched;
    }
    vkUnmapMemory(ctx.device, download_memory);

    const bool ok = mismatched == 0 && untouched == 0;
    std::cout << "  " << (ok ? "PASS" : "FAIL") << "  " << test.name << ": " << texel_width << "x"
              << texel_height << " texels, " << block_columns << "x" << block_rows << " blocks, "
              << total_bytes << " bytes";
    if (!ok) {
        std::cout << ", mismatched=" << mismatched << " first_at=" << first_mismatch
                  << " untouched=" << untouched;
    }
    std::cout << "\n";
    if (verbose && !ok && first_mismatch != ~0ull) {
        const u64 at = first_mismatch;
        std::cout << "        expected " << static_cast<int>(payload[at]) << " got "
                  << static_cast<int>(actual[at]) << "\n";
    }

    vkDestroyBuffer(ctx.device, download, nullptr);
    vkFreeMemory(ctx.device, download_memory, nullptr);
    vkDestroyBuffer(ctx.device, upload, nullptr);
    vkFreeMemory(ctx.device, upload_memory, nullptr);
    vkDestroyImage(ctx.device, image, nullptr);
    vkFreeMemory(ctx.device, image_memory, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &cmd);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const bool verbose = argc > 1 && std::string{argv[1]} == "--verbose";

    Context ctx{};
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_readback_test";
    app.apiVersion = VK_API_VERSION_1_1;

    // Enable the validation layer when it is installed, so an invalid copy region is reported
    // rather than silently accepted.
    const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    u32 layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, validation_layer) == 0) {
            ctx.validation_enabled = true;
            break;
        }
    }

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (ctx.validation_enabled) {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &validation_layer;
    }
    if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed\n";
        return 1;
    }
    std::cout << "validation layers: " << (ctx.validation_enabled ? "enabled" : "unavailable")
              << "\n";

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

    // The cases the task names, plus a mip and a layer case.
    const Case cases[] = {
        {"BC3 64x64", VK_FORMAT_BC3_UNORM_BLOCK, 64, 64, 0, 0, 1, 1},
        {"BC3 8x8", VK_FORMAT_BC3_UNORM_BLOCK, 8, 8, 0, 0, 1, 1},
        {"BC3 7x5 (not block aligned)", VK_FORMAT_BC3_UNORM_BLOCK, 7, 5, 0, 0, 1, 1},
        {"BC3 64x64 mip 2", VK_FORMAT_BC3_UNORM_BLOCK, 64, 64, 2, 0, 3, 1},
        {"BC3 64x64 layer 1", VK_FORMAT_BC3_UNORM_BLOCK, 64, 64, 0, 1, 1, 2},
        {"RGBA8 64x64 control", VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 0, 0, 1, 1},
        {"RGBA8 7x5 control", VK_FORMAT_R8G8B8A8_UNORM, 7, 5, 0, 0, 1, 1},
        {"BC1 64x64 (8-byte blocks)", VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 64, 64, 0, 0, 1, 1},
    };

    std::cout << "\nreadback cases:\n";
    u32 passed = 0;
    u32 total = 0;
    for (const auto& test : cases) {
        ++total;
        if (RunCase(ctx, test, verbose))
            ++passed;
    }

    std::cout << "\n" << passed << "/" << total << " cases byte-for-byte equal\n";

    // The negative control: the same BC3 64x64 case with the block count used as the copy extent.
    // It must fail, or the test is not sensitive to the defect it exists to catch.
    std::cout << "\nnegative control (block extent, the SC-10 defect):\n";
    const Case defect_case{"BC3 64x64 with block extent", VK_FORMAT_BC3_UNORM_BLOCK, 64, 64,
                           0, 0, 1, 1};
    const bool defect_passed = RunCase(ctx, defect_case, verbose, true);
    std::cout << "  " << (defect_passed ? "UNEXPECTED PASS" : "correctly FAILED")
              << "  the block-extent copy does not reproduce the image\n";

    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
    return (passed == total && !defect_passed) ? 0 : 1;
}
