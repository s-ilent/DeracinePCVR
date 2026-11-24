#include "common/vk_bootstrap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace harness {

namespace {

ValidationCounters g_counters;

const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/) {
    const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool isWarning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    const bool isSync = (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0 &&
                        data->pMessageIdName != nullptr &&
                        std::strstr(data->pMessageIdName, "SYNC-") != nullptr;

    if (isError) {
        g_counters.errors++;
        if (isSync) g_counters.syncErrors++;
    } else if (isWarning) {
        g_counters.warnings++;
    }

    if (isError || isWarning) {
        std::ostringstream os;
        os << (isError ? "[VK ERROR] " : "[VK WARN] ")
           << (data->pMessageIdName ? data->pMessageIdName : "?") << ": "
           << (data->pMessage ? data->pMessage : "");
        g_counters.messages.push_back(os.str());
        std::fprintf(stderr, "%s\n", os.str().c_str());
    }
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> InstanceExtensions() {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) != VK_SUCCESS) return {};
    return exts;
}

bool InstanceHasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice dev) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr) != VK_SUCCESS) return {};
    std::vector<VkExtensionProperties> exts(count);
    if (vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data()) != VK_SUCCESS) return {};
    return exts;
}

bool DeviceHasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

}  // namespace

ValidationCounters& GlobalValidationCounters() { return g_counters; }

std::string DeviceManifest::Describe() const {
    std::ostringstream os;
    os << "device: " << deviceName << "\n";
    os << "  apiVersion: " << VK_VERSION_MAJOR(apiVersion) << "."
       << VK_VERSION_MINOR(apiVersion) << "." << VK_VERSION_PATCH(apiVersion) << "\n";
    os << "  driverVersion: 0x" << std::hex << driverVersion << std::dec << "\n";
    os << "  type: " << static_cast<int>(deviceType) << (isSoftware ? " (SOFTWARE)" : "") << "\n";
    os << "  maxViewCount: " << maxViewCount << "\n";
    os << "  maxColorAttachments: " << maxColorAttachments << "\n";
    os << "  maxFramebuffer: " << maxFramebufferWidth << "x" << maxFramebufferHeight << "\n";
    os << "  features:";
    auto add = [&](const char* n, bool v) { if (v) os << " " << n; };
    add("timelineSemaphore", timelineSemaphore);
    add("synchronization2", synchronization2);
    add("dynamicRendering", dynamicRendering);
    add("multiview", multiview);
    add("separateDepthStencilLayouts", separateDepthStencilLayouts);
    add("maintenance4", maintenance4);
    add("maintenance5", maintenance5);
    add("maintenance8", maintenance8);
    add("descriptorIndexing", descriptorIndexing);
    add("bufferDeviceAddress", bufferDeviceAddress);
    add("sampleRateShading", sampleRateShading);
    add("geometryShader", geometryShader);
    add("tessellationShader", tessellationShader);
    os << "\n";
    os << "  queues:";
    for (const auto& q : queues) {
        os << " [fam=" << q.familyIndex << " idx=" << q.queueIndex
           << " n=" << q.count << " flags=0x" << std::hex << q.flags << std::dec << "]";
    }
    os << "\n";
    os << "  deviceExtensions: " << deviceExtensions.size() << "\n";
    return os.str();
}

VulkanContext::~VulkanContext() { Shutdown(); }

