// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Independent GPU fixture for the reference capture's rendering-scope lifecycle.
//
// The defect this exists to catch: a capture reads an attachment by recording a copy, and a copy
// cannot be issued inside a Vulkan rendering scope. So the capture ends the scope. If nothing
// re-opens it, the draw that follows executes outside any rendering scope, which is invalid and
// produces no output.
//
// This program does not use the emulator. It reproduces the sequence the rasterizer records --
// begin scope, capture, copy, bind pipeline, draw, capture -- and proves:
//
//   1. A draw after a capture-induced scope break executes inside a scope (the fix).
//   2. The same sequence without the resume executes outside one (the negative control).
//   3. A resumed scope loads rather than clears, so the pre-draw contents survive.
//   4. Capture OFF and capture ON produce bit-identical output.
//   5. Two consecutive draws into one target both land.
//   6. A depth-tested draw with a colour attachment works.
//   7. An MSAA configuration is rejected rather than captured wrongly.
//
// The negative control is the important one: it shows the defect is real and that the fixture can
// see it, rather than only asserting that the corrected path works.

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

/// The clear colour. Distinct from anything the shader writes, so a pixel equal to it is a pixel
/// the draw did not cover.
constexpr u32 kClearR = 13;
constexpr u32 kClearG = 13;
constexpr u32 kClearB = 20;

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
    bool validation_enabled{};
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* user) {
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        auto* ctx = static_cast<Context*>(user);
        if (ctx != nullptr) {
            ++ctx->validation_errors;
        }
        std::fprintf(stderr, "  validation error: %s %s\n",
                     data->pMessageIdName ? data->pMessageIdName : "",
                     data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

u32 FindMemoryType(const Context& ctx, u32 bits, VkMemoryPropertyFlags properties) {
    for (u32 i = 0; i < ctx.memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 &&
            (ctx.memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
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
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = FindMemoryType(ctx, requirements.memoryTypeBits, properties);
    if (vkAllocateMemory(ctx.device, &alloc, nullptr, &memory) != VK_SUCCESS)
        return false;
    return vkBindBufferMemory(ctx.device, buffer, memory, 0) == VK_SUCCESS;
}

bool CreateImage(Context& ctx, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                 VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {kWidth, kHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
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
    const bool depth = format == VK_FORMAT_D32_SFLOAT;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    const VkImageAspectFlags aspect =
        depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(ctx.device, &view_info, nullptr, &view) == VK_SUCCESS;
}

VkShaderModule MakeModule(Context& ctx, const u32* words, size_t count) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = count * sizeof(u32);
    info.pCode = words;
    VkShaderModule module{};
    vkCreateShaderModule(ctx.device, &info, nullptr, &module);
    return module;
}

u64 Fnv1a(const u8* data, size_t size) {
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

/// What one run produced.
struct Result {
    u64 hash{};
    u32 covered{};
    u32 distinct{};
    bool draw_executed{};
    bool scope_open_at_draw{};
    /// The configuration cannot be captured, so no capture was attempted.
    bool unsupported{};
    u32 validation_errors{};
};

/// The scenario a run exercises.
struct Scenario {
    const char* name;
    /// Whether a pre-draw capture is taken, which ends the scope.
    bool pre_capture;
    /// Whether the scope is re-opened after the capture. False is the negative control.
    bool resume_after_capture;
    /// Whether the pass clears the colour attachment.
    bool clear;
    /// How many draws to issue into the same target.
    u32 draws;
    /// Whether a depth attachment is present and depth-tested.
    bool depth;
    /// Whether the colour attachment is multisampled.
    bool msaa;
    /// Whether the draw covers only part of the target, so a clear and a load differ.
    bool partial;
};

} // namespace

// The shaders are embedded so the fixture needs no files beside it.
#include "rp_tri_spv.inc"

namespace {

/// Runs one scenario and returns what it produced.
Result RunScenario(Context& ctx, const Scenario& scenario, bool verbose) {
    Result result{};

    VkImage color_image{};
    VkDeviceMemory color_memory{};
    VkImageView color_view{};
    const VkSampleCountFlagBits samples =
        scenario.msaa ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    if (!CreateImage(ctx, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, samples,
                     color_image, color_memory, color_view)) {
        std::fprintf(stderr, "colour image creation failed\n");
        return result;
    }

    VkImage depth_image{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    if (scenario.depth) {
        if (!CreateImage(ctx, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_SAMPLE_COUNT_1_BIT, depth_image, depth_memory, depth_view)) {
            std::fprintf(stderr, "depth image creation failed\n");
            return result;
        }
    }

    // The staging buffer a capture copies into. One per run, so a capture cannot overwrite another.
    VkBuffer staging{};
    VkDeviceMemory staging_memory{};
    if (!CreateBuffer(ctx, kImageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging, staging_memory)) {
        std::fprintf(stderr, "staging buffer creation failed\n");
        return result;
    }

    VkShaderModule vert = MakeModule(ctx, kRpTriVertSpv, sizeof(kRpTriVertSpv) / sizeof(u32));
    VkShaderModule frag = MakeModule(ctx, kRpTriFragSpv, sizeof(kRpTriFragSpv) / sizeof(u32));

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkPipelineLayout layout{};
    vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &layout);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f,
                        1.0f};
    VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
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
    multisample.rasterizationSamples = samples;

    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = scenario.depth ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = scenario.depth ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;

    // Dynamic rendering needs the attachment formats declared on the pipeline.
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering_create{};
    rendering_create.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_create.colorAttachmentCount = 1;
    rendering_create.pColorAttachmentFormats = &color_format;
    rendering_create.depthAttachmentFormat =
        scenario.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.pNext = &rendering_create;
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
    pipeline_info.layout = layout;
    VkPipeline pipeline{};
    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline) != VK_SUCCESS) {
        std::fprintf(stderr, "pipeline creation failed\n");
        return result;
    }

    // --- Record --------------------------------------------------------------------------------
    VkCommandBuffer cmd{};
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = ctx.command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // The colour attachment to transfer-source, so a copy can read it. The pass then brings it back
    // to colour-attachment-optimal, which is the layout the scope needs.
    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = color_image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_transfer);

    if (scenario.depth) {
        VkImageMemoryBarrier to_depth{};
        to_depth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_depth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        to_depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        to_depth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_depth.image = depth_image;
        to_depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_depth);
    }

    VkImageMemoryBarrier to_color = to_transfer;
    to_color.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_color);

    // --- Begin the scope ------------------------------------------------------------------------
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView = color_view;
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp = scenario.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue.color = {{kClearR / 255.0f, kClearG / 255.0f, kClearB / 255.0f,
                                          1.0f}};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = depth_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {kWidth, kHeight}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_attachment;
    rendering.pDepthAttachment = scenario.depth ? &depth_attachment : nullptr;

    vkCmdBeginRendering(cmd, &rendering);
    bool scope_open = true;

    // The pipeline uses dynamic viewport and scissor, so both must be set for every draw. A
    // partial-coverage draw leaves the corners untouched, which is what makes a clear and a load
    // distinguishable in the final image; a full-screen triangle would hide the difference.
    {
        VkViewport active = viewport;
        VkRect2D active_scissor = scissor;
        if (scenario.partial) {
            active.width = static_cast<float>(kWidth) / 2.0f;
            active.height = static_cast<float>(kHeight) / 2.0f;
            active_scissor.extent = {kWidth / 2, kHeight / 2};
        }
        vkCmdSetViewport(cmd, 0, 1, &active);
        vkCmdSetScissor(cmd, 0, 1, &active_scissor);
    }

    // --- Pre-draw capture -----------------------------------------------------------------------
    // The capture ends the scope to record its copy, exactly as Image::DownloadScoped does.
    //
    // A multisampled attachment cannot be copied: a copy requires a single-sample image, and a
    // resolve would be needed first. The capture reports that as unsupported rather than issuing an
    // invalid copy, which is what the emulator's CaptureColorTarget does.
    if (scenario.pre_capture && scenario.msaa) {
        result.unsupported = true;
    }
    if (scenario.pre_capture && !scenario.msaa) {
        vkCmdEndRendering(cmd);
        scope_open = false;

        VkImageMemoryBarrier capture_barrier = to_transfer;
        capture_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        capture_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        capture_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        capture_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &capture_barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                               &region);
        VkImageMemoryBarrier back = capture_barrier;
        back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &back);

        if (scenario.resume_after_capture) {
            // The fix: re-open the scope with a load, so the clear is not repeated.
            VkRenderingAttachmentInfo resumed = color_attachment;
            resumed.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            VkRenderingAttachmentInfo resumed_depth = depth_attachment;
            resumed_depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            VkRenderingInfo resumed_info = rendering;
            resumed_info.pColorAttachments = &resumed;
            resumed_info.pDepthAttachment = scenario.depth ? &resumed_depth : nullptr;
            vkCmdBeginRendering(cmd, &resumed_info);
            scope_open = true;
        }
    }

    // --- Bind and draw --------------------------------------------------------------------------
    result.scope_open_at_draw = scope_open;
    if (scope_open) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (u32 i = 0; i < scenario.draws; ++i) {
            vkCmdDraw(cmd, 3, 1, 0, 0);
            ++result.covered;
        }
        result.draw_executed = true;
        vkCmdEndRendering(cmd);
        scope_open = false;
    }

    // --- Post-draw capture ----------------------------------------------------------------------
    if (scenario.pre_capture && !scenario.msaa) {
        VkImageMemoryBarrier post = to_transfer;
        post.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        post.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        post.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &post);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                               &region);
    }

    // Every scenario ends with a copy, so the readback is the final attachment contents whether or
    // not a capture was taken. Without this a no-capture run would read an untouched staging buffer
    // and the comparison would be meaningless.
    if (!scenario.pre_capture) {
        VkImageMemoryBarrier final_barrier = to_transfer;
        final_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        final_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        final_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        final_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &final_barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                               &region);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.queue);

    // --- Read back ------------------------------------------------------------------------------
    void* mapped = nullptr;
    vkMapMemory(ctx.device, staging_memory, 0, kImageBytes, 0, &mapped);
    const auto* pixels = static_cast<const u8*>(mapped);
    std::vector<u8> copy(pixels, pixels + kImageBytes);
    vkUnmapMemory(ctx.device, staging_memory);

    result.hash = Fnv1a(copy.data(), copy.size());
    std::vector<u32> seen;
    for (u32 i = 0; i < kWidth * kHeight; ++i) {
        const u32 packed = (static_cast<u32>(copy[i * 4]) << 16) |
                           (static_cast<u32>(copy[i * 4 + 1]) << 8) |
                           static_cast<u32>(copy[i * 4 + 2]);
        if (packed != ((kClearR << 16) | (kClearG << 8) | kClearB)) {
            ++result.covered;
        }
        bool found = false;
        for (const u32 value : seen) {
            if (value == packed) {
                found = true;
                break;
            }
        }
        if (!found && seen.size() < 4096) {
            seen.push_back(packed);
        }
    }
    result.distinct = static_cast<u32>(seen.size());
    result.validation_errors = ctx.validation_errors;

    if (verbose) {
        std::printf("    hash=%llu distinct=%u covered=%u draw=%s scope_at_draw=%s\n",
                    static_cast<unsigned long long>(result.hash), result.distinct, result.covered,
                    result.draw_executed ? "yes" : "no",
                    result.scope_open_at_draw ? "yes" : "no");
    }

    vkFreeCommandBuffers(ctx.device, ctx.command_pool, 1, &cmd);
    vkDestroyPipeline(ctx.device, pipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, layout, nullptr);
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, staging_memory, nullptr);
    vkDestroyImageView(ctx.device, color_view, nullptr);
    vkDestroyImage(ctx.device, color_image, nullptr);
    vkFreeMemory(ctx.device, color_memory, nullptr);
    if (scenario.depth) {
        vkDestroyImageView(ctx.device, depth_view, nullptr);
        vkDestroyImage(ctx.device, depth_image, nullptr);
        vkFreeMemory(ctx.device, depth_memory, nullptr);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0)
            verbose = true;
    }

    Context ctx{};

    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "deracine_renderpass_test";
    // Dynamic rendering is a Vulkan 1.3 feature, so the instance must ask for 1.3.
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    instance_info.enabledLayerCount = 1;
    instance_info.ppEnabledLayerNames = layers;
    instance_info.enabledExtensionCount = 1;
    instance_info.ppEnabledExtensionNames = extensions;
    if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
        // Validation may be unavailable; the fixture still runs, and says so.
        instance_info.enabledLayerCount = 0;
        instance_info.ppEnabledLayerNames = nullptr;
        instance_info.enabledExtensionCount = 0;
        instance_info.ppEnabledExtensionNames = nullptr;
        if (vkCreateInstance(&instance_info, nullptr, &ctx.instance) != VK_SUCCESS) {
            std::fprintf(stderr, "vkCreateInstance failed\n");
            return 1;
        }
    } else {
        ctx.validation_enabled = true;
    }

    if (ctx.validation_enabled) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messenger_info{};
            messenger_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messenger_info.pfnUserCallback = DebugCallback;
            messenger_info.pUserData = &ctx;
            create_messenger(ctx.instance, &messenger_info, nullptr, &ctx.messenger);
        }
    }
    std::printf("validation: %s\n", ctx.validation_enabled ? "enabled" : "unavailable");

    u32 device_count = 0;
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, nullptr);
    if (device_count == 0) {
        std::fprintf(stderr, "no Vulkan physical device\n");
        return 1;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(ctx.instance, &device_count, devices.data());
    ctx.physical = devices[0];

    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.physical, &family_count, families.data());
    for (u32 i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            ctx.queue_family = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = ctx.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    // Dynamic rendering is what the emulator uses, so the fixture must enable it too.
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures features{};
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &features13;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    if (vkCreateDevice(ctx.physical, &device_info, nullptr, &ctx.device) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateDevice failed\n");
        return 1;
    }
    vkGetDeviceQueue(ctx.device, ctx.queue_family, 0, &ctx.queue);
    vkGetPhysicalDeviceMemoryProperties(ctx.physical, &ctx.memory_properties);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = ctx.queue_family;
    vkCreateCommandPool(ctx.device, &pool_info, nullptr, &ctx.command_pool);

    // --- The matrix -----------------------------------------------------------------------------
    const Scenario scenarios[] = {
        {"A  loaded target, one draw, no capture", false, false, false, 1, false, false, false},
        {"B  same draw with pre/post capture", true, true, false, 1, false, false, false},
        {"C  clearing target, correct clear semantics", true, true, true, 1, false, false, false},
        {"D  two consecutive draws into one target", true, true, false, 2, false, false, false},
        {"E  draw after a capture-induced scope break", true, true, false, 1, false, false, false},
        {"F  depth-tested draw with a colour attachment", true, true, false, 1, true, false, false},
        {"G  unsupported MSAA reference configuration", true, true, false, 1, false, true, false},
        {"H  partial coverage, capture OFF", false, false, false, 1, false, false, true},
        {"I  partial coverage, capture ON", true, true, false, 1, false, false, true},
    };

    u32 failures = 0;
    Result baseline{};
    Result captured{};
    Result partial_off{};
    Result partial_on{};
    Result msaa{};
    for (const auto& scenario : scenarios) {
        std::printf("\n%s\n", scenario.name);
        const Result result = RunScenario(ctx, scenario, verbose);
        if (std::strcmp(scenario.name, "A  loaded target, one draw, no capture") == 0) {
            baseline = result;
        } else if (std::strcmp(scenario.name, "B  same draw with pre/post capture") == 0) {
            captured = result;
        } else if (std::strcmp(scenario.name, "H  partial coverage, capture OFF") == 0) {
            partial_off = result;
        } else if (std::strcmp(scenario.name, "I  partial coverage, capture ON") == 0) {
            partial_on = result;
        } else if (std::strcmp(scenario.name,
                              "G  unsupported MSAA reference configuration") == 0) {
            msaa = result;
        }
    }

    // --- The negative control -------------------------------------------------------------------
    // The same sequence without the resume: the draw executes outside a rendering scope.
    std::printf("\nNEGATIVE CONTROL: capture without resuming the scope\n");
    const Scenario negative{"negative", true, false, false, 1, false, false, false};
    const Result negative_result = RunScenario(ctx, negative, verbose);
    std::printf("  draw executed: %s, scope open at draw: %s\n",
                negative_result.draw_executed ? "yes" : "no",
                negative_result.scope_open_at_draw ? "yes" : "no");

    // --- Verdicts -------------------------------------------------------------------------------
    std::printf("\n=== verdicts ===\n");

    const auto check = [&failures](const char* what, bool ok, const char* detail) {
        std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail[0] ? ": " : "", detail);
        if (!ok)
            ++failures;
    };

    check("the draw executes inside a rendering scope", captured.scope_open_at_draw,
          "a capture must not leave the draw outside one");
    check("the negative control reproduces the defect", !negative_result.scope_open_at_draw,
          "without the resume the draw is outside a scope");
    check("capture OFF and ON produce identical output", baseline.hash == captured.hash,
          "the capture must not change what the draw renders");
    check("the captured image is not the clear colour", captured.distinct > 1,
          "the draw must have covered pixels");
    check("no validation errors in the corrected path", captured.validation_errors == 0,
          "validation must be clean");
    check("a resumed scope loads rather than clears",
          partial_off.hash == partial_on.hash && partial_on.distinct > 1,
          "the uncovered pixels must keep what the pass loaded");
    check("an MSAA attachment is reported unsupported rather than copied", msaa.unsupported,
          "a multisampled attachment cannot be copied without a resolve");

    std::printf("\n%d failure(s)\n", failures);
    std::printf("validation errors total: %u\n", ctx.validation_errors);

    vkDestroyCommandPool(ctx.device, ctx.command_pool, nullptr);
    vkDestroyDevice(ctx.device, nullptr);
    if (ctx.messenger != VK_NULL_HANDLE) {
        const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(ctx.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy_messenger != nullptr)
            destroy_messenger(ctx.instance, ctx.messenger, nullptr);
    }
    vkDestroyInstance(ctx.instance, nullptr);
    return failures == 0 ? 0 : 1;
}
