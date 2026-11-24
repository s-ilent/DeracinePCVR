// Image creation, readback and content verification helpers.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/vk_bootstrap.h"

namespace harness {

struct ImageDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t depth = 1;
    uint32_t mipNum = 1;
    uint32_t layerNum = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImageCreateFlags flags = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ImageDesc desc;
    bool ownsMemory = true;
};

// Creates an image and binds memory. Returns false on failure.
bool CreateImage(VulkanContext& ctx, const ImageDesc& desc, Image& out, std::string& error);

// Creates an image without binding memory (for external/imported images).
bool CreateImageUnbound(VulkanContext& ctx, const ImageDesc& desc, Image& out,
                        std::string& error);

void DestroyImage(VulkanContext& ctx, Image& img);

// Creates a view over a subresource range.
VkImageView CreateView(VulkanContext& ctx, VkImage image, VkFormat format,
                       VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount,
                       uint32_t baseLayer, uint32_t layerCount, VkImageViewType viewType,
                       std::string& error);

// Transitions a subresource range. If 'aspects' is 0, all format aspects are used.
void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspects,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
                     uint32_t layerCount, VkPipelineStageFlags srcStage,
                     VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                     VkAccessFlags dstAccess);

// Reads back a color image into RGBA8 (or the closest representable form).
// Returns false if the format is not supported by the reader.
bool ReadbackColor(VulkanContext& ctx, VkCommandPool pool, const Image& img,
                   uint32_t layer, std::vector<unsigned char>& out, std::string& error);

// Reads back a depth image as float.
bool ReadbackDepth(VulkanContext& ctx, VkCommandPool pool, const Image& img,
                   uint32_t layer, std::vector<float>& out, std::string& error);

// Reads back a stencil image as uint8.
bool ReadbackStencil(VulkanContext& ctx, VkCommandPool pool, const Image& img,
                     uint32_t layer, std::vector<unsigned char>& out, std::string& error);

// Clears a color image to a known RGBA value.
void ClearColor(VulkanContext& ctx, VkCommandPool pool, const Image& img,
                const float rgba[4], uint32_t layer);

// Clears depth and stencil independently.
void ClearDepthStencil(VulkanContext& ctx, VkCommandPool pool, const Image& img,
                       float depth, uint32_t stencil, uint32_t layer);

// Reports whether a format has a depth aspect, stencil aspect, or both.
bool HasDepthAspect(VkFormat f);
bool HasStencilAspect(VkFormat f);

// Picks a supported combined depth/stencil format, or VK_FORMAT_UNDEFINED.
VkFormat PickDepthStencilFormat(VulkanContext& ctx, bool requireSampled);

}  // namespace harness
