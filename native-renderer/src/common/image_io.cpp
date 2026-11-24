#include "common/image_io.h"

#include <cstring>

namespace harness {

bool HasDepthAspect(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return true;
        default:
            return false;
    }
}

bool HasStencilAspect(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

VkFormat PickDepthStencilFormat(VulkanContext& ctx, bool requireSampled) {
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
    };
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), f, &props);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (requireSampled) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & need) == need) return f;
    }
    return VK_FORMAT_UNDEFINED;
}

bool CreateImageUnbound(VulkanContext& ctx, const ImageDesc& desc, Image& out,
                        std::string& error) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.flags = desc.flags;
    ci.imageType = desc.type;
    ci.format = desc.format;
    ci.extent = {desc.width, desc.height, desc.depth};
    ci.mipLevels = desc.mipNum;
    ci.arrayLayers = desc.layerNum;
    ci.samples = desc.samples;
    ci.tiling = desc.tiling;
    ci.usage = desc.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkResult res = vkCreateImage(ctx.device(), &ci, nullptr, &out.image);
    if (res != VK_SUCCESS) {
        error = "vkCreateImage failed: " + std::to_string(res);
        return false;
    }
    out.desc = desc;
    out.memory = VK_NULL_HANDLE;
    out.ownsMemory = false;
    return true;
}

bool CreateImage(VulkanContext& ctx, const ImageDesc& desc, Image& out, std::string& error) {
    if (!CreateImageUnbound(ctx, desc, out, error)) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(ctx.device(), out.image, &req);

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        ai.memoryTypeIndex = ctx.FindMemoryType(req.memoryTypeBits, 0);
    }
    if (ai.memoryTypeIndex == UINT32_MAX) {
        error = "no suitable memory type for image";
        vkDestroyImage(ctx.device(), out.image, nullptr);
        out.image = VK_NULL_HANDLE;
        return false;
    }

    const VkResult res = vkAllocateMemory(ctx.device(), &ai, nullptr, &out.memory);
    if (res != VK_SUCCESS) {
        error = "vkAllocateMemory failed: " + std::to_string(res);
        vkDestroyImage(ctx.device(), out.image, nullptr);
        out.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(ctx.device(), out.image, out.memory, 0);
    out.ownsMemory = true;
    return true;
}

void DestroyImage(VulkanContext& ctx, Image& img) {
    if (img.image != VK_NULL_HANDLE) {
        vkDestroyImage(ctx.device(), img.image, nullptr);
        img.image = VK_NULL_HANDLE;
    }
    if (img.memory != VK_NULL_HANDLE && img.ownsMemory) {
        vkFreeMemory(ctx.device(), img.memory, nullptr);
        img.memory = VK_NULL_HANDLE;
    }
}

