// t13_p0_lifetime_corrections: regression for the three P0 defects.
//
// 1. Dependency edges must survive until the dependent is physically destroyed.
// 2. An executable command context must not be overwritten by BeginCommands.
// 3. A pending-destroy texture must reject new views.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/resource_registry.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

// Records the order in which physical destructors run.
struct DestructionLog {
    std::vector<std::string> order;

    std::function<void(void*)> MakeFn(const char* tag) {
        return [this, tag](void*) { order.push_back(tag); };
    }
    int Count(const char* tag) const {
        int n = 0;
        for (const auto& s : order) if (s == tag) ++n;
        return n;
    }
};

const char* kVs = R"(
#version 450
layout(location = 0) in vec2 inPos;
void main() { gl_Position = vec4(inPos, 0.0, 1.0); }
)";

const char* kFs = R"(
#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0); }
)";

}  // namespace

int main() {
    harness::TestLog log("t13_p0_lifetime_corrections");

    // =======================================================================
    // Part A: CPU registry — dependency survives until physical destruction
    // =======================================================================
    {
        harness::ResourceRegistry reg;
        DestructionLog dl;
        const auto tex = reg.Create(harness::ResourceType::kTexture,
                                    reinterpret_cast<void*>(0x1000), dl.MakeFn("T"));
        const auto view = reg.Create(harness::ResourceType::kTextureView,
                                     reinterpret_cast<void*>(0x1001), dl.MakeFn("V"));
        reg.AddDependency(view, tex);

        // The view is retained by a GPU submission.
        reg.RetainForGpu(view, 0, 1);

        reg.Destroy(tex);
        reg.Destroy(view);

        if (dl.order.empty()) {
            log.Pass("A1: neither destructor ran while the view is GPU-retained");
        } else {
            log.Fail("A1: destructor ran early: " + dl.order.front());
        }

        // The parent must still be alive because the view is still alive.
        if (reg.IsAlive(tex)) {
            log.Pass("A2: parent texture still alive while the view is alive");
        } else {
            log.Fail("A2: parent texture retired while a live view depended on it");
        }

        reg.CompleteDomain(0, 1);

        if (dl.order.size() == 2) {
            log.Pass("A3: both destructors ran after completion");
        } else {
            log.Fail("A3: expected 2 destructors, saw " + std::to_string(dl.order.size()));
        }
        if (dl.order.size() == 2 && dl.order[0] == "V" && dl.order[1] == "T") {
            log.Pass("A4: view destroyed before its parent texture");
        } else {
            log.Fail("A4: wrong destruction order");
        }
        if (dl.Count("V") == 1 && dl.Count("T") == 1) {
            log.Pass("A5: each destructor ran exactly once");
        } else {
            log.Fail("A5: duplicate destructor invocation");
        }
        if (!reg.IsAlive(tex) && !reg.IsAlive(view)) {
            log.Pass("A6: both ids invalid after destruction");
        } else {
            log.Fail("A6: an id still resolves");
        }
    }

    // ---- A7: DestroyAll respects dependency ordering ----
    {
        harness::ResourceRegistry reg;
        DestructionLog dl;
        const auto tex = reg.Create(harness::ResourceType::kTexture,
                                    reinterpret_cast<void*>(0x2000), dl.MakeFn("T"));
        const auto view = reg.Create(harness::ResourceType::kTextureView,
                                     reinterpret_cast<void*>(0x2001), dl.MakeFn("V"));
        reg.AddDependency(view, tex);
        reg.RetainForGpu(view, 0, 5);   // never completed

        const int n = reg.DestroyAll();
        if (n == 2) log.Pass("A7: DestroyAll drained both resources");
        else log.Fail("A7: DestroyAll drained " + std::to_string(n) + " of 2");
        if (dl.order.size() == 2 && dl.order[0] == "V" && dl.order[1] == "T") {
            log.Pass("A8: DestroyAll destroyed the view before the texture");
        } else {
            log.Fail("A8: DestroyAll used the wrong order");
        }
    }

    // ---- A9: AddDependency rejects a pending-destroy parent ----
    {
        harness::ResourceRegistry reg;
        DestructionLog dl;
        const auto tex = reg.Create(harness::ResourceType::kTexture,
                                    reinterpret_cast<void*>(0x3000), dl.MakeFn("T"));
        const auto view = reg.Create(harness::ResourceType::kTextureView,
                                     reinterpret_cast<void*>(0x3001), dl.MakeFn("V"));
        reg.Destroy(tex);
        if (!reg.AddDependency(view, tex)) {
            log.Pass("A9: AddDependency rejected a pending-destroy parent");
        } else {
            log.Fail("A9: AddDependency accepted a pending-destroy parent");
        }
    }

    // =======================================================================
    // Part B: real backend
    // =======================================================================
    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan init: " + error);
        return log.Finish();
    }

    auto device = semantic::CreateNriDevice(vk, error);
    if (!device) {
        log.Fail("semantic device: " + error);
        return log.Finish();
    }
    auto* nri = static_cast<harness::NriDevice*>(device.get());
    log.Pass("B0: semantic NRI device created");

    // ---- B1: pending-destroy texture rejects a new view ----
    {
        semantic::TextureDesc td;
        td.format = semantic::Format::kRGBA8Unorm;
        td.width = 16;
        td.height = 16;
        td.usage = semantic::TextureUsage::kColorAttachment;
        const auto tex = device->CreateTexture(td, error);
        if (!tex.IsValid()) {
            log.Fail("B1: texture create: " + error);
        } else {
            device->Destroy(tex);
            semantic::TextureViewDesc vd;
            vd.texture = tex;
            vd.aspect = semantic::ImageAspect::kColor;
            const auto view = device->CreateTextureView(vd, error);
            if (!view.IsValid()) {
                log.Pass("B1: pending-destroy texture rejected a new view");
            } else {
                log.Fail("B1: pending-destroy texture accepted a new view");
            }
        }
    }

    // ---- B2: executable context cannot be overwritten by BeginCommands ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto x = device->CreateBuffer(bd, error);
        if (!x.IsValid()) {
            log.Fail("B2: buffer create: " + error);
        } else {
            if (!device->BeginCommands(error)) log.Fail("B2: BeginCommands A: " + error);
            device->SetVertexBuffer(0, x, 0, 24);
            if (!device->EndCommands(error)) log.Fail("B2: EndCommands A: " + error);

            // A is now executable and unsubmitted. A second Begin must fail.
            std::string secondError;
            const bool second = device->BeginCommands(secondError);
            if (!second) {
                log.Pass("B2: second BeginCommands rejected while A is executable");
            } else {
                log.Fail("B2: second BeginCommands overwrote an executable context");
            }
            if (!secondError.empty()) {
                log.Pass("B2: rejection carried a reason: " + secondError);
            } else {
                log.Fail("B2: rejection had no reason");
            }

            // A must still be submittable and must still retain X.
            uint64_t v = 0;
            if (device->Submit(v, error)) {
                log.Pass("B3: executable A still submits after the rejected Begin");
            } else {
                log.Fail("B3: executable A could not submit: " + error);
            }
            device->Destroy(x);
            if (device->Registry().IsAlive(x)) {
                log.Pass("B4: submitted work retains X");
            } else {
                log.Fail("B4: X was not retained by the submission");
            }
            device->WaitForCompletion(v);
            if (!device->Registry().IsAlive(x)) {
                log.Pass("B5: X retired after completion");
            } else {
                log.Fail("B5: X survived completion");
            }
        }
    }

    // ---- B6: executable A -> AbandonRecording -> Begin B ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto x = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("B6: BeginCommands A: " + error);
        device->SetVertexBuffer(0, x, 0, 24);
        if (!device->EndCommands(error)) log.Fail("B6: EndCommands A: " + error);
        device->AbandonRecording();
        if (device->BeginCommands(error)) {
            log.Pass("B6: Begin succeeded after abandoning an executable context");
        } else {
            log.Fail("B6: Begin failed after abandoning an executable context: " + error);
        }
        device->AbandonRecording();
        device->Destroy(x);
    }

    // ---- B7: executable A -> Destroy(X) -> Abandon -> X retires ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto x = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("B7: BeginCommands: " + error);
        device->SetVertexBuffer(0, x, 0, 24);
        if (!device->EndCommands(error)) log.Fail("B7: EndCommands: " + error);
        device->Destroy(x);
        if (device->Registry().IsAlive(x)) {
            log.Pass("B7: recording reference keeps X alive while executable");
        } else {
            log.Fail("B7: X died while an executable context referenced it");
        }
        device->AbandonRecording();
        if (!device->Registry().IsAlive(x)) {
            log.Pass("B8: X retired after the executable context was abandoned");
        } else {
            log.Fail("B8: X leaked after abandon");
        }
    }

    // ---- B9: recording tokens are unique and independent of submit values ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto x = device->CreateBuffer(bd, error);
        std::vector<uint64_t> tokens;
        for (int i = 0; i < 3; ++i) {
            if (!device->BeginCommands(error)) { log.Fail("B9: Begin: " + error); break; }
            tokens.push_back(nri->CurrentRecordingToken());
            device->SetVertexBuffer(0, x, 0, 24);
            if (!device->EndCommands(error)) { log.Fail("B9: End: " + error); break; }
            device->AbandonRecording();
        }
        bool unique = tokens.size() == 3;
        for (size_t i = 0; i < tokens.size() && unique; ++i) {
            for (size_t j = i + 1; j < tokens.size(); ++j) {
                if (tokens[i] == tokens[j]) unique = false;
            }
        }
        if (unique) log.Pass("B9: recording tokens are unique across abandoned recordings");
        else log.Fail("B9: duplicate recording tokens");
        device->Destroy(x);
    }

    // ---- B10: texture + view submitted, both destroyed before completion ----
    {
        const uint32_t W = 32, H = 32;
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = W;
        d.height = H;
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("B10: external image: " + error);
        } else {
            VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
            const float bg[4] = {0, 0, 0, 1};
            harness::ClearColor(vk, pool, img, bg, 0);

            semantic::ExternalImageDesc ed;
            ed.nativeHandle = reinterpret_cast<void*>(img.image);
            ed.format = semantic::Format::kRGBA8Unorm;
            ed.width = W;
            ed.height = H;
            ed.usage = semantic::TextureUsage::kColorAttachment |
                       semantic::TextureUsage::kTransferSrc;
            const auto tex = device->ImportExternalImage(ed, error);
            semantic::TextureViewDesc vd;
            vd.texture = tex;
            vd.aspect = semantic::ImageAspect::kColor;
            const auto view = device->CreateTextureView(vd, error);

            if (!tex.IsValid() || !view.IsValid()) {
                log.Fail("B10: import/view: " + error);
            } else {
                std::vector<uint32_t> vsSpv, fsSpv;
                harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error);
                harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error);
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
                pd.vertexBindings.push_back({0, 8});
                pd.vertexAttributes.push_back({0, 0, semantic::Format::kR32G32Float, 0});
                pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
                const auto pipe = device->CreateGraphicsPipeline(pd, error);

                if (!device->BeginCommands(error)) log.Fail("B10: Begin: " + error);
                device->TransitionResource(tex, semantic::ImageAspect::kColor,
                                           semantic::ResourceState::kUndefined,
                                           semantic::ResourceState::kColorAttachment);
                semantic::RenderPassDesc rp;
                semantic::ColorAttachment ca;
                ca.view = view;
                ca.loadOp = semantic::LoadOp::kLoad;
                rp.colorAttachments.push_back(ca);
                rp.width = W;
                rp.height = H;
                device->BeginRenderPass(rp);
                device->BindPipeline(pipe);
                semantic::Viewport vp;
                vp.width = static_cast<float>(W);
                vp.height = static_cast<float>(H);
                device->SetViewport(vp);
                semantic::Scissor sc;
                sc.width = W;
                sc.height = H;
                device->SetScissor(sc);
                device->EndRenderPass();
                if (!device->EndCommands(error)) log.Fail("B10: End: " + error);

                uint64_t v = 0;
                if (!device->Submit(v, error)) log.Fail("B10: Submit: " + error);

                // Destroy both before the GPU has necessarily finished.
                device->Destroy(tex);
                device->Destroy(view);

                if (device->Registry().IsAlive(view)) {
                    log.Pass("B10: view stays alive while submitted work references it");
                } else {
                    log.Fail("B10: view died while submitted work referenced it");
                }
                if (device->Registry().IsAlive(tex)) {
                    log.Pass("B11: parent texture stays alive while the view is alive");
                } else {
                    log.Fail("B11: parent texture retired while a live view depended on it");
                }

                device->WaitForCompletion(v);

                if (!device->Registry().IsAlive(view) && !device->Registry().IsAlive(tex)) {
                    log.Pass("B12: both retired after completion");
                } else {
                    log.Fail("B12: a resource leaked after completion");
                }
                if (device->Registry().TotalRefs() == 0) {
                    log.Pass("B13: no reference leaks remain");
                } else {
                    log.Fail("B13: " + std::to_string(device->Registry().TotalRefs()) +
                             " references leaked");
                }

                device->Destroy(vs);
                device->Destroy(fs);
                device->Destroy(pipe);
            }
            harness::DestroyImage(vk, img);
            vkDestroyCommandPool(vk.device(), pool, nullptr);
        }
    }

    device.reset();
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
