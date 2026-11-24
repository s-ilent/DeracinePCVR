// Vulkan bootstrap: instance, physical device, logical device with explicit
// feature negotiation, and a manifest of what was actually enabled.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace harness {

struct QueueRequest {
    VkQueueFlags flags = VK_QUEUE_GRAPHICS_BIT;
    uint32_t count = 1;
};

// Everything the harness actually enabled, recorded so tests can assert on it
// rather than assuming a feature is present.
struct DeviceManifest {
    std::string deviceName;
    uint32_t apiVersion = 0;
    uint32_t driverVersion = 0;
    VkPhysicalDeviceType deviceType = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool isSoftware = false;

    // Enabled extensions
    std::vector<std::string> instanceExtensions;
    std::vector<std::string> deviceExtensions;

    // Enabled features (only the ones the harness asks for)
    bool timelineSemaphore = false;
    bool synchronization2 = false;
    bool dynamicRendering = false;
    bool multiview = false;
    bool multiviewGeometryShader = false;
    bool separateDepthStencilLayouts = false;
    bool samplerAnisotropy = false;
    bool depthClamp = false;
    bool independentBlend = false;
    bool fillModeNonSolid = false;
    bool wideLines = false;
    bool imageCubeArray = false;
    bool shaderStorageImageWriteWithoutFormat = false;
    bool shaderStorageImageReadWithoutFormat = false;
    bool fragmentStoresAndAtomics = false;
    bool vertexPipelineStoresAndAtomics = false;
    bool shaderFloat64 = false;
    bool shaderInt64 = false;
    bool shaderInt16 = false;
    bool maintenance4 = false;
    bool maintenance5 = false;
    bool maintenance8 = false;
    bool hostQueryReset = false;
    bool bufferDeviceAddress = false;
    bool descriptorIndexing = false;
    bool sampleRateShading = false;
    bool depthBiasClamp = false;
    bool logicOp = false;
    bool occlusionQueryPrecise = false;
    bool pipelineStatisticsQuery = false;
    bool geometryShader = false;
    bool tessellationShader = false;
    bool dualSrcBlend = false;
    bool multiDrawIndirect = false;
    bool drawIndirectFirstInstance = false;
    bool alphaToOne = false;
    bool sparseBinding = false;
    bool textureCompressionBC = false;
    bool textureCompressionASTC_LDR = false;
    bool textureCompressionETC2 = false;

    // Limits that matter for the tests
    uint32_t maxViewCount = 0;
    uint32_t maxViewportDimensions[2] = {0, 0};
    uint32_t maxColorAttachments = 0;
    uint32_t maxFramebufferWidth = 0;
    uint32_t maxFramebufferHeight = 0;
    VkSampleCountFlags framebufferColorSampleCounts = 0;
    VkSampleCountFlags framebufferDepthSampleCounts = 0;
    VkSampleCountFlags framebufferStencilSampleCounts = 0;

    // Queues
    struct QueueInfo {
        VkQueueFlags flags = 0;
        uint32_t familyIndex = 0;
        uint32_t queueIndex = 0;
        uint32_t count = 0;
    };
    std::vector<QueueInfo> queues;

    std::string Describe() const;
};

struct BootstrapConfig {
    bool enableValidation = true;
    bool enableSyncValidation = true;
    bool preferDiscreteGpu = true;
    bool allowSoftwareDevice = false;
    // Extra device extensions the caller needs (e.g. OpenXR's).
    std::vector<const char*> extraDeviceExtensions;
    std::vector<const char*> extraInstanceExtensions;
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Creates instance, picks a physical device, creates the logical device.
    // Returns false and fills 'error' on failure.
    bool Initialize(const BootstrapConfig& config, std::string& error);

    void Shutdown();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    uint32_t graphicsFamily() const { return graphicsFamily_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    const DeviceManifest& manifest() const { return manifest_; }

    // True when the validation layer is active (not merely requested).
    bool validationActive() const { return validationActive_; }

    // Convenience: find a memory type index satisfying both bits.
    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    // One-shot command buffer helpers for tests.
    VkCommandPool CreateCommandPool(uint32_t familyIndex) const;
    VkCommandBuffer BeginOneShot(VkCommandPool pool) const;
    void EndOneShot(VkCommandPool pool, VkCommandBuffer cmd) const;

private:
    bool CreateInstance(const BootstrapConfig& config, std::string& error);
    bool PickPhysicalDevice(const BootstrapConfig& config, std::string& error);
    bool CreateLogicalDevice(const BootstrapConfig& config, std::string& error);
    void FillManifest();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    DeviceManifest manifest_;
    std::vector<std::string> instanceExtensions_;
    bool validationActive_ = false;
    bool syncValidationActive_ = false;
};

// Validation message counters, so tests can assert "zero errors".
struct ValidationCounters {
    uint32_t errors = 0;
    uint32_t warnings = 0;
    uint32_t syncErrors = 0;
    std::vector<std::string> messages;
};

ValidationCounters& GlobalValidationCounters();

}  // namespace harness
