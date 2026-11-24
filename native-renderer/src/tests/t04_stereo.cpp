// t04_stereo: sequential and multiview stereo rendering into a two-layer image,
// verifying per-layer isolation and view-mask semantics.
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

const char* kVs = R"(
#version 450
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Sequential: the fragment color comes from a push constant, so each draw can
// target a different layer with a different color.
const char* kFsPush = R"(
#version 450
layout(push_constant) uniform Push { vec4 color; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
)";

// Multiview: the color is selected by gl_ViewIndex, proving per-view semantics.
const char* kFsView = R"(
#version 450
#extension GL_EXT_multiview : require
layout(location = 0) out vec4 o;
void main() {
    if (gl_ViewIndex == 0) o = vec4(1.0, 0.0, 0.0, 1.0);   // left  = red
    else                   o = vec4(0.0, 0.0, 1.0, 1.0);   // right = blue
}
)";

struct Pipe {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

bool BuildPipe(harness::VulkanContext& vk, const char* fsSrc, VkFormat colorFmt,
               uint32_t viewMask, bool usePush, Pipe& out, std::string& error) {
    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error)) return false;
    if (!harness::CompileGlsl(fsSrc, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error))
        return false;

    out.vs = harness::CreateShaderModule(vk, vsSpv, error);
    out.fs = harness::CreateShaderModule(vk, fsSpv, error);
    if (!out.vs || !out.fs) return false;

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = 16;

    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (usePush) {
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
    }
    if (vkCreatePipelineLayout(vk.device(), &lci, nullptr, &out.layout) != VK_SUCCESS) {
        error = "layout failed";
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
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
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
    prc.viewMask = viewMask;

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
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dsi;
    gp.layout = out.layout;

    if (vkCreateGraphicsPipelines(vk.device(), VK_NULL_HANDLE, 1, &gp, nullptr, &out.pipeline) !=
        VK_SUCCESS) {
        error = "pipeline failed";
        return false;
    }
    return true;
}

void DestroyPipe(harness::VulkanContext& vk, Pipe& p) {
    if (p.pipeline) vkDestroyPipeline(vk.device(), p.pipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(vk.device(), p.layout, nullptr);
    if (p.vs) vkDestroyShaderModule(vk.device(), p.vs, nullptr);
    if (p.fs) vkDestroyShaderModule(vk.device(), p.fs, nullptr);
}

}  // namespace

