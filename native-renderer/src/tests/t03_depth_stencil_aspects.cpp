// t03_depth_stencil_aspects: validate that depth and stencil aspects can be
// addressed independently, and that a combined attachment view does not imply
// writes to both aspects.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/nri_adoption.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

// A minimal fullscreen-triangle pipeline that writes depth only, leaving
// stencil untouched. Uses dynamic rendering so no render pass object is needed.
struct DepthOnlyPipeline {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

const char* kVs = R"(
#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Writes a constant depth value; never touches stencil.
// gl_FragDepth must be written explicitly, otherwise the interpolated
// gl_Position.z (0.0 here) is used instead.
const char* kFsDepth = R"(
#version 450
layout(location = 0) out vec4 o;
void main() {
    gl_FragDepth = 0.75;
    o = vec4(1.0, 0.0, 1.0, 1.0);
}
)";

bool BuildDepthOnly(harness::VulkanContext& vk, VkFormat colorFmt, VkFormat dsFmt,
                    DepthOnlyPipeline& out, std::string& error) {
    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error)) return false;
    if (!harness::CompileGlsl(kFsDepth, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error))
        return false;

    out.vs = harness::CreateShaderModule(vk, vsSpv, error);
    out.fs = harness::CreateShaderModule(vk, fsSpv, error);
    if (!out.vs || !out.fs) return false;

    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(vk.device(), &lci, nullptr, &out.layout) != VK_SUCCESS) {
        error = "pipeline layout failed";
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = out.vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = out.fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test always passes and writes 0.75. Stencil is disabled entirely,
    // so the stencil aspect must remain untouched.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prc{};
    prc.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    prc.colorAttachmentCount = 1;
    prc.pColorAttachmentFormats = &colorFmt;
    prc.depthAttachmentFormat = dsFmt;
    prc.stencilAttachmentFormat = dsFmt;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.pNext = &prc;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsi;
    gp.layout = out.layout;

    if (vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &out.pipeline) !=
        VK_SUCCESS) {
        error = "graphics pipeline failed";
        return false;
    }
    return true;
}

void DestroyDepthOnly(harness::VulkanContext& vk, DepthOnlyPipeline& p) {
    if (p.pipeline) vkDestroyPipeline(vk.device(), p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(vk.device(), p.layout, nullptr);
    if (p.vs) vkDestroyShaderModule(vk.device(), p.vs, nullptr);
    if (p.fs) vkDestroyShaderModule(vk.device(), p.fs, nullptr);
}

}  // namespace

