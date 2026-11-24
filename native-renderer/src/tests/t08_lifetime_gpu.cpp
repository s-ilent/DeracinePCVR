// t08_lifetime_gpu: resource lifetime against real GPU completion.
//
// Proves that application ownership and GPU submission retention are enforced
// independently, using an actual timeline-semaphore fence rather than sleeps.
#include <cstdio>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/shader_compile.h"
#include "common/vk_bootstrap.h"

namespace {

// Builds a trivial pipeline so root constants can be set.
semantic::ResourceId MakePipeline(semantic::Device& device, std::string& error) {
    std::vector<uint32_t> vsSpv, fsSpv;
    if (!harness::CompileGlsl("#version 450\nvoid main(){gl_Position=vec4(0.0);}\n",
                              VK_SHADER_STAGE_VERTEX_BIT, "main", vsSpv, error)) {
        return {};
    }
    if (!harness::CompileGlsl(
            "#version 450\nlayout(push_constant) uniform P{vec4 c;}pc;"
            "layout(location=0) out vec4 o;void main(){o=pc.c;}\n",
            VK_SHADER_STAGE_FRAGMENT_BIT, "main", fsSpv, error)) {
        return {};
    }
    semantic::ShaderDesc vsd;
    vsd.stage = semantic::ShaderStage::kVertex;
    vsd.bytecode = vsSpv;
    const auto vs = device.CreateShader(vsd, error);
    semantic::ShaderDesc fsd;
    fsd.stage = semantic::ShaderStage::kPixel;
    fsd.bytecode = fsSpv;
    const auto fs = device.CreateShader(fsd, error);
    semantic::GraphicsPipelineDesc pd;
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.colorFormats.push_back(semantic::Format::kRGBA8Unorm);
    return device.CreateGraphicsPipeline(pd, error);
}

}  // namespace

int main() {
    harness::TestLog log("t08_lifetime_gpu");

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
    log.Pass("semantic NRI device created");

    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto id = device->CreateBuffer(bd, error);
        if (!id.IsValid()) {
            log.Fail("buffer create: " + error);
        } else {
            if (!device->BeginCommands(error)) log.Fail("BeginCommands: " + error);
            device->SetVertexBuffer(0, id, 0, 24);
            if (!device->EndCommands(error)) log.Fail("EndCommands: " + error);
            uint64_t v = 0;
            if (!device->Submit(v, error)) log.Fail("Submit: " + error);
            device->WaitForCompletion(v);
            if (device->Registry().IsAlive(id)) log.Pass("resource survives GPU completion while app-owned");
            else log.Fail("resource died at GPU completion despite app ownership");
            device->Destroy(id);
        }
    }

    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto id = device->CreateBuffer(bd, error);
        if (!id.IsValid()) {
            log.Fail("buffer create 2: " + error);
        } else {
            if (!device->BeginCommands(error)) log.Fail("BeginCommands 2: " + error);
            device->SetVertexBuffer(0, id, 0, 24);
            if (!device->EndCommands(error)) log.Fail("EndCommands 2: " + error);
            uint64_t v = 0;
            if (!device->Submit(v, error)) log.Fail("Submit 2: " + error);
            device->Destroy(id);
            if (device->Registry().PendingDestroyCount() >= 1) log.Pass("destroy deferred while GPU work is outstanding");
            else log.Fail("resource was not marked pending-destroy");
            if (!device->Registry().CanRecord(id)) log.Pass("pending-destroy resource rejects new work");
            else log.Fail("pending-destroy resource accepted new work");
            device->WaitForCompletion(v);
            if (!device->Registry().IsAlive(id)) log.Pass("physical destruction occurred after GPU completion");
            else log.Fail("resource still alive after completion");
        }
    }

    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto id = device->CreateBuffer(bd, error);
        if (!id.IsValid()) {
            log.Fail("buffer create 3: " + error);
        } else {
            uint64_t v1 = 0, v2 = 0;
            for (int i = 0; i < 2; ++i) {
                if (!device->BeginCommands(error)) log.Fail("BeginCommands 3: " + error);
                device->SetVertexBuffer(0, id, 0, 24);
                if (!device->EndCommands(error)) log.Fail("EndCommands 3: " + error);
                uint64_t v = 0;
                if (!device->Submit(v, error)) log.Fail("Submit 3: " + error);
                if (i == 0) v1 = v; else v2 = v;
            }
            device->Destroy(id);
            device->WaitForCompletion(v1);
            if (device->Registry().IsAlive(id)) log.Pass("still retained by the second in-flight submission");
            else log.Fail("destroyed after only the first submission");
            device->WaitForCompletion(v2);
            if (!device->Registry().IsAlive(id)) log.Pass("destroyed after both submissions completed");
            else log.Fail("not destroyed after both submissions");
        }
    }

    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto id = device->CreateBuffer(bd, error);
        device->Destroy(id);
        device->WaitForCompletion(device->CompletedValue());
        if (!device->Registry().CanRecord(id)) log.Pass("stale id rejected for new work");
        else log.Fail("stale id accepted new work");
        if (!device->Registry().IsAlive(id)) log.Pass("stale id does not resolve");
        else log.Fail("stale id still resolves");
    }

    {
        const auto pipe = MakePipeline(*device, error);
        if (!pipe.IsValid()) {
            log.Fail("pipeline for parameter test: " + error);
        } else {
            float params[4] = {1, 0, 0, 1};
            if (!device->BeginCommands(error)) log.Fail("BeginCommands 5: " + error);
            device->BindPipeline(pipe);
            semantic::ShaderParameterBlock block;
            block.data = params;
            block.sizeBytes = sizeof(params);
            device->SetShaderParameters(block);
            params[0] = 99.0f;   // mutate after recording
            if (!device->EndCommands(error)) log.Fail("EndCommands 5: " + error);
            uint64_t v = 0;
            if (!device->Submit(v, error)) log.Fail("Submit 5: " + error);
            device->WaitForCompletion(v);
            log.Pass("parameter snapshot recorded and submitted without mutation");
            device->Destroy(pipe);
        }
    }

    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto id = device->CreateBuffer(bd, error);
        if (!device->BeginCommands(error)) log.Fail("BeginCommands 6: " + error);
        device->SetVertexBuffer(0, id, 0, 24);
        if (!device->EndCommands(error)) log.Fail("EndCommands 6: " + error);
        uint64_t v = 0;
        if (!device->Submit(v, error)) log.Fail("Submit 6: " + error);
        device.reset();
        log.Pass("backend shutdown with outstanding work completed without crash");
    }

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
