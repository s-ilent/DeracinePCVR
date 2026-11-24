#include "common/nri_adoption.h"

#include <cstring>
#include <vector>

namespace harness {

namespace {

void NriMessageCallback(nri::Message messageType, const char* file, uint32_t line,
                        const char* message, void* /*userArg*/) {
    const char* sev = "INFO";
    if (messageType == nri::Message::ERROR) sev = "ERROR";
    else if (messageType == nri::Message::WARNING) sev = "WARN";
    std::fprintf(stderr, "[NRI %s] %s:%u %s\n", sev, file ? file : "?", line,
                 message ? message : "");
}

}  // namespace

NriContext::~NriContext() { Shutdown(); }

bool NriContext::Initialize(VulkanContext& vk, bool enableNriValidation, std::string& error) {
    const auto& manifest = vk.manifest();

    // Queue families: NRI needs the same family we created.
    std::vector<nri::QueueFamilyVKDesc> families;
    nri::QueueFamilyVKDesc fam{};
    fam.queueNum = 1;
    fam.queueType = nri::QueueType::GRAPHICS;
    fam.familyIndex = vk.graphicsFamily();
    families.push_back(fam);

    // Extensions NRI should consider enabled. These must match what we
    // actually enabled on the VkDevice.
    std::vector<const char*> devExts;
    for (const auto& e : manifest.deviceExtensions) devExts.push_back(e.c_str());
    std::vector<const char*> instExts;
    for (const auto& e : manifest.instanceExtensions) instExts.push_back(e.c_str());

    nri::VKExtensions vkExts{};
    vkExts.instanceExtensions = instExts.empty() ? nullptr : instExts.data();
    vkExts.instanceExtensionNum = static_cast<uint32_t>(instExts.size());
    vkExts.deviceExtensions = devExts.empty() ? nullptr : devExts.data();
    vkExts.deviceExtensionNum = static_cast<uint32_t>(devExts.size());

    nri::DeviceCreationVKDesc desc{};
    desc.vkInstance = vk.instance();
    desc.vkDevice = vk.device();
    desc.vkPhysicalDevice = vk.physicalDevice();
    desc.queueFamilies = families.data();
    desc.queueFamilyNum = static_cast<uint32_t>(families.size());
    desc.minorVersion = 3;
    desc.vkExtensions = vkExts;
    desc.enableNRIValidation = enableNriValidation;
    desc.enableMemoryZeroInitialization = false;
    // We enabled a specific feature set; tell NRI not to assume more.
    desc.unknownFeatures = false;

    nri::CallbackInterface cb{};
    cb.MessageCallback = NriMessageCallback;
    desc.callbackInterface = cb;

    const nri::Result res = nri::nriCreateDeviceFromVKDevice(desc, device_);
    if (res != nri::Result::SUCCESS) {
        error = "nriCreateDeviceFromVKDevice failed: " + std::to_string(static_cast<int>(res));
        return false;
    }

    if (nri::nriGetInterface(*device_, NRI_INTERFACE(nri::CoreInterface), &core_) !=
        nri::Result::SUCCESS) {
        error = "nriGetInterface(CoreInterface) failed";
        return false;
    }
    if (nri::nriGetInterface(*device_, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper_) !=
        nri::Result::SUCCESS) {
        error = "nriGetInterface(WrapperVKInterface) failed";
        return false;
    }
    return true;
}

void NriContext::Shutdown() {
    if (device_ != nullptr) {
        nri::nriDestroyDevice(device_);
        device_ = nullptr;
    }
}

bool NriContext::WrapTexture(VkImage image, VkFormat format, VkImageType type,
                             VkImageUsageFlags usage, uint32_t width, uint32_t height,
                             uint32_t depth, uint32_t mipNum, uint32_t layerNum,
                             uint32_t sampleNum, nri::Texture*& out, std::string& error) {
    nri::TextureVKDesc desc{};
    desc.vkImage = reinterpret_cast<VKNonDispatchableHandle>(image);
    desc.vkFormat = static_cast<VKEnum>(format);
    desc.vkImageType = static_cast<VKEnum>(type);
    desc.vkImageUsageFlags = static_cast<VKFlags>(usage);
    desc.width = static_cast<nri::Dim_t>(width);
    desc.height = static_cast<nri::Dim_t>(height);
    desc.depth = static_cast<nri::Dim_t>(depth);
    desc.mipNum = static_cast<nri::Dim_t>(mipNum);
    desc.layerNum = static_cast<nri::Dim_t>(layerNum);
    desc.sampleNum = static_cast<nri::Sample_t>(sampleNum);

    out = nullptr;
    const nri::Result res = wrapper_.CreateTextureVK(*device_, desc, out);
    if (res != nri::Result::SUCCESS || out == nullptr) {
        error = "CreateTextureVK failed: " + std::to_string(static_cast<int>(res));
        return false;
    }
    return true;
}

void NriContext::DestroyWrappedTexture(nri::Texture* texture) {
    if (texture != nullptr) core_.DestroyTexture(texture);
}

bool NriContext::VerifyHandleIdentity(VulkanContext& vk, std::string& error) const {
    void* nativeDevice = core_.GetDeviceNativeObject(device_);
    if (nativeDevice != reinterpret_cast<void*>(vk.device())) {
        error = "NRI device handle differs from the adopted VkDevice";
        return false;
    }
    void* nativePhys = wrapper_.GetPhysicalDeviceVK(*device_);
    if (nativePhys != reinterpret_cast<void*>(vk.physicalDevice())) {
        error = "NRI physical device handle differs";
        return false;
    }
    void* nativeInst = wrapper_.GetInstanceVK(*device_);
    if (nativeInst != reinterpret_cast<void*>(vk.instance())) {
        error = "NRI instance handle differs";
        return false;
    }
    return true;
}

}  // namespace harness