bool VulkanContext::CreateInstance(const BootstrapConfig& config, std::string& error) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "DeracineNativeHarness";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "None";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> exts;

    const auto available = InstanceExtensions();
    if (InstanceHasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    if (config.enableValidation && HasLayer(kValidationLayer)) {
        layers.push_back(kValidationLayer);
        validationActive_ = true;
    }

    for (const char* e : config.extraInstanceExtensions) {
        if (InstanceHasExtension(available, e)) exts.push_back(e);
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();

    // Synchronization validation is requested through the layer settings
    // extension when the layer supports it.
    VkLayerSettingsCreateInfoEXT layerSettings{};
    VkBool32 syncValEnabled = VK_TRUE;
    VkLayerSettingEXT setting{};
    if (config.enableSyncValidation && validationActive_ &&
        InstanceHasExtension(available, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();

        setting.pLayerName = kValidationLayer;
        setting.pSettingName = "validate_sync";
        setting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
        setting.valueCount = 1;
        setting.pValues = &syncValEnabled;

        layerSettings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
        layerSettings.settingCount = 1;
        layerSettings.pSettings = &setting;
        ci.pNext = &layerSettings;
        syncValidationActive_ = true;
    }

    const VkResult res = vkCreateInstance(&ci, nullptr, &instance_);
    if (res != VK_SUCCESS) {
        error = "vkCreateInstance failed: " + std::to_string(res);
        return false;
    }

    for (const char* e : exts) instanceExtensions_.push_back(e);

    if (validationActive_ && InstanceHasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (createFn) {
            VkDebugUtilsMessengerCreateInfoEXT dci{};
            dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = DebugCallback;
            createFn(instance_, &dci, nullptr, &messenger_);
        }
    }
    return true;
}

bool VulkanContext::PickPhysicalDevice(const BootstrapConfig& config, std::string& error) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance_, &count, nullptr) != VK_SUCCESS || count == 0) {
        error = "no Vulkan physical devices";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);

        const bool isSoftware = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (isSoftware && !config.allowSoftwareDevice) continue;

        uint32_t famCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, nullptr);
        std::vector<VkQueueFamilyProperties> fams(famCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, fams.data());
        bool hasGraphics = false;
        for (const auto& f : fams) {
            if (f.queueFlags & VK_QUEUE_GRAPHICS_BIT) hasGraphics = true;
        }
        if (!hasGraphics) continue;

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
        if (isSoftware) score += 10;
        if (config.preferDiscreteGpu && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100;

        if (score > bestScore) {
            bestScore = score;
            physicalDevice_ = dev;
        }
    }

    if (physicalDevice_ == VK_NULL_HANDLE) {
        error = "no suitable physical device";
        return false;
    }
    return true;
}

