// t10_recording_lifetime: a recording owns its resources until it is submitted.
//
// The invariant under test: every backend resource used by a recorded command
// must remain physically alive until that command can no longer execute.
#include <cstdio>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/vk_bootstrap.h"

int main() {
    harness::TestLog log("t10_recording_lifetime");

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
    log.Pass("semantic NRI device created");

    // ---- 1. Record A -> Destroy A -> Submit: the work still finishes ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!a.IsValid()) {
            log.Fail("buffer create: " + error);
        } else {
            if (!device->BeginCommands(error)) log.Fail("BeginCommands: " + error);
            device->SetVertexBuffer(0, a, 0, 24);
            if (nri->RecordingPoisoned()) log.Fail("valid record was rejected");

            // Destroy while the command buffer still references it.
            device->Destroy(a);
            if (device->Registry().IsAlive(a)) {
                log.Pass("recording reference kept the resource alive after Destroy");
            } else {
                log.Fail("resource was destroyed while a recording referenced it");
            }

            if (!device->EndCommands(error)) log.Fail("EndCommands: " + error);
            uint64_t v = 0;
            if (device->Submit(v, error)) {
                log.Pass("submission succeeded after destroy-before-submit");
            } else {
                log.Fail("submission failed after destroy-before-submit: " + error);
            }
            device->WaitForCompletion(v);
            if (!device->Registry().IsAlive(a)) {
                log.Pass("resource retired after the submission completed");
            } else {
                log.Fail("resource still alive after completion");
            }
        }
    }

    // ---- 2. Record A -> Destroy A -> record another use of A: rejected ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 2: " + error);
        device->SetVertexBuffer(0, a, 0, 24);
        device->Destroy(a);
        // A second use in the same recording must be refused.
        device->SetIndexBuffer(a, 0, semantic::IndexFormat::kUint16);
        if (nri->RecordingPoisoned()) log.Pass("new use of a pending-destroy resource rejected");
        else log.Fail("new use of a pending-destroy resource accepted");
        device->AbandonRecording();
        device->WaitForCompletion(device->CompletedValue());
    }

    // ---- 3. Record A -> abandon: retention released ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 3: " + error);
        device->SetVertexBuffer(0, a, 0, 24);
        device->Destroy(a);
        if (device->Registry().IsAlive(a)) log.Pass("retained during recording");
        else log.Fail("not retained during recording");
        device->AbandonRecording();
        if (!device->Registry().IsAlive(a)) log.Pass("abandon released the recording reference");
        else log.Fail("abandon leaked the recording reference");
    }

    // ---- 4. Record A twice in one command buffer ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 4: " + error);
        device->SetVertexBuffer(0, a, 0, 24);
        device->SetVertexBuffer(0, a, 0, 24);
        if (!nri->RecordingPoisoned()) log.Pass("recording the same resource twice is allowed");
        else log.Fail("recording the same resource twice was rejected");
        if (!device->EndCommands(error)) log.Fail("EndCommands 4: " + error);
        uint64_t v = 0;
        if (!device->Submit(v, error)) log.Fail("Submit 4: " + error);
        device->Destroy(a);
        device->WaitForCompletion(v);
        if (!device->Registry().IsAlive(a)) log.Pass("no duplicate-release bug on completion");
        else log.Fail("resource survived after completion");
    }

    // ---- 5. Record A -> Submit -> Destroy A -> GPU completes ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 5: " + error);
        device->SetVertexBuffer(0, a, 0, 24);
        if (!device->EndCommands(error)) log.Fail("EndCommands 5: " + error);
        uint64_t v = 0;
        if (!device->Submit(v, error)) log.Fail("Submit 5: " + error);
        device->Destroy(a);
        if (device->Registry().IsAlive(a)) log.Pass("GPU reference keeps it alive after Destroy");
        else log.Fail("destroyed while a GPU submission referenced it");
        device->WaitForCompletion(v);
        if (!device->Registry().IsAlive(a)) log.Pass("retired after GPU completion");
        else log.Fail("not retired after completion");
    }

    // ---- 6. Destroy before recording: rejected before any NRI call ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        device->Destroy(a);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 6: " + error);
        device->SetVertexBuffer(0, a, 0, 24);
        if (nri->RecordingPoisoned()) log.Pass("destroy-before-record rejected");
        else log.Fail("destroy-before-record accepted");
        device->AbandonRecording();
    }

    // ---- 7. Texture view keeps its parent alive ----
    {
        semantic::TextureDesc td;
        td.format = semantic::Format::kRGBA8Unorm;
        td.width = 16;
        td.height = 16;
        td.usage = semantic::TextureUsage::kColorAttachment;
        const auto tex = device->CreateTexture(td, error);
        semantic::TextureViewDesc vd;
        vd.texture = tex;
        vd.aspect = semantic::ImageAspect::kColor;
        const auto view = device->CreateTextureView(vd, error);
        if (!view.IsValid()) {
            log.Fail("view create: " + error);
        } else {
            device->Destroy(tex);
            if (device->Registry().IsAlive(tex)) {
                log.Pass("parent texture survives while a view depends on it");
            } else {
                log.Fail("parent texture destroyed while a view existed");
            }
            device->Destroy(view);
            if (!device->Registry().IsAlive(tex)) {
                log.Pass("parent texture retires after the last view is destroyed");
            } else {
                log.Fail("parent texture did not retire");
            }
        }
    }

    // ---- 8. Multiple views: destroying one leaves the other valid ----
    {
        semantic::TextureDesc td;
        td.format = semantic::Format::kRGBA8Unorm;
        td.width = 16;
        td.height = 16;
        td.layerNum = 2;
        td.usage = semantic::TextureUsage::kColorAttachment;
        const auto tex = device->CreateTexture(td, error);
        semantic::TextureViewDesc vd;
        vd.texture = tex;
        vd.aspect = semantic::ImageAspect::kColor;
        vd.baseLayer = 0;
        const auto v1 = device->CreateTextureView(vd, error);
        vd.baseLayer = 1;
        const auto v2 = device->CreateTextureView(vd, error);
        if (!v1.IsValid() || !v2.IsValid()) {
            log.Fail("view create 8: " + error);
        } else {
            device->Destroy(tex);
            device->Destroy(v1);
            if (device->Registry().IsAlive(tex)) {
                log.Pass("texture survives while the second view exists");
            } else {
                log.Fail("texture retired while a view still existed");
            }
            if (device->Registry().IsAlive(v2)) log.Pass("the second view remains valid");
            else log.Fail("the second view was invalidated");
            device->Destroy(v2);
            if (!device->Registry().IsAlive(tex)) log.Pass("texture retires after the last view");
            else log.Fail("texture did not retire");
        }
    }

    // ---- 9. External image wrapper destroyed, VkImage survives ----
    {
        harness::ImageDesc d;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.width = 16;
        d.height = 16;
        d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        harness::Image img;
        if (!harness::CreateImage(vk, d, img, error)) {
            log.Fail("external image: " + error);
        } else {
            // ReadbackColor expects the image in COLOR_ATTACHMENT_OPTIMAL, so
            // put it there before importing. A fresh image is UNDEFINED.
            VkCommandPool clearPool = vk.CreateCommandPool(vk.graphicsFamily());
            const float bg[4] = {0.25f, 0.5f, 0.75f, 1.0f};
            harness::ClearColor(vk, clearPool, img, bg, 0);
            vkDestroyCommandPool(vk.device(), clearPool, nullptr);

            semantic::ExternalImageDesc ed;
            ed.nativeHandle = reinterpret_cast<void*>(img.image);
            ed.format = semantic::Format::kRGBA8Unorm;
            ed.width = 16;
            ed.height = 16;
            ed.usage = semantic::TextureUsage::kColorAttachment |
                       semantic::TextureUsage::kTransferSrc;
            const auto ext = device->ImportExternalImage(ed, error);
            if (!ext.IsValid()) {
                log.Fail("import: " + error);
            } else {
                device->Destroy(ext);
                device->WaitForCompletion(device->CompletedValue());
                // The wrapper is gone; the caller's image must still be usable.
                VkCommandPool pool = vk.CreateCommandPool(vk.graphicsFamily());
                std::vector<unsigned char> px;
                if (harness::ReadbackColor(vk, pool, img, 0, px, error)) {
                    log.Pass("external VkImage survives destruction of its NRI wrapper");
                } else {
                    log.Fail("external VkImage was invalidated: " + error);
                }
                vkDestroyCommandPool(vk.device(), pool, nullptr);
            }
            harness::DestroyImage(vk, img);
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
