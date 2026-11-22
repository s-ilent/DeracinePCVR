# Native OpenXR image ownership — NRI source-level spike

**Date:** 2026-11-23. **Status:** static source inspection only, not a build or OpenXR runtime test.  
**NRI revision:** `06ff6f643777e72eda88bc6fb8d56267b85d872f`.

## Falsifiable question

Can NRI represent **native OpenXR-owned Vulkan swapchain images**, without creating a second Vulkan device or taking ownership of the runtime's images?

### Source observations

1. [`NRIWrapperVK.h`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h) defines `DeviceCreationVKDesc` containing `vkInstance`, `vkDevice`, `vkPhysicalDevice` and queue-family data. `nriCreateDeviceFromVKDevice` accepts this wrapper description, so a Vulkan device created via OpenXR's own `XR_KHR_vulkan_enable2` pathway is a *plausible* input.
2. The same header defines `TextureVKDesc` with a native `vkImage`, format, type, usage bits, dimensions, levels, layers, and samples, and `WrapperVKInterface::CreateTextureVK`.
3. [`Source/VK/TextureVK.hpp`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp) `TextureVK::Create(const TextureVKDesc&)` explicitly sets `m_OwnsNativeObjects = false` and stores `m_Handle = (VkImage)textureVKDesc.vkImage` (lines 76–129).
4. Its destructor only destroys the `VkImage` when `m_OwnsNativeObjects` is true (lines 3–17). Thus the wrapper's destruction **does not destroy the native image**, as required for externally owned OpenXR swapchain surfaces.
5. [`Source/VK/DeviceVK.hpp`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp) uses `isWrapper = descVK.vkDevice != nullptr` and `m_OwnsNativeObjects = !isWrapper` (lines 740–742); the device destructor only destroys native `VkDevice` and `VkInstance` under the ownership guard (lines 728–735). NRI's wrapper therefore supports non-ownership of the OpenXR-created native device.

### What is NOT proven

- No `xrCreateInstance`, Vulkan creation through `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`, `xrCreateSession`, `xrCreateSwapchain`, `xrAcquireSwapchainImage`, or `xrEndFrame` was run.
- No actual NRI command buffer was recorded against an OpenXR `VkImage`; no image-layout, synchronization, view-layer, format-compatibility, or zero-copy test passed.
- Non-ownership of `VkImage` and `VkDevice` is source-supported. Whether NRI has a safe policy for command-buffer destruction and queue ownership across `xrEndFrame` requires testing.
- No claim about Plume or NVRHI being unable to implement the same functionality; only their published public interfaces are less direct for this exact use case.

## Next executable test

Create a **standalone** native Vulkan+OpenXR harness, not part of shadPS4:

1. Use `XR_KHR_vulkan_enable2` to create the Vulkan instance/device and session. Use the runtime-selected graphics device and an appropriate queue family.
2. Create a 2D array swapchain when supported; call `xrEnumerateSwapchainImages` and wrap each `XrSwapchainImageVulkan2KHR.image` through `CreateTextureVK`.
3. Verify the wrapped native `VkImage` address/handle is unchanged, and that deleting the NRI wrapper does **not** call `vkDestroyImage` on the runtime-owned image.
4. Draw different per-eye patterns into the acquired image with correct layouts and barriers; release it and submit a native `XrCompositionLayerProjection`.
5. Test image count / acquisition, layer/array-slice bindings, format support, external queue synchronization, recreation and teardown. Run validation layers, and reject any ownership mismatch or forced copy.

First fall-back is direct Vulkan, not a shadPS4 image-export bridge.

**Decision:** Source-level gate *passes* for NRI's external Vulkan device/image ownership model. Runtime render/present gate remains *unverified*.