int main() {
    harness::TestLog log("t04_stereo");

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
        log.Unsupported("dynamicRendering required");
        return log.Finish();
    }

    harness::NriContext nri;
    if (!nri.Initialize(vk, true, error)) {
        log.Fail("NRI init: " + error);
        return log.Finish();
    }

    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
    const uint32_t W = 32, H = 32;

    harness::ImageDesc d;
    d.format = VK_FORMAT_R8G8B8A8_UNORM;
    d.width = W;
    d.height = H;
    d.layerNum = 2;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image img;
    if (!harness::CreateImage(vk, d, img, error)) {
        log.Fail("stereo image: " + error);
        return log.Finish();
    }

    // Per-layer views for sequential rendering.
    VkImageView layer0 = harness::CreateView(vk, img.image, d.format, VK_IMAGE_ASPECT_COLOR_BIT,
                                             0, 1, 0, 1, VK_IMAGE_VIEW_TYPE_2D, error);
    VkImageView layer1 = harness::CreateView(vk, img.image, d.format, VK_IMAGE_ASPECT_COLOR_BIT,
                                             0, 1, 1, 1, VK_IMAGE_VIEW_TYPE_2D, error);
    // Array view spanning both layers for multiview.
    VkImageView arrayView = harness::CreateView(vk, img.image, d.format,
                                                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2,
                                                VK_IMAGE_VIEW_TYPE_2D_ARRAY, error);
    if (!layer0 || !layer1 || !arrayView) {
        log.Fail("view creation: " + error);
        return log.Finish();
    }
    log.Pass("per-layer and array views created");

    nri::Texture* nriTex = nullptr;
    if (nri.WrapTexture(img.image, d.format, VK_IMAGE_TYPE_2D, d.usage, W, H, 1, 1, 2, 1,
                        nriTex, error)) {
        log.Pass("two-layer image wrapped by NRI");
    } else {
        log.Fail("NRI wrap: " + error);
    }

    // ---- Sequential stereo: two draws, one per layer ----
    {
        Pipe pipe;
        if (!BuildPipe(vk, kFsPush, d.format, 0, true, pipe, error)) {
            log.Fail("sequential pipeline: " + error);
        } else {
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 0);
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 1);

            VkCommandBuffer cmd = vk.BeginOneShot(pool);
            harness::TransitionImage(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 2,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

            const float left[4] = {1, 0, 0, 1};
            const float right[4] = {0, 0, 1, 1};
            for (int eye = 0; eye < 2; ++eye) {
                VkRenderingAttachmentInfo att{};
                att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                att.imageView = (eye == 0) ? layer0 : layer1;
                att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

                VkRenderingInfo ri{};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea = {{0, 0}, {W, H}};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &att;

                vkCmdBeginRendering(cmd, &ri);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
                VkViewport vpt{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
                VkRect2D sc{{0, 0}, {W, H}};
                vkCmdSetViewport(cmd, 0, 1, &vpt);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdPushConstants(cmd, pipe.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16,
                                   eye == 0 ? left : right);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }
            vk.EndOneShot(pool, cmd);

            std::vector<unsigned char> l0, l1;
            const bool ok0 = harness::ReadbackColor(vk, pool, img, 0, l0, error);
            const bool ok1 = harness::ReadbackColor(vk, pool, img, 1, l1, error);
            const unsigned char red[4] = {255, 0, 0, 255};
            const unsigned char blue[4] = {0, 0, 255, 255};
            if (ok0 && harness::PixelNear(l0.data(), red, 2)) log.Pass("sequential: layer 0 is red");
            else log.Fail("sequential: layer 0 wrong");
            if (ok1 && harness::PixelNear(l1.data(), blue, 2)) log.Pass("sequential: layer 1 is blue");
            else log.Fail("sequential: layer 1 wrong");
            DestroyPipe(vk, pipe);
        }
    }

    // ---- Multiview: one draw, viewMask selects both layers ----
    if (!m.multiview || m.maxViewCount < 2) {
        log.Unsupported("multiview with 2 views unavailable");
    } else {
        Pipe pipe;
        if (!BuildPipe(vk, kFsView, d.format, 0x3, false, pipe, error)) {
            log.Fail("multiview pipeline: " + error);
        } else {
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 0);
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 1);

            VkCommandBuffer cmd = vk.BeginOneShot(pool);
            harness::TransitionImage(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 2,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView = arrayView;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {W, H}};
            ri.layerCount = 1;
            ri.viewMask = 0x3;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;

            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
            VkViewport vpt{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            VkRect2D sc{{0, 0}, {W, H}};
            vkCmdSetViewport(cmd, 0, 1, &vpt);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            vk.EndOneShot(pool, cmd);

            std::vector<unsigned char> l0, l1;
            const bool ok0 = harness::ReadbackColor(vk, pool, img, 0, l0, error);
            const bool ok1 = harness::ReadbackColor(vk, pool, img, 1, l1, error);
            const unsigned char red[4] = {255, 0, 0, 255};
            const unsigned char blue[4] = {0, 0, 255, 255};
            if (ok0 && harness::PixelNear(l0.data(), red, 2)) {
                log.Pass("multiview: view 0 wrote red to layer 0");
            } else {
                log.Fail("multiview: layer 0 wrong");
            }
            if (ok1 && harness::PixelNear(l1.data(), blue, 2)) {
                log.Pass("multiview: view 1 wrote blue to layer 1");
            } else {
                log.Fail("multiview: layer 1 wrong");
            }
            DestroyPipe(vk, pipe);
        }
    }

    // ---- View mask 1: only the first view should be written ----
    if (m.multiview && m.maxViewCount >= 2) {
        Pipe pipe;
        if (BuildPipe(vk, kFsView, d.format, 0x1, false, pipe, error)) {
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 0);
            harness::ClearColor(vk, pool, img, (const float[4]){0, 0, 0, 1}, 1);

            VkCommandBuffer cmd = vk.BeginOneShot(pool);
            harness::TransitionImage(cmd, img.image, VK_IMAGE_ASPECT_COLOR_BIT,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1, 0, 2,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     VK_ACCESS_MEMORY_WRITE_BIT,
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo att{};
            att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            att.imageView = arrayView;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{};
            ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            ri.renderArea = {{0, 0}, {W, H}};
            ri.layerCount = 1;
            ri.viewMask = 0x1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe.pipeline);
            VkViewport vpt{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
            VkRect2D sc{{0, 0}, {W, H}};
            vkCmdSetViewport(cmd, 0, 1, &vpt);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
            vk.EndOneShot(pool, cmd);

            std::vector<unsigned char> l0, l1;
            const bool ok0 = harness::ReadbackColor(vk, pool, img, 0, l0, error);
            const bool ok1 = harness::ReadbackColor(vk, pool, img, 1, l1, error);
            const unsigned char red[4] = {255, 0, 0, 255};
            const unsigned char black[4] = {0, 0, 0, 255};
            if (ok0 && harness::PixelNear(l0.data(), red, 2)) {
                log.Pass("viewMask=1: layer 0 written");
            } else {
                log.Fail("viewMask=1: layer 0 not written");
            }
            if (ok1 && harness::PixelNear(l1.data(), black, 2)) {
                log.Pass("viewMask=1: layer 1 UNTOUCHED (isolation verified)");
            } else {
                log.Fail("viewMask=1: layer 1 was modified");
            }
            DestroyPipe(vk, pipe);
        }
    }

    if (nriTex) nri.DestroyWrappedTexture(nriTex);
    vkDestroyImageView(vk.device(), layer0, nullptr);
    vkDestroyImageView(vk.device(), layer1, nullptr);
    vkDestroyImageView(vk.device(), arrayView, nullptr);
    harness::DestroyImage(vk, img);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
    nri.Shutdown();
    vk.Shutdown();

    const auto& counters = harness::GlobalValidationCounters();
    if (counters.errors == 0) {
        log.Pass("zero validation errors");
    } else {
        log.Fail(std::to_string(counters.errors) + " validation errors");
        for (const auto& msg : counters.messages) log.Info(msg);
    }

    return log.Finish();
}
