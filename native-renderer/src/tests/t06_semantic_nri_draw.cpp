// t06_semantic_nri_draw: the decisive end-to-end test.
//
// A real indexed draw is recorded and submitted through the semantic RHI,
// executed by NRI on the GPU, into an externally owned two-layer Vulkan image.
// Output is verified by readback. Nothing is manufactured by a Vulkan clear.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

struct Vertex {
    float x, y;
    float r, g, b, a;
};

const Vertex kQuad[4] = {
    {-0.8f, -0.8f, 1, 1, 1, 1},
    { 0.8f, -0.8f, 1, 1, 1, 1},
    { 0.8f,  0.8f, 1, 1, 1, 1},
    {-0.8f,  0.8f, 1, 1, 1, 1},
};

const uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};

const char* kVs = R"(
#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = inColor;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
)";

const char* kFs = R"(
#version 450
layout(push_constant) uniform Push { vec4 color; } pc;
layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 o;
void main() {
    o = pc.color * vColor;
}
)";

}  // namespace

int main() {
    harness::TestLog log("t06_semantic_nri_draw");

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
    if (m.isSoftware) {
        log.Info("SOFTWARE device: results are not physical-GPU evidence");
    } else {
        log.Pass("physical GPU: " + m.deviceName);
    }

    const uint32_t W = 64, H = 64;
    harness::ImageDesc imgDesc;
    imgDesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgDesc.width = W;
    imgDesc.height = H;
    imgDesc.layerNum = 2;
    imgDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness::Image external;
    if (!harness::CreateImage(vk, imgDesc, external, error)) {
        log.Fail("external image: " + error);
        return log.Finish();
    }
    log.Pass("externally owned two-layer image created");

    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
    const float bg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    harness::ClearColor(vk, pool, external, bg, 0);
    harness::ClearColor(vk, pool, external, bg, 1);
    log.Pass("both layers cleared to black background");

    auto device = semantic::CreateNriDevice(vk, error);
    if (!device) {
        log.Fail("semantic device: " + error);
        return log.Finish();
    }
    log.Pass("semantic NRI device created");

    semantic::ExternalImageDesc extDesc;
    extDesc.nativeHandle = reinterpret_cast<void*>(external.image);
    extDesc.format = semantic::Format::kRGBA8Unorm;
    extDesc.width = W;
    extDesc.height = H;
    extDesc.layerNum = 2;
    extDesc.usage = semantic::TextureUsage::kColorAttachment |
                    semantic::TextureUsage::kTransferSrc;
    const auto texId = device->ImportExternalImage(extDesc, error);
    if (!texId.IsValid()) {
        log.Fail("import external image: " + error);
        return log.Finish();
    }
    log.Pass("external image imported into the semantic backend");

    semantic::TextureViewDesc vd;
    vd.texture = texId;
    vd.aspect = semantic::ImageAspect::kColor;
    vd.baseLayer = 0;
    vd.layerNum = 1;
    const auto view0 = device->CreateTextureView(vd, error);
    vd.baseLayer = 1;
    const auto view1 = device->CreateTextureView(vd, error);
    if (!view0.IsValid() || !view1.IsValid()) {
        log.Fail("texture views: " + error);
        return log.Finish();
    }
    log.Pass("per-layer views created");

    semantic::BufferDesc vbDesc;
    vbDesc.size = sizeof(kQuad);
    vbDesc.usage = semantic::BufferUsage::kVertex;
    const auto vb = device->CreateBuffer(vbDesc, error);
    if (!vb.IsValid() || !device->UploadBuffer(vb, kQuad, sizeof(kQuad), 0, error)) {
        log.Fail("vertex buffer: " + error);
        return log.Finish();
    }

    semantic::BufferDesc ibDesc;
    ibDesc.size = sizeof(kIndices);
    ibDesc.usage = semantic::BufferUsage::kIndex;
    const auto ib = device->CreateBuffer(ibDesc, error);
    if (!ib.IsValid() || !device->UploadBuffer(ib, kIndices, sizeof(kIndices), 0, error)) {
        log.Fail("index buffer: " + error);
        return log.Finish();
    }
    log.Pass("vertex and index buffers uploaded");

    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error)) {
        log.Fail("vertex shader: " + error);
        return log.Finish();
    }
    if (!harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
        log.Fail("pixel shader: " + error);
        return log.Finish();
    }

    semantic::ShaderDesc vsDesc;
    vsDesc.stage = semantic::ShaderStage::kVertex;
    vsDesc.bytecode = vsSpv;
    const auto vsId = device->CreateShader(vsDesc, error);

    semantic::ShaderDesc fsDesc;
    fsDesc.stage = semantic::ShaderStage::kPixel;
    fsDesc.bytecode = fsSpv;
    const auto fsId = device->CreateShader(fsDesc, error);
    if (!vsId.IsValid() || !fsId.IsValid()) {
        log.Fail("shader creation: " + error);
        return log.Finish();
    }

    semantic::GraphicsPipelineDesc pd;
    pd.vertexShader = vsId;
    pd.pixelShader = fsId;
    pd.vertexBindings.push_back({0, sizeof(Vertex)});
    pd.vertexAttributes.push_back({0, 0, semantic::Format::kR32G32Float, 0});
    pd.vertexAttributes.push_back({1, 0, semantic::Format::kR32G32B32A32Float, 8});
    pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
    const auto pipeId = device->CreateGraphicsPipeline(pd, error);
    if (!pipeId.IsValid()) {
        log.Fail("pipeline: " + error);
        return log.Finish();
    }
    log.Pass("graphics pipeline created");

    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};

    if (!device->BeginCommands(error)) {
        log.Fail("BeginCommands: " + error);
        return log.Finish();
    }

    device->TransitionResource(texId, semantic::ImageAspect::kColor,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kColorAttachment);

    for (int eye = 0; eye < 2; ++eye) {
        semantic::RenderPassDesc rp;
        semantic::ColorAttachment ca;
        ca.view = (eye == 0) ? view0 : view1;
        ca.loadOp = semantic::LoadOp::kLoad;
        ca.storeOp = semantic::StoreOp::kStore;
        rp.colorAttachments.push_back(ca);
        rp.width = W;
        rp.height = H;
        rp.layerCount = 1;

        device->BeginRenderPass(rp);
        device->BindPipeline(pipeId);
        semantic::Viewport vp;
        vp.width = static_cast<float>(W);
        vp.height = static_cast<float>(H);
        device->SetViewport(vp);
        semantic::Scissor sc;
        sc.width = W;
        sc.height = H;
        device->SetScissor(sc);
        device->SetVertexBuffer(0, vb, 0, sizeof(Vertex));
        device->SetIndexBuffer(ib, 0, semantic::IndexFormat::kUint16);

        semantic::ShaderParameterBlock params;
        params.data = (eye == 0) ? red : blue;
        params.sizeBytes = 16;
        params.visibility = semantic::ShaderStage::kPixel;
        device->SetShaderParameters(params);

        semantic::DrawIndexedDesc draw;
        draw.indexCount = 6;
        draw.instanceCount = 1;
        device->DrawIndexed(draw);
        device->EndRenderPass();
    }

    if (!device->EndCommands(error)) {
        log.Fail("EndCommands: " + error);
        return log.Finish();
    }

    uint64_t completion = 0;
    if (!device->Submit(completion, error)) {
        log.Fail("Submit: " + error);
        return log.Finish();
    }
    if (!device->LastSubmitUsedNri()) {
        log.Fail("submission did not go through NRI");
    } else {
        log.Pass("commands submitted through NRI, completion value " +
                 std::to_string(completion));
    }

    if (!device->WaitForCompletion(completion)) {
        log.Fail("WaitForCompletion failed");
        return log.Finish();
    }
    log.Pass("GPU completion observed");

    {
        std::vector<unsigned char> l0, l1;
        if (!device->ReadbackTexture(texId, 0, l0, error)) {
            log.Fail("readback layer 0: " + error);
        } else if (!device->ReadbackTexture(texId, 1, l1, error)) {
            log.Fail("readback layer 1: " + error);
        } else {
            const size_t centre = (static_cast<size_t>(H / 2) * W + (W / 2)) * 4;
            const unsigned char* p0 = l0.data() + centre;
            const unsigned char* p1 = l1.data() + centre;

            std::printf("  [info] layer 0 centre RGBA = %u,%u,%u,%u\n", p0[0], p0[1], p0[2], p0[3]);
            std::printf("  [info] layer 1 centre RGBA = %u,%u,%u,%u\n", p1[0], p1[1], p1[2], p1[3]);

            const unsigned char expectRed[4] = {255, 0, 0, 255};
            const unsigned char expectBlue[4] = {0, 0, 255, 255};
            if (harness::PixelNear(p0, expectRed, 2)) log.Pass("layer 0 centre is red");
            else log.Fail("layer 0 centre is not red");
            if (harness::PixelNear(p1, expectBlue, 2)) log.Pass("layer 1 centre is blue");
            else log.Fail("layer 1 centre is not blue");

            const unsigned char* c0 = l0.data();
            const unsigned char expectBg[4] = {0, 0, 0, 255};
            if (harness::PixelNear(c0, expectBg, 2)) log.Pass("layer 0 corner is untouched background");
            else log.Fail("layer 0 corner was overwritten");

            if (!(p0[2] > 200 && p0[0] < 50)) log.Pass("no cross-layer contamination");
            else log.Fail("layer 0 contains blue");
        }
    }

    device->Destroy(texId);
    device->Destroy(view0);
    device->Destroy(view1);
    device->Destroy(vb);
    device->Destroy(ib);
    device->Destroy(vsId);
    device->Destroy(fsId);
    device->Destroy(pipeId);
    device.reset();
    log.Pass("semantic backend destroyed");

    {
        std::vector<unsigned char> check;
        if (harness::ReadbackColor(vk, pool, external, 0, check, error)) {
            log.Pass("external Vulkan image still valid after backend teardown");
        } else {
            log.Fail("external image invalid after teardown: " + error);
        }
    }

    harness::DestroyImage(vk, external);
    vkDestroyCommandPool(vk.device(), pool, nullptr);
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
