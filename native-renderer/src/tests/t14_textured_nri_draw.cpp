// t14_textured_nri_draw: a real texture-sampled indexed draw through NRI.
//
// The decisive proof: pixels come from a texture bound through an NRI descriptor
// set and sampled by the fragment shader. Nothing is hard-coded in the shader and
// nothing is manufactured by a Vulkan clear.
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

// Vertex: position (vec2) + UV (vec2). 16 bytes.
struct Vertex {
    float x, y;
    float u, v;
};

// A quad covering most of the target. UVs map the full texture.
const Vertex kQuad[4] = {
    {-0.8f, -0.8f, 0.0f, 1.0f},
    { 0.8f, -0.8f, 1.0f, 1.0f},
    { 0.8f,  0.8f, 1.0f, 0.0f},
    {-0.8f,  0.8f, 0.0f, 0.0f},
};
const uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};

const char* kVs = R"(
#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 vUV;
void main() {
    vUV = inUV;
    gl_Position = vec4(inPos, 0.0, 1.0);
}
)";

// Samples the bound texture. The output color is entirely texture-derived.
const char* kFs = R"(
#version 450
layout(set = 0, binding = 0) uniform texture2D tex;
layout(set = 0, binding = 1) uniform sampler samp;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 o;
void main() {
    o = texture(sampler2D(tex, samp), vUV);
}
)";

constexpr uint32_t kTexSize = 8;

// Four distinct quadrants: TL red, TR green, BL blue, BR yellow.
void MakeQuadrantTexture(std::vector<unsigned char>& out) {
    out.assign(kTexSize * kTexSize * 4, 0);
    for (uint32_t y = 0; y < kTexSize; ++y) {
        for (uint32_t x = 0; x < kTexSize; ++x) {
            const bool right = x >= kTexSize / 2;
            const bool bottom = y >= kTexSize / 2;
            unsigned char r = 0, g = 0, b = 0;
            if (!right && !bottom) { r = 255; }                 // TL red
            else if (right && !bottom) { g = 255; }             // TR green
            else if (!right && bottom) { b = 255; }             // BL blue
            else { r = 255; g = 255; }                          // BR yellow
            const size_t i = (static_cast<size_t>(y) * kTexSize + x) * 4;
            out[i + 0] = r;
            out[i + 1] = g;
            out[i + 2] = b;
            out[i + 3] = 255;
        }
    }
}

// A second, deliberately different texture: solid magenta.
void MakeSolidTexture(std::vector<unsigned char>& out, unsigned char r, unsigned char g,
                      unsigned char b) {
    out.assign(kTexSize * kTexSize * 4, 0);
    for (uint32_t i = 0; i < kTexSize * kTexSize; ++i) {
        out[i * 4 + 0] = r;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = b;
        out[i * 4 + 3] = 255;
    }
}

// Reads a pixel from a readback buffer.
const unsigned char* At(const std::vector<unsigned char>& buf, uint32_t w, uint32_t x,
                        uint32_t y) {
    return buf.data() + (static_cast<size_t>(y) * w + x) * 4;
}

void ReportPixel(const char* label, const unsigned char* p) {
    std::printf("  [info] %s = %u,%u,%u,%u\n", label, p[0], p[1], p[2], p[3]);
}

}  // namespace