bool VulkanContext::CreateLogicalDevice(const BootstrapConfig& config, std::string& error) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);

    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &famCount, fams.data());

    graphicsFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < famCount; ++i) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily_ = i;
            break;
        }
    }
    if (graphicsFamily_ == UINT32_MAX) {
        error = "no graphics queue family";
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    // Query what the device supports, then enable only what we ask for.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &supported);

    VkPhysicalDeviceVulkan11Features supported11{};
    supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    VkPhysicalDeviceFeatures2 q2{};
    q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    q2.pNext = &supported11;
    supported11.pNext = &supported12;
    supported12.pNext = &supported13;
    vkGetPhysicalDeviceFeatures2(physicalDevice_, &q2);

    VkPhysicalDeviceFeatures enabled{};
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    enabled.depthClamp = supported.depthClamp;
    enabled.independentBlend = supported.independentBlend;
    enabled.fillModeNonSolid = supported.fillModeNonSolid;
    enabled.wideLines = supported.wideLines;
    enabled.imageCubeArray = supported.imageCubeArray;
    enabled.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
    enabled.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
    enabled.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    enabled.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    enabled.shaderFloat64 = supported.shaderFloat64;
    enabled.shaderInt64 = supported.shaderInt64;
    enabled.shaderInt16 = supported.shaderInt16;
    enabled.depthBiasClamp = supported.depthBiasClamp;
    enabled.logicOp = supported.logicOp;
    enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    enabled.pipelineStatisticsQuery = supported.pipelineStatisticsQuery;
    enabled.geometryShader = supported.geometryShader;
    enabled.tessellationShader = supported.tessellationShader;
    enabled.dualSrcBlend = supported.dualSrcBlend;
    enabled.multiDrawIndirect = supported.multiDrawIndirect;
    enabled.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;
    enabled.alphaToOne = supported.alphaToOne;
    enabled.sparseBinding = supported.sparseBinding;
    enabled.textureCompressionBC = supported.textureCompressionBC;
    enabled.textureCompressionASTC_LDR = supported.textureCompressionASTC_LDR;
    enabled.textureCompressionETC2 = supported.textureCompressionETC2;

    VkPhysicalDeviceVulkan11Features enabled11{};
    enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    enabled11.multiview = supported11.multiview;
    enabled11.multiviewGeometryShader = supported11.multiviewGeometryShader;
    enabled11.shaderDrawParameters = supported11.shaderDrawParameters;

    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.timelineSemaphore = supported12.timelineSemaphore;
    enabled12.bufferDeviceAddress = supported12.bufferDeviceAddress;
    enabled12.descriptorIndexing = supported12.descriptorIndexing;
    enabled12.hostQueryReset = supported12.hostQueryReset;
    enabled12.drawIndirectCount = supported12.drawIndirectCount;
    enabled12.scalarBlockLayout = supported12.scalarBlockLayout;
    enabled12.uniformBufferStandardLayout = supported12.uniformBufferStandardLayout;
    enabled12.shaderSubgroupExtendedTypes = supported12.shaderSubgroupExtendedTypes;
    enabled12.separateDepthStencilLayouts = supported12.separateDepthStencilLayouts;
    enabled12.shaderSampledImageArrayNonUniformIndexing =
        supported12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.descriptorBindingVariableDescriptorCount =
        supported12.descriptorBindingVariableDescriptorCount;
    enabled12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;

    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.synchronization2 = supported13.synchronization2;
    enabled13.dynamicRendering = supported13.dynamicRendering;
    enabled13.maintenance4 = supported13.maintenance4;

    // maintenance5/8 live in separate feature structs.
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{};
    m5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
    VkPhysicalDeviceMaintenance8FeaturesKHR m8{};
    m8.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR;

    const auto devExts = DeviceExtensions(physicalDevice_);
    const bool hasM5 = DeviceHasExtension(devExts, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    const bool hasM8 = DeviceHasExtension(devExts, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);

    std::vector<const char*> devExtNames;
    if (hasM5) devExtNames.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    if (hasM8) devExtNames.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    for (const char* e : config.extraDeviceExtensions) {
        if (DeviceHasExtension(devExts, e)) devExtNames.push_back(e);
    }

    void* chain = nullptr;
    if (hasM8) {
        m8.maintenance8 = VK_TRUE;
        m8.pNext = chain;
        chain = &m8;
    }
    if (hasM5) {
        m5.maintenance5 = VK_TRUE;
        m5.pNext = chain;
        chain = &m5;
    }
    enabled13.pNext = chain;
    enabled12.pNext = &enabled13;
    enabled11.pNext = &enabled12;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enabled11;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enabled;
    dci.enabledExtensionCount = static_cast<uint32_t>(devExtNames.size());
    dci.ppEnabledExtensionNames = devExtNames.data();

    const VkResult res = vkCreateDevice(physicalDevice_, &dci, nullptr, &device_);
    if (res != VK_SUCCESS) {
        error = "vkCreateDevice failed: " + std::to_string(res);
        return false;
    }

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);

    // Record what was actually enabled.
    manifest_.deviceName = props.deviceName;
    manifest_.apiVersion = props.apiVersion;
    manifest_.driverVersion = props.driverVersion;
    manifest_.deviceType = props.deviceType;
    manifest_.isSoftware = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    manifest_.timelineSemaphore = enabled12.timelineSemaphore == VK_TRUE;
    manifest_.synchronization2 = enabled13.synchronization2 == VK_TRUE;
    manifest_.dynamicRendering = enabled13.dynamicRendering == VK_TRUE;
    manifest_.multiview = enabled11.multiview == VK_TRUE;
    manifest_.multiviewGeometryShader = enabled11.multiviewGeometryShader == VK_TRUE;
    manifest_.separateDepthStencilLayouts = enabled12.separateDepthStencilLayouts == VK_TRUE;
    manifest_.maintenance4 = enabled13.maintenance4 == VK_TRUE;
    manifest_.maintenance5 = hasM5;
    manifest_.maintenance8 = hasM8;
    manifest_.descriptorIndexing = enabled12.descriptorIndexing == VK_TRUE;
    manifest_.bufferDeviceAddress = enabled12.bufferDeviceAddress == VK_TRUE;
    manifest_.hostQueryReset = enabled12.hostQueryReset == VK_TRUE;
    manifest_.samplerAnisotropy = enabled.samplerAnisotropy == VK_TRUE;
    manifest_.depthClamp = enabled.depthClamp == VK_TRUE;
    manifest_.independentBlend = enabled.independentBlend == VK_TRUE;
    manifest_.fillModeNonSolid = enabled.fillModeNonSolid == VK_TRUE;
    manifest_.imageCubeArray = enabled.imageCubeArray == VK_TRUE;
    manifest_.shaderStorageImageWriteWithoutFormat =
        enabled.shaderStorageImageWriteWithoutFormat == VK_TRUE;
    manifest_.shaderStorageImageReadWithoutFormat =
        enabled.shaderStorageImageReadWithoutFormat == VK_TRUE;
    manifest_.fragmentStoresAndAtomics = enabled.fragmentStoresAndAtomics == VK_TRUE;
    manifest_.vertexPipelineStoresAndAtomics = enabled.vertexPipelineStoresAndAtomics == VK_TRUE;
    manifest_.shaderFloat64 = enabled.shaderFloat64 == VK_TRUE;
    manifest_.shaderInt64 = enabled.shaderInt64 == VK_TRUE;
    manifest_.shaderInt16 = enabled.shaderInt16 == VK_TRUE;
    manifest_.sampleRateShading = supported.sampleRateShading == VK_TRUE;
    manifest_.geometryShader = enabled.geometryShader == VK_TRUE;
    manifest_.tessellationShader = enabled.tessellationShader == VK_TRUE;
    manifest_.textureCompressionBC = enabled.textureCompressionBC == VK_TRUE;

    VkPhysicalDeviceMultiviewProperties mvProps{};
    mvProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &mvProps;
    vkGetPhysicalDeviceProperties2(physicalDevice_, &props2);
    manifest_.maxViewCount = mvProps.maxMultiviewViewCount;
    manifest_.maxViewportDimensions[0] = props.limits.maxViewportDimensions[0];
    manifest_.maxViewportDimensions[1] = props.limits.maxViewportDimensions[1];
    manifest_.maxColorAttachments = props.limits.maxColorAttachments;
    manifest_.maxFramebufferWidth = props.limits.maxFramebufferWidth;
    manifest_.maxFramebufferHeight = props.limits.maxFramebufferHeight;
    manifest_.framebufferColorSampleCounts = props.limits.framebufferColorSampleCounts;
    manifest_.framebufferDepthSampleCounts = props.limits.framebufferDepthSampleCounts;
    manifest_.framebufferStencilSampleCounts = props.limits.framebufferStencilSampleCounts;

    for (const char* e : devExtNames) manifest_.deviceExtensions.push_back(e);
    for (const auto& e : instanceExtensions_) manifest_.instanceExtensions.push_back(e);

    DeviceManifest::QueueInfo qi;
    qi.flags = fams[graphicsFamily_].queueFlags;
    qi.familyIndex = graphicsFamily_;
    qi.queueIndex = 0;
    qi.count = fams[graphicsFamily_].queueCount;
    manifest_.queues.push_back(qi);

    return true;
}

bool VulkanContext::Initialize(const BootstrapConfig& config, std::string& error) {
    if (!CreateInstance(config, error)) return false;
    if (!PickPhysicalDevice(config, error)) return false;
    if (!CreateLogicalDevice(config, error)) return false;
    return true;
}

void VulkanContext::Shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (messenger_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyFn) destroyFn(instance_, messenger_, nullptr);
        messenger_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

uint32_t VulkanContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkCommandPool VulkanContext::CreateCommandPool(uint32_t familyIndex) const {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = familyIndex;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device_, &ci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
}

VkCommandBuffer VulkanContext::BeginOneShot(VkCommandPool pool) const {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanContext::EndOneShot(VkCommandPool pool, VkCommandBuffer cmd) const {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphicsQueue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);
    vkFreeCommandBuffers(device_, pool, 1, &cmd);
}

}  // namespace harness