int main() {
    harness::TestLog log("t03_depth_stencil_aspects");

    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }
    const auto& m = vk.manifest();
    if (!m.dynamicRendering) {
        log.Unsupported("dynamicRendering unavailable; test requires it");
        return log.Finish();
    }

    harness::NriContext nri;
    if (!nri.Initialize(vk, true, error)) {
        log.Fail("NRI init: " + error);
        return log.Finish();
    }

    const VkFormat dsFmt = harness::PickDepthStencilFormat(vk, true);
    if (dsFmt == VK_FORMAT_UNDEFINED) {
        log.Unsupported("no sampled combined depth/stencil format");
        return log.Finish();
    }
    log.Pass("combined depth/stencil format selected: " + std::to_string(dsFmt));

    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());

    const uint32_t W = 32, H = 32;
    harness::ImageDesc colorDesc;
    colorDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorDesc.width = W;
    colorDesc.height = H;
    colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image color;
    if (!harness::CreateImage(vk, colorDesc, color, error)) {
        log.Fail("color image: " + error);
        return log.Finish();
    }

    harness::ImageDesc dsDesc;
    dsDesc.format = dsFmt;
    dsDesc.width = W;
    dsDesc.height = H;
    dsDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image ds;
    if (!harness::CreateImage(vk, dsDesc, ds, error)) {
        log.Fail("depth/stencil image: " + error);
        return log.Finish();
    }

    // ---- Aspect views ----
    VkImageView depthOnly = harness::CreateView(vk, ds.image, dsFmt, VK_IMAGE_ASPECT_DEPTH_BIT,
                                                0, 1, 0, 1, VK_IMAGE_VIEW_TYPE_2D, error);
    VkImageView stencilOnly = harness::CreateView(vk, ds.image, dsFmt,
                                                  VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1,
                                                  VK_IMAGE_VIEW_TYPE_2D, error);
    VkImageView combined = harness::CreateView(vk, ds.image, dsFmt,
                                               VK_IMAGE_ASPECT_DEPTH_BIT |
                                                   VK_IMAGE_ASPECT_STENCIL_BIT,
                                               0, 1, 0, 1, VK_IMAGE_VIEW_TYPE_2D, error);
    if (depthOnly && stencilOnly && combined) {
        log.Pass("depth-only, stencil-only and combined views all created");
    } else {
        log.Fail("aspect view creation failed: " + error);
    }

    // ---- NRI wrapping of the same image ----
    nri::Texture* nriTex = nullptr;
    if (nri.WrapTexture(ds.image, dsFmt, VK_IMAGE_TYPE_2D, dsDesc.usage, W, H, 1, 1, 1, 1,
                        nriTex, error)) {
        log.Pass("depth/stencil image wrapped by NRI");
    } else {
        log.Fail("NRI wrap of depth/stencil image: " + error);
    }

    // ---- Initialise depth and stencil to distinguishable values ----
    harness::ClearDepthStencil(vk, pool, ds, 0.25f, 0x5A, 0);
    {
        std::vector<float> d;
        std::vector<unsigned char> s;
        const bool dok = harness::ReadbackDepth(vk, pool, ds, 0, d, error);
        const bool sok = harness::ReadbackStencil(vk, pool, ds, 0, s, error);
        if (dok && d[0] > 0.24f && d[0] < 0.26f) log.Pass("initial depth is 0.25");
        else log.Fail("initial depth wrong");
        if (sok && s[0] == 0x5A) log.Pass("initial stencil is 0x5A");
        else log.Fail("initial stencil wrong");
    }

    // ---- Modify depth only, verify stencil preserved ----
    {
        DepthOnlyPipeline pipe;
        if (!BuildDepthOnly(vk, colorDesc.format, dsFmt, pipe, error)) {
            log.Fail("depth-only pipeline: " + error);
        } else {
            VkCommandBuffer cmd = vk.BeginOneShot(pool);

            harness::TransitionImage(cmd, ds.image,
                                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1, 0,
                                     1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            // The color image has never been used, so its layout is UNDEFINED.
            harness::TransitionImage(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 1,
                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo colorAtt{};
            colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAtt.imageView = harness::CreateView(vk, color.image, colorDesc.format,
                                                     VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                                                     VK_IMAGE_VIEW_TYPE_2D, error);
            colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

            VkRenderingAttachmentInfo dsAtt{};
            dsAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            dsAtt.imageView = combined;
            dsAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            dsAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve stencil
            dsAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {W, H}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &colorAtt;
            ri.pDepthAttachment = &dsAtt;
            ri.pStencilAttachment = &dsAtt;

            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
            VkViewport vpt{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            VkRect2D sc{{0, 0}, {W, H}};
            vkCmdSetViewport(cmd, 0, 1, &vpt);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);

            vk.EndOneShot(pool, cmd);
            vkDestroyImageView(vk.device(), colorAtt.imageView, nullptr);

            std::vector<float> d;
            std::vector<unsigned char> s;
            const bool dok = harness::ReadbackDepth(vk, pool, ds, 0, d, error);
            const bool sok = harness::ReadbackStencil(vk, pool, ds, 0, s, error);
            if (dok && d[0] > 0.74f && d[0] < 0.76f) {
                log.Pass("depth written to 0.75 by the depth-only pipeline");
            } else {
                log.Fail("depth not written as expected");
            }
            if (sok && s[0] == 0x5A) {
                log.Pass("stencil PRESERVED at 0x5A across a depth-only write");
            } else {
                log.Fail("stencil was modified by a depth-only write");
            }
            DestroyDepthOnly(vk, pipe);
        }
    }

    // ---- Independent per-aspect layout transitions ----
    if (m.separateDepthStencilLayouts) {
        VkCommandBuffer cmd = vk.BeginOneShot(pool);
        harness::TransitionImage(cmd, ds.image, VK_IMAGE_ASPECT_DEPTH_BIT,
                                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, 0, 1, 0, 1,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
        harness::TransitionImage(cmd, ds.image, VK_IMAGE_ASPECT_STENCIL_BIT,
                                 VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL,
                                 VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL, 0, 1, 0, 1,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT);
        vk.EndOneShot(pool, cmd);
        log.Pass("independent depth and stencil layout transitions executed");
    } else {
        log.Unsupported("separateDepthStencilLayouts unavailable; combined fallback only");
    }

    // ---- NRI view aspect inspection ----
    if (nriTex != nullptr) {
        nri::TextureViewDesc vd{};
        vd.texture = nriTex;
        vd.format = nri::Format::UNKNOWN;
        vd.type = nri::TextureView::TEXTURE;
        vd.mipNum = 1;
        vd.layerNum = 1;

        vd.planes = nri::PlaneBits::DEPTH;
        nri::Descriptor* depthView = nullptr;
        const nri::Result r = nri.core().CreateTextureView(vd, depthView);
        if (r == nri::Result::SUCCESS && depthView != nullptr) {
            log.Pass("NRI created a depth-only texture view");
            nri.core().DestroyDescriptor(depthView);
        } else {
            log.Info("NRI depth-only view creation returned " +
                     std::to_string(static_cast<int>(r)));
        }

        vd.planes = nri::PlaneBits::STENCIL;
        nri::Descriptor* stencilView = nullptr;
        const nri::Result r2 = nri.core().CreateTextureView(vd, stencilView);
        if (r2 == nri::Result::SUCCESS && stencilView != nullptr) {
            log.Pass("NRI created a stencil-only texture view");
            nri.core().DestroyDescriptor(stencilView);
        } else {
            log.Info("NRI stencil-only view creation returned " +
                     std::to_string(static_cast<int>(r2)));
        }

        vd.planes = nri::PlaneBits::DEPTH | nri::PlaneBits::STENCIL;
        nri::Descriptor* combinedView = nullptr;
        const nri::Result r3 = nri.core().CreateTextureView(vd, combinedView);
        if (r3 == nri::Result::SUCCESS && combinedView != nullptr) {
            log.Pass("NRI created a combined depth+stencil texture view");
            nri.core().DestroyDescriptor(combinedView);
        } else {
            log.Info("NRI combined view creation returned " +
                     std::to_string(static_cast<int>(r3)));
        }
        nri.DestroyWrappedTexture(nriTex);
    }

    if (depthOnly) vkDestroyImageView(vk.device(), depthOnly, nullptr);
    if (stencilOnly) vkDestroyImageView(vk.device(), stencilOnly, nullptr);
    if (combined) vkDestroyImageView(vk.device(), combined, nullptr);
    harness::DestroyImage(vk, ds);
    harness::DestroyImage(vk, color);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
    nri.Shutdown();
    vk.Shutdown();

    const uint32_t unexpected = harness::UnexpectedErrorCount();
    if (unexpected == 0) {
        log.Pass("zero unexpected validation errors");
    } else {
        log.Fail(std::to_string(unexpected) + " unexpected validation errors");
        for (const auto& msg : harness::GlobalValidationCounters().messages) log.Info(msg);
    }
    if (!harness::AllExpectationsMet()) {
        log.Fail("a declared expected diagnostic never fired");
    }

    return log.Finish();
}