int main() {
    harness::TestLog log("t14_textured_nri_draw");

    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }
    log.Pass("physical GPU: " + vk.manifest().deviceName);

    const uint32_t W = 64, H = 64;

    // ---- Externally owned two-layer target ----
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
    VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
    const float bg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    harness::ClearColor(vk, pool, external, bg, 0);
    harness::ClearColor(vk, pool, external, bg, 1);
    log.Pass("externally owned two-layer image created and cleared");

    auto device = semantic::CreateNriDevice(vk, error);
    if (!device) {
        log.Fail("semantic device: " + error);
        return log.Finish();
    }
    auto* nri = static_cast<harness::NriDevice*>(device.get());
    log.Pass("semantic NRI device created");

    // ---- Import the target ----
    semantic::ExternalImageDesc extDesc;
    extDesc.nativeHandle = reinterpret_cast<void*>(external.image);
    extDesc.format = semantic::Format::kRGBA8Unorm;
    extDesc.width = W;
    extDesc.height = H;
    extDesc.layerNum = 2;
    extDesc.usage = semantic::TextureUsage::kColorAttachment |
                    semantic::TextureUsage::kTransferSrc;
    const auto target = device->ImportExternalImage(extDesc, error);
    if (!target.IsValid()) {
        log.Fail("import target: " + error);
        return log.Finish();
    }
    semantic::TextureViewDesc vd;
    vd.texture = target;
    vd.aspect = semantic::ImageAspect::kColor;
    vd.kind = semantic::ViewKind::kColorAttachment;
    vd.baseLayer = 0;
    const auto view0 = device->CreateTextureView(vd, error);
    vd.baseLayer = 1;
    const auto view1 = device->CreateTextureView(vd, error);
    if (!view0.IsValid() || !view1.IsValid()) {
        log.Fail("target views: " + error);
        return log.Finish();
    }
    log.Pass("per-layer render-target views created");

    // ---- Sampled texture, uploaded through the backend ----
    semantic::TextureDesc td;
    td.format = semantic::Format::kRGBA8Unorm;
    td.width = kTexSize;
    td.height = kTexSize;
    td.usage = semantic::TextureUsage::kSampled;
    const auto tex = device->CreateTexture(td, error);
    if (!tex.IsValid()) {
        log.Fail("sampled texture create: " + error);
        return log.Finish();
    }
    std::vector<unsigned char> pixels;
    MakeQuadrantTexture(pixels);
    semantic::TextureUploadDesc up;
    up.texture = tex;
    up.data = pixels.data();
    up.width = kTexSize;
    up.height = kTexSize;
    up.rowPitch = kTexSize * 4;
    if (!device->UploadTexture(up, error)) {
        log.Fail("texture upload: " + error);
        return log.Finish();
    }
    log.Pass("sampled texture uploaded through the NRI backend");

    // ---- Sampled view + sampler + descriptor set ----
    semantic::TextureViewDesc svd;
    svd.texture = tex;
    svd.aspect = semantic::ImageAspect::kColor;
    svd.kind = semantic::ViewKind::kSampled;
    const auto sampledView = device->CreateTextureView(svd, error);
    if (!sampledView.IsValid()) {
        log.Fail("sampled view: " + error);
        return log.Finish();
    }
    semantic::SamplerDesc sampDesc;
    sampDesc.minFilter = semantic::Filter::kPoint;
    sampDesc.magFilter = semantic::Filter::kPoint;
    sampDesc.addressU = semantic::AddressMode::kClamp;
    sampDesc.addressV = semantic::AddressMode::kClamp;
    const auto sampler = device->CreateSampler(sampDesc, error);
    if (!sampler.IsValid()) {
        log.Fail("sampler: " + error);
        return log.Finish();
    }
    semantic::DescriptorSetDesc dsDesc;
    dsDesc.sampledView = sampledView;
    dsDesc.sampler = sampler;
    const auto descriptorSet = device->CreateDescriptorSet(dsDesc, error);
    if (!descriptorSet.IsValid()) {
        log.Fail("descriptor set: " + error);
        return log.Finish();
    }
    log.Pass("sampled view, sampler and descriptor set created");

    // ---- Geometry and pipeline ----
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

    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error) ||
        !harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
        log.Fail("shader compile: " + error);
        return log.Finish();
    }
    semantic::ShaderDesc vsd;
    vsd.stage = semantic::ShaderStage::kVertex;
    vsd.bytecode = vsSpv;
    const auto vs = device->CreateShader(vsd, error);
    semantic::ShaderDesc fsd;
    fsd.stage = semantic::ShaderStage::kPixel;
    fsd.bytecode = fsSpv;
    const auto fs = device->CreateShader(fsd, error);
    semantic::GraphicsPipelineDesc pd;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.vertexBindings.push_back({0, sizeof(Vertex)});
    pd.vertexAttributes.push_back({0, 0, semantic::Format::kR32G32Float, 0});
    pd.vertexAttributes.push_back({1, 0, semantic::Format::kR32G32Float, 8});
    pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
    const auto pipe = device->CreateGraphicsPipeline(pd, error);
    if (!pipe.IsValid()) {
        log.Fail("pipeline: " + error);
        return log.Finish();
    }
    log.Pass("textured graphics pipeline created");

    // ---- Record and submit ----
    if (!device->BeginCommands(error)) {
        log.Fail("BeginCommands: " + error);
        return log.Finish();
    }
    device->TransitionResource(target, semantic::ImageAspect::kColor,
                               semantic::ResourceState::kUndefined,
                               semantic::ResourceState::kColorAttachment);
    for (int eye = 0; eye < 2; ++eye) {
        semantic::RenderPassDesc rp;
        semantic::ColorAttachment ca;
        ca.view = (eye == 0) ? view0 : view1;
        ca.loadOp = semantic::LoadOp::kLoad;
        rp.colorAttachments.push_back(ca);
        rp.width = W;
        rp.height = H;
        device->BeginRenderPass(rp);
        device->BindPipeline(pipe);
        device->BindDescriptorSet(descriptorSet);
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
        semantic::DrawIndexedDesc draw;
        draw.indexCount = 6;
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
    if (!device->LastSubmitUsedNri()) log.Fail("submission did not go through NRI");
    else log.Pass("textured draw submitted through NRI");
    device->WaitForCompletion(completion);
    log.Pass("GPU completion observed");

    // ---- Verify pixels ----
    {
        std::vector<unsigned char> l0, l1;
        if (!device->ReadbackTexture(target, 0, l0, error) ||
            !device->ReadbackTexture(target, 1, l1, error)) {
            log.Fail("readback: " + error);
        } else {
            // The quad spans roughly x,y in [6,57]. Sample inside each quadrant.
            const uint32_t q = 16;
            const uint32_t xs[2] = {W / 2 - q, W / 2 + q};
            const uint32_t ys[2] = {H / 2 - q, H / 2 + q};
            const unsigned char expect[4][4] = {
                {255, 0, 0, 255},      // TL red
                {0, 255, 0, 255},      // TR green
                {0, 0, 255, 255},      // BL blue
                {255, 255, 0, 255},    // BR yellow
            };
            const char* names[4] = {"TL red", "TR green", "BL blue", "BR yellow"};

            for (int layer = 0; layer < 2; ++layer) {
                const auto& buf = (layer == 0) ? l0 : l1;
                int ok = 0;
                for (int i = 0; i < 4; ++i) {
                    const uint32_t x = xs[i % 2];
                    const uint32_t y = ys[i / 2];
                    const unsigned char* p = At(buf, W, x, y);
                    char label[64];
                    std::snprintf(label, sizeof(label), "layer %d %s", layer, names[i]);
                    ReportPixel(label, p);
                    if (harness::PixelNear(p, expect[i], 4)) ++ok;
                }
                if (ok == 4) {
                    log.Pass("layer " + std::to_string(layer) +
                             ": all four quadrants match the sampled texture");
                } else {
                    log.Fail("layer " + std::to_string(layer) + ": " +
                             std::to_string(ok) + " of 4 quadrants matched");
                }
                const unsigned char* corner = At(buf, W, 0, 0);
                const unsigned char expectBg[4] = {0, 0, 0, 255};
                if (harness::PixelNear(corner, expectBg, 2)) {
                    log.Pass("layer " + std::to_string(layer) + ": corner is untouched background");
                } else {
                    log.Fail("layer " + std::to_string(layer) + ": corner was overwritten");
                }
            }
        }
    }

    // ---- Negative control: a different texture must change the output ----
    {
        const auto tex2 = device->CreateTexture(td, error);
        std::vector<unsigned char> solid;
        MakeSolidTexture(solid, 255, 0, 255);   // magenta
        semantic::TextureUploadDesc up2;
        up2.texture = tex2;
        up2.data = solid.data();
        up2.width = kTexSize;
        up2.height = kTexSize;
        up2.rowPitch = kTexSize * 4;
        if (!tex2.IsValid() || !device->UploadTexture(up2, error)) {
            log.Fail("negative control texture: " + error);
        } else {
            semantic::TextureViewDesc svd2;
            svd2.texture = tex2;
            svd2.aspect = semantic::ImageAspect::kColor;
            svd2.kind = semantic::ViewKind::kSampled;
            const auto view2 = device->CreateTextureView(svd2, error);
            semantic::DescriptorSetDesc ds2;
            ds2.sampledView = view2;
            ds2.sampler = sampler;
            const auto set2 = device->CreateDescriptorSet(ds2, error);
            if (!set2.IsValid()) {
                log.Fail("negative control descriptor set: " + error);
            } else {
                if (!device->BeginCommands(error)) log.Fail("Begin: " + error);
                device->TransitionResource(target, semantic::ImageAspect::kColor,
                                           semantic::ResourceState::kColorAttachment,
                                           semantic::ResourceState::kColorAttachment);
                semantic::RenderPassDesc rp;
                semantic::ColorAttachment ca;
                ca.view = view0;
                ca.loadOp = semantic::LoadOp::kClear;
                ca.clearColor[0] = 0; ca.clearColor[1] = 0;
                ca.clearColor[2] = 0; ca.clearColor[3] = 1;
                rp.colorAttachments.push_back(ca);
                rp.width = W;
                rp.height = H;
                device->BeginRenderPass(rp);
                device->BindPipeline(pipe);
                device->BindDescriptorSet(set2);
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
                semantic::DrawIndexedDesc draw;
                draw.indexCount = 6;
                device->DrawIndexed(draw);
                device->EndRenderPass();
                if (!device->EndCommands(error)) log.Fail("End: " + error);
                uint64_t v = 0;
                if (!device->Submit(v, error)) log.Fail("Submit: " + error);
                device->WaitForCompletion(v);

                std::vector<unsigned char> out;
                if (!device->ReadbackTexture(target, 0, out, error)) {
                    log.Fail("negative control readback: " + error);
                } else {
                    const unsigned char* p = At(out, W, W / 2, H / 2);
                    ReportPixel("negative control centre", p);
                    const unsigned char expectMagenta[4] = {255, 0, 255, 255};
                    if (harness::PixelNear(p, expectMagenta, 4)) {
                        log.Pass("negative control: binding a different texture changed the output");
                    } else {
                        log.Fail("negative control: output did not follow the bound texture");
                    }
                }
                device->Destroy(set2);
                device->Destroy(view2);
            }
            device->Destroy(tex2);
        }
    }

    // ---- Teardown ----
    device->Destroy(descriptorSet);
    device->Destroy(sampler);
    device->Destroy(sampledView);
    device->Destroy(tex);
    device->Destroy(vb);
    device->Destroy(ib);
    device->Destroy(vs);
    device->Destroy(fs);
    device->Destroy(pipe);
    device->Destroy(view0);
    device->Destroy(view1);
    device->Destroy(target);
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