VkImageView CreateView(VulkanContext& ctx, VkImage image, VkFormat format,
                       VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                       uint32_t baseLayer, uint32_t layerCount, VkImageViewType viewType,
                       std::string& error) {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;
    ci.viewType = viewType;
    ci.format = format;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = baseMip;
    ci.subresourceRange.levelCount = mipCount;
    ci.subresourceRange.baseArrayLayer = baseLayer;
    ci.subresourceRange.layerCount = layerCount;

    VkImageView view = VK_NULL_HANDLE;
    const VkResult res = vkCreateImageView(ctx.device(), &ci, nullptr, &view);
    if (res != VK_SUCCESS) {
        error = "vkCreateImageView failed: " + std::to_string(res);
        return VK_NULL_HANDLE;
    }
    return view;
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
                     uint32_t layerCount, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                     VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspects;
    b.subresourceRange.baseMipLevel = baseMip;
    b.subresourceRange.levelCount = mipCount;
    b.subresourceRange.baseArrayLayer = baseLayer;
    b.subresourceRange.layerCount = layerCount;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

namespace {

// Creates a host-visible staging buffer, copies the image into it, and returns
// the bytes. Handles the layout transition and the copy.
bool ReadbackRaw(VulkanContext& ctx, VkCommandPool pool, const Image& img, uint32_t layer,
                 VkImageAspectFlags aspect, VkImageLayout currentLayout,
                 VkDeviceSize bytesPerPixel, std::vector<unsigned char>& out,
                 std::string& error) {
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(img.desc.width) * img.desc.height * bytesPerPixel;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(ctx.device(), &bci, nullptr, &buffer) != VK_SUCCESS) {
        error = "staging vkCreateBuffer failed";
        return false;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(ctx.device(), buffer, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = ctx.FindMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        error = "no host-visible memory for staging";
        vkDestroyBuffer(ctx.device(), buffer, nullptr);
        return false;
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(ctx.device(), &ai, nullptr, &mem) != VK_SUCCESS) {
        error = "staging vkAllocateMemory failed";
        vkDestroyBuffer(ctx.device(), buffer, nullptr);
        return false;
    }
    vkBindBufferMemory(ctx.device(), buffer, mem, 0);

    VkCommandBuffer cmd = ctx.BeginOneShot(pool);
    TransitionImage(cmd, img.image, aspect, currentLayout,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, layer, 1,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {img.desc.width, img.desc.height, 1};
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                           &region);

    TransitionImage(cmd, img.image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    currentLayout, 0, 1, layer, 1, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_MEMORY_WRITE_BIT);
    ctx.EndOneShot(pool, cmd);

    void* mapped = nullptr;
    if (vkMapMemory(ctx.device(), mem, 0, size, 0, &mapped) != VK_SUCCESS) {
        error = "vkMapMemory failed";
        vkDestroyBuffer(ctx.device(), buffer, nullptr);
        vkFreeMemory(ctx.device(), mem, nullptr);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    std::memcpy(out.data(), mapped, static_cast<size_t>(size));
    vkUnmapMemory(ctx.device(), mem);

    vkDestroyBuffer(ctx.device(), buffer, nullptr);
    vkFreeMemory(ctx.device(), mem, nullptr);
    return true;
}

}  // namespace

bool ReadbackColor(VulkanContext& ctx, VkCommandPool pool, const Image& img, uint32_t layer,
                   std::vector<unsigned char>& out, std::string& error) {
    VkDeviceSize bpp = 4;
    if (img.desc.format == VK_FORMAT_R8G8B8A8_UNORM ||
        img.desc.format == VK_FORMAT_R8G8B8A8_SRGB ||
        img.desc.format == VK_FORMAT_B8G8R8A8_UNORM ||
        img.desc.format == VK_FORMAT_B8G8R8A8_SRGB) {
        bpp = 4;
    } else if (img.desc.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        bpp = 8;
    } else if (img.desc.format == VK_FORMAT_R32G32B32A32_SFLOAT) {
        bpp = 16;
    } else {
        error = "ReadbackColor: unsupported format " + std::to_string(img.desc.format);
        return false;
    }
    return ReadbackRaw(ctx, pool, img, layer, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, bpp, out, error);
}

bool ReadbackDepth(VulkanContext& ctx, VkCommandPool pool, const Image& img, uint32_t layer,
                   std::vector<float>& out, std::string& error) {
    VkDeviceSize bpp = 0;
    switch (img.desc.format) {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            bpp = 4;
            break;
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            bpp = 2;
            break;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            bpp = 4;
            break;
        default:
            error = "ReadbackDepth: unsupported format";
            return false;
    }
    std::vector<unsigned char> raw;
    if (!ReadbackRaw(ctx, pool, img, layer, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, bpp, raw, error)) {
        return false;
    }
    const size_t count = static_cast<size_t>(img.desc.width) * img.desc.height;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (bpp == 4) {
            if (img.desc.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                img.desc.format == VK_FORMAT_X8_D24_UNORM_PACK32) {
                uint32_t v;
                std::memcpy(&v, raw.data() + i * 4, 4);
                out[i] = static_cast<float>(v & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
            } else {
                std::memcpy(&out[i], raw.data() + i * 4, 4);
            }
        } else {
            uint16_t v;
            std::memcpy(&v, raw.data() + i * 2, 2);
            out[i] = static_cast<float>(v) / 65535.0f;
        }
    }
    return true;
}

bool ReadbackStencil(VulkanContext& ctx, VkCommandPool pool, const Image& img, uint32_t layer,
                     std::vector<unsigned char>& out, std::string& error) {
    if (!HasStencilAspect(img.desc.format)) {
        error = "ReadbackStencil: format has no stencil aspect";
        return false;
    }
    VkDeviceSize bpp = 1;
    if (img.desc.format == VK_FORMAT_D24_UNORM_S8_UINT ||
        img.desc.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        bpp = 4;
    } else if (img.desc.format == VK_FORMAT_D16_UNORM_S8_UINT) {
        bpp = 4;
    }
    std::vector<unsigned char> raw;
    if (!ReadbackRaw(ctx, pool, img, layer, VK_IMAGE_ASPECT_STENCIL_BIT,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, bpp, raw, error)) {
        return false;
    }
    const size_t count = static_cast<size_t>(img.desc.width) * img.desc.height;
    out.resize(count);
    for (size_t i = 0; i < count; ++i) {
        // Stencil occupies byte 1 of the packed D24S8 / D32S8 word.
        out[i] = raw[i * bpp + 1];
    }
    return true;
}

void ClearColor(VulkanContext& ctx, VkCommandPool pool, const Image& img, const float rgba[4],
                uint32_t layer) {
    VkCommandBuffer cmd = ctx.BeginOneShot(pool);
    TransitionImage(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, layer, 1,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue cv{};
    std::memcpy(cv.float32, rgba, sizeof(cv.float32));
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = layer;
    range.layerCount = 1;
    vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    TransitionImage(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, layer, 1,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    ctx.EndOneShot(pool, cmd);
}

void ClearDepthStencil(VulkanContext& ctx, VkCommandPool pool, const Image& img, float depth,
                       uint32_t stencil, uint32_t layer) {
    VkCommandBuffer cmd = ctx.BeginOneShot(pool);
    VkImageAspectFlags aspects = 0;
    if (HasDepthAspect(img.desc.format)) aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (HasStencilAspect(img.desc.format)) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;

    TransitionImage(cmd, img.image, aspects, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, layer, 1,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT);

    VkClearDepthStencilValue cv{};
    cv.depth = depth;
    cv.stencil = stencil;
    VkImageSubresourceRange range{};
    range.aspectMask = aspects;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = layer;
    range.layerCount = 1;
    vkCmdClearDepthStencilImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1,
                                &range);

    TransitionImage(cmd, img.image, aspects, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1, layer, 1,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    ctx.EndOneShot(pool, cmd);
}

}  // namespace harness
