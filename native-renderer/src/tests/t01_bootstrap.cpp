// t01_bootstrap: Vulkan instance/device creation, explicit feature negotiation,
// NRI adoption of the same device, and native handle identity verification.
#include <cstdio>
#include <string>

#include "common/diagnostics.h"
#include "common/nri_adoption.h"
#include "common/vk_bootstrap.h"

int main() {
    harness::TestLog log("t01_bootstrap");

    harness::BootstrapConfig cfg;
    cfg.enableValidation = true;
    cfg.enableSyncValidation = true;
    cfg.preferDiscreteGpu = true;
    cfg.allowSoftwareDevice = false;

    harness::VulkanContext vk;
    std::string error;
    if (!vk.Initialize(cfg, error)) {
        log.Fail("Vulkan initialize: " + error);
        return log.Finish();
    }
    log.Pass("Vulkan instance and device created");

    const auto& m = vk.manifest();
    std::printf("%s", m.Describe().c_str());

    if (m.isSoftware) {
        log.Info("running on a SOFTWARE device; results are not physical-GPU evidence");
    } else {
        log.Pass("physical GPU device selected: " + m.deviceName);
    }

    if (vk.validationActive()) {
        log.Pass("validation layer active");
    } else {
        log.Info("validation layer NOT active (not installed or disabled)");
    }

    // Feature negotiation must be explicit: we asked for these, so they must
    // either be enabled or reported as unavailable.
    if (m.timelineSemaphore) {
        log.Pass("timelineSemaphore enabled");
    } else {
        log.Unsupported("timelineSemaphore unavailable");
    }
    if (m.synchronization2) {
        log.Pass("synchronization2 enabled");
    } else {
        log.Unsupported("synchronization2 unavailable");
    }
    if (m.dynamicRendering) {
        log.Pass("dynamicRendering enabled");
    } else {
        log.Unsupported("dynamicRendering unavailable");
    }
    if (m.multiview) {
        log.Pass("multiview enabled, maxViewCount=" + std::to_string(m.maxViewCount));
    } else {
        log.Unsupported("multiview unavailable");
    }
    if (m.separateDepthStencilLayouts) {
        log.Pass("separateDepthStencilLayouts enabled");
    } else {
        log.Unsupported("separateDepthStencilLayouts unavailable");
    }

    // NRI adoption of the existing device.
    harness::NriContext nri;
    if (!nri.Initialize(vk, true, error)) {
        log.Fail("NRI adoption: " + error);
        return log.Finish();
    }
    log.Pass("NRI adopted the existing Vulkan device");

    if (!nri.VerifyHandleIdentity(vk, error)) {
        log.Fail("handle identity: " + error);
    } else {
        log.Pass("NRI reports the same VkDevice, VkPhysicalDevice and VkInstance");
    }

    // A minimal NRI operation proves the adopted device is usable.
    nri::Queue* queue = nullptr;
    if (nri.core().GetQueue(*nri.device(), nri::QueueType::GRAPHICS, 0, queue) !=
            nri::Result::SUCCESS ||
        queue == nullptr) {
        log.Fail("NRI GetQueue failed");
    } else {
        log.Pass("NRI graphics queue acquired");
        void* nativeQueue = nri.core().GetQueueNativeObject(queue);
        if (nativeQueue == reinterpret_cast<void*>(vk.graphicsQueue())) {
            log.Pass("NRI queue handle matches the adopted VkQueue");
        } else {
            log.Fail("NRI queue handle differs from the adopted VkQueue");
        }
    }

    // Teardown order: NRI device wrapper first, then the native device.
    nri.Shutdown();
    log.Pass("NRI device wrapper destroyed");

    vk.Shutdown();
    log.Pass("native Vulkan device destroyed");

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
