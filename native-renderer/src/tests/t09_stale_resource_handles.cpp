// t09_stale_resource_handles: stale ids must never reach a freed NRI object.
//
// Every case destroys a resource, keeps its old id, and then attempts the
// operations that would use it. The backend must reject each one with a
// structured reason and must not call NRI with a freed pointer.
#include <cstdio>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

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
    harness::TestLog log("t09_stale_resource_handles");

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

    // ---- 1. Destroyed buffer rejects every buffer operation ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto buf = device->CreateBuffer(bd, error);
        if (!buf.IsValid()) {
            log.Fail("buffer create: " + error);
        } else {
            device->Destroy(buf);
            device->WaitForCompletion(device->CompletedValue());

            if (!device->Registry().IsAlive(buf)) log.Pass("destroyed buffer no longer resolves");
            else log.Fail("destroyed buffer still resolves");

            // UploadBuffer must reject.
            const float junk[4] = {1, 2, 3, 4};
            if (!device->UploadBuffer(buf, junk, sizeof(junk), 0, error)) {
                log.Pass("UploadBuffer rejected a destroyed buffer");
            } else {
                log.Fail("UploadBuffer accepted a destroyed buffer");
            }

            // Recording operations must reject and poison the recording.
            if (!device->BeginCommands(error)) log.Fail("BeginCommands: " + error);
            device->SetVertexBuffer(0, buf, 0, 24);
            if (nri->RecordingPoisoned()) log.Pass("SetVertexBuffer poisoned the recording");
            else log.Fail("SetVertexBuffer did not poison the recording");
            if (!device->LastRejection().empty()) {
                log.Pass("rejection reason recorded: " + device->LastRejection());
            } else {
                log.Fail("no rejection reason recorded");
            }
            if (!device->EndCommands(error)) {
                log.Pass("poisoned recording cannot be ended successfully");
            } else {
                log.Fail("poisoned recording ended successfully");
            }
        }
    }

    // ---- 2. Destroyed pipeline rejects binding ----
    {
        std::vector<uint32_t> vsSpv, fsSpv;
        if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error) ||
            !harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
            log.Fail("shader compile: " + error);
        } else {
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
            // The vertex shader declares location 0, so the pipeline must too.
            pd.vertexBindings.push_back({0, 8});
            pd.vertexAttributes.push_back({0, 0, semantic::Format::kR32G32Float, 0});
            pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
            const auto pipe = device->CreateGraphicsPipeline(pd, error);
            if (!pipe.IsValid()) {
                log.Fail("pipeline create: " + error);
            } else {
                device->Destroy(pipe);
                if (!device->BeginCommands(error)) log.Fail("BeginCommands: " + error);
                device->BindPipeline(pipe);
                if (nri->RecordingPoisoned()) log.Pass("BindPipeline rejected a destroyed pipeline");
                else log.Fail("BindPipeline accepted a destroyed pipeline");
                device->AbandonRecording();
                device->Destroy(vs);
                device->Destroy(fs);
            }
        }
    }

    // ---- 3. Destroyed texture rejects view creation ----
    {
        semantic::TextureDesc td;
        td.format = semantic::Format::kRGBA8Unorm;
        td.width = 16;
        td.height = 16;
        td.usage = semantic::TextureUsage::kColorAttachment;
        const auto tex = device->CreateTexture(td, error);
        if (!tex.IsValid()) {
            log.Fail("texture create: " + error);
        } else {
            device->Destroy(tex);
            semantic::TextureViewDesc vd;
            vd.texture = tex;
            vd.aspect = semantic::ImageAspect::kColor;
            const auto view = device->CreateTextureView(vd, error);
            if (!view.IsValid()) {
                log.Pass("CreateTextureView rejected a destroyed texture");
            } else {
                log.Fail("CreateTextureView accepted a destroyed texture");
            }
        }
    }

    // ---- 4. Destroyed shader rejects pipeline creation ----
    {
        std::vector<uint32_t> vsSpv, fsSpv;
        if (!harness::CompileGlsl(kVs, VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error) ||
            !harness::CompileGlsl(kFs, VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
            log.Fail("shader compile 4: " + error);
        } else {
            semantic::ShaderDesc vsd;
            vsd.stage = semantic::ShaderStage::kVertex;
            vsd.bytecode = vsSpv;
            const auto vs = device->CreateShader(vsd, error);
            semantic::ShaderDesc fsd;
            fsd.stage = semantic::ShaderStage::kPixel;
            fsd.bytecode = fsSpv;
            const auto fs = device->CreateShader(fsd, error);
            device->Destroy(vs);
            semantic::GraphicsPipelineDesc pd;
            pd.vertexShader = vs;
            pd.pixelShader = fs;
            pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
            const auto pipe = device->CreateGraphicsPipeline(pd, error);
            if (!pipe.IsValid()) {
                log.Pass("CreateGraphicsPipeline rejected a destroyed shader");
            } else {
                log.Fail("CreateGraphicsPipeline accepted a destroyed shader");
            }
            device->Destroy(fs);
        }
    }

    // ---- 5. Valid index with wrong generation ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto buf = device->CreateBuffer(bd, error);
        semantic::ResourceId stale = buf;
        stale.generation += 1;
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 5: " + error);
        device->SetVertexBuffer(0, stale, 0, 24);
        if (nri->RecordingPoisoned()) log.Pass("wrong generation rejected");
        else log.Fail("wrong generation accepted");
        if (device->LastRejection().find("stale-generation") != std::string::npos) {
            log.Pass("rejection names stale-generation");
        } else {
            log.Fail("rejection did not name stale-generation: " + device->LastRejection());
        }
        device->AbandonRecording();
        device->Destroy(buf);
    }

    // ---- 6. Wrong resource type ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto buf = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 6: " + error);
        // A buffer id used where a pipeline is expected.
        device->BindPipeline(buf);
        if (nri->RecordingPoisoned()) log.Pass("wrong resource type rejected");
        else log.Fail("wrong resource type accepted");
        if (device->LastRejection().find("wrong-type") != std::string::npos) {
            log.Pass("rejection names wrong-type");
        } else {
            log.Fail("rejection did not name wrong-type: " + device->LastRejection());
        }
        device->AbandonRecording();
        device->Destroy(buf);
    }

    // ---- 7. Destroy before recording is rejected before any NRI call ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto buf = device->CreateBuffer(bd, error);
        device->Destroy(buf);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 7: " + error);
        device->SetIndexBuffer(buf, 0, semantic::IndexFormat::kUint16);
        if (nri->RecordingPoisoned()) log.Pass("destroy-before-record rejected");
        else log.Fail("destroy-before-record accepted");
        device->AbandonRecording();
    }

    // ---- 8. A valid resource still works after all the rejections ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto buf = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 8: " + error);
        device->SetVertexBuffer(0, buf, 0, 24);
        if (!nri->RecordingPoisoned()) log.Pass("a valid resource records cleanly");
        else log.Fail("a valid resource was rejected: " + device->LastRejection());
        if (device->EndCommands(error)) log.Pass("clean recording ends successfully");
        else log.Fail("clean recording failed to end: " + error);
        uint64_t v = 0;
        if (device->Submit(v, error)) log.Pass("clean recording submits");
        else log.Fail("clean recording failed to submit: " + error);
        device->WaitForCompletion(v);
        device->Destroy(buf);
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
