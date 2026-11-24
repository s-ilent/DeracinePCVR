// t11_multi_submission: several submissions outstanding at once.
//
// The claim under test is backend bookkeeping, not simultaneous GPU execution.
// Both submissions are accepted before any wait, and the resource they share
// stays alive until every referencing submission completes.
#include <cstdio>
#include <string>
#include <vector>

#include "backend/nri_device.h"
#include "common/diagnostics.h"
#include "common/image_io.h"
#include "common/vk_bootstrap.h"

int main() {
    harness::TestLog log("t11_multi_submission");

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

    // ---- 1. Two submissions accepted without an intervening wait ----
    {
        semantic::BufferDesc bd;
        bd.size = 256;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        if (!a.IsValid()) {
            log.Fail("buffer create: " + error);
        } else {
            uint64_t v1 = 0, v2 = 0;
            if (!device->BeginCommands(error)) log.Fail("BeginCommands 1: " + error);
            device->SetVertexBuffer(0, a, 0, 24);
            if (!device->EndCommands(error)) log.Fail("EndCommands 1: " + error);
            if (!device->Submit(v1, error)) log.Fail("Submit 1: " + error);

            // No wait here. The second recording must be accepted.
            if (!device->BeginCommands(error)) log.Fail("BeginCommands 2: " + error);
            device->SetVertexBuffer(0, a, 0, 24);
            if (!device->EndCommands(error)) log.Fail("EndCommands 2: " + error);
            if (!device->Submit(v2, error)) log.Fail("Submit 2: " + error);

            if (v2 > v1) log.Pass("second submission accepted without waiting for the first");
            else log.Fail("submission values out of order");

            if (nri->OutstandingSubmissionCount() == 2) {
                log.Pass("both submissions outstanding in backend bookkeeping");
            } else {
                log.Fail("outstanding = " + std::to_string(nri->OutstandingSubmissionCount()));
            }

            // Revoke application ownership while both are in flight.
            device->Destroy(a);
            if (device->Registry().IsAlive(a)) {
                log.Pass("shared resource retained by two in-flight submissions");
            } else {
                log.Fail("shared resource destroyed while submissions referenced it");
            }

            device->WaitForCompletion(v1);
            const uint64_t afterFirst = device->CompletedValue();
            if (afterFirst < v2) {
                if (device->Registry().IsAlive(a)) {
                    log.Pass("still retained by the second submission");
                } else {
                    log.Fail("retired after only the first submission");
                }
            } else {
                log.Pass("GPU had already completed both submissions");
            }

            device->WaitForCompletion(v2);
            if (!device->Registry().IsAlive(a)) log.Pass("retired after every submission completed");
            else log.Fail("not retired after all submissions");
        }
    }

    // ---- 2. Contexts are reused only after completion ----
    {
        const int availableBefore = nri->AvailableContextCount();
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);

        std::vector<uint64_t> values;
        for (int i = 0; i < 3; ++i) {
            if (!device->BeginCommands(error)) log.Fail("BeginCommands 2." + std::to_string(i) + ": " + error);
            device->SetVertexBuffer(0, a, 0, 24);
            if (!device->EndCommands(error)) log.Fail("EndCommands 2." + std::to_string(i) + ": " + error);
            uint64_t v = 0;
            if (!device->Submit(v, error)) log.Fail("Submit 2." + std::to_string(i) + ": " + error);
            values.push_back(v);
        }
        if (nri->OutstandingSubmissionCount() == 3) {
            log.Pass("three submissions outstanding simultaneously");
        } else {
            log.Fail("outstanding = " + std::to_string(nri->OutstandingSubmissionCount()));
        }
        device->WaitForCompletion(values.back());
        if (nri->OutstandingSubmissionCount() == 0) {
            log.Pass("all contexts retired after completion");
        } else {
            log.Fail("outstanding after completion = " +
                     std::to_string(nri->OutstandingSubmissionCount()));
        }
        if (nri->AvailableContextCount() == availableBefore) {
            log.Pass("context pool returned to its initial size");
        } else {
            log.Fail("context pool leaked a context");
        }
        device->Destroy(a);
    }

    // ---- 3. Pool exhaustion applies backpressure instead of failing ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);

        // Submit more than the pool holds. The backend must wait rather than
        // fail or reset an in-flight allocator.
        int submitted = 0;
        bool ok = true;
        for (int i = 0; i < 8; ++i) {
            if (!device->BeginCommands(error)) { ok = false; break; }
            device->SetVertexBuffer(0, a, 0, 24);
            if (!device->EndCommands(error)) { ok = false; break; }
            uint64_t v = 0;
            if (!device->Submit(v, error)) { ok = false; break; }
            ++submitted;
        }
        if (ok && submitted == 8) {
            log.Pass("pool exhaustion applied backpressure, all 8 submissions accepted");
        } else {
            log.Fail("only " + std::to_string(submitted) + " of 8 submissions accepted: " + error);
        }
        device->WaitForCompletion(device->CompletedValue());
        device->Destroy(a);
    }

    // ---- 4. Shutdown with several outstanding submissions ----
    {
        semantic::BufferDesc bd;
        bd.size = 128;
        bd.usage = semantic::BufferUsage::kVertex;
        const auto a = device->CreateBuffer(bd, error);
        for (int i = 0; i < 3; ++i) {
            if (!device->BeginCommands(error)) break;
            device->SetVertexBuffer(0, a, 0, 24);
            if (!device->EndCommands(error)) break;
            uint64_t v = 0;
            if (!device->Submit(v, error)) break;
        }
        device.reset();
        log.Pass("shutdown drained outstanding submissions without crash");
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
