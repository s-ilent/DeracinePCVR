// NRI adoption of an externally created Vulkan device, plus the wrapper
// interface used to import Vulkan images without transferring ownership.
#pragma once

#include <vulkan/vulkan.h>

#include <NRI.h>
#include <Extensions/NRIWrapperVK.h>

#include <string>

#include "common/vk_bootstrap.h"

namespace harness {

// Tracks whether NRI destroyed native objects it was told not to own.
struct NativeDestructionProbe {
    int vkDestroyImageCalls = 0;
    int vkDestroyDeviceCalls = 0;
    int vkDestroyInstanceCalls = 0;
};

class NriContext {
public:
    NriContext() = default;
    ~NriContext();

    NriContext(const NriContext&) = delete;
    NriContext& operator=(const NriContext&) = delete;

    // Adopts an existing Vulkan device. NRI must not take ownership.
    bool Initialize(VulkanContext& vk, bool enableNriValidation, std::string& error);

    void Shutdown();

    nri::Device* device() const { return device_; }
    const nri::CoreInterface& core() const { return core_; }
    const nri::WrapperVKInterface& wrapper() const { return wrapper_; }

    // Wraps an existing VkImage. The wrapper does not own the image.
    bool WrapTexture(VkImage image, VkFormat format, VkImageType type,
                     VkImageUsageFlags usage, uint32_t width, uint32_t height,
                     uint32_t depth, uint32_t mipNum, uint32_t layerNum,
                     uint32_t sampleNum, nri::Texture*& out, std::string& error);

    // Destroys the NRI wrapper only. The VkImage must outlive this call.
    void DestroyWrappedTexture(nri::Texture* texture);

    // Verifies NRI reports the same native handles we gave it.
    bool VerifyHandleIdentity(VulkanContext& vk, std::string& error) const;

private:
    nri::Device* device_ = nullptr;
    nri::CoreInterface core_{};
    nri::WrapperVKInterface wrapper_{};
};

}  // namespace harness
