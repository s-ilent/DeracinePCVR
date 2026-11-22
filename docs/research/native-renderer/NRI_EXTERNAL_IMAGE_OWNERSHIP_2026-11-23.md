# Native renderer: NRI Vulkan external-image ownership boundary

**Date:** 2026-11-23. **Scope:** one source-level investigation, no runtime test. **Research branch:** `research/native-nri-external-image-ownership-20261123`. **Base:** foundation commit `313f277c7ac5a19a5de50cdd460c6a8568103225`; `main` independently observed at `e2f981c40cba7c8490177c20c179cb3608cc1442` before writing. No changes to main, emulator or game executable.

## Question

Can an independently owned native OpenXR Vulkan device and its runtime-owned swapchain `VkImage` handles be represented in NRI **without NRI taking ownership of the underlying device or image**? What does this *not* establish?

## Pinned upstream evidence

All source observations below refer to NRI revision `06ff6f643777e72eda88bc6fb8d56267b85d872f`, not an unpinned README.

1. [`Include/Extensions/NRIWrapperVK.h`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L19-L45) declares `DeviceCreationVKDesc` with externally supplied `vkInstance`, `vkDevice`, `vkPhysicalDevice`, queue-family descriptors, enabled extensions and feature assumptions; `nriCreateDeviceFromVKDevice` is the adoption entry point ([same file](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L104-L125)).
2. [`Source/VK/DeviceVK.hpp` lines 728–743](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L728-L743) sets `isWrapper = descVK.vkDevice != nullptr`, then `m_OwnsNativeObjects = !isWrapper`; destructor calls `DestroyDevice`/`DestroyInstance` only when that flag is true. **Observed:** wrapping an existing Vulkan device prevents NRI from destroying those native objects.
3. [`Include/Extensions/NRIWrapperVK.h` `TextureVKDesc`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L62-L73) accepts `vkImage`, format, image type, Vulkan usage flags, dimensions, mip/layer/sample counts. `WrapperVKInterface::CreateTextureVK` accepts this descriptor.
4. [`Source/VK/TextureVK.hpp` lines 76–129](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L76-L129) converts image metadata and usage flags to an NRI texture description, sets `m_OwnsNativeObjects = false`, and retains the supplied `VkImage`. Its [destructor, lines 3–17](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L3-L17), destroys the native image only if `m_OwnsNativeObjects` is true. **Observed:** destruction of the wrapper does not destroy the OpenXR-owned `VkImage`.
5. [`Source/VK/TextureVK.hpp` lines 183–195](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L183-L195) explicitly rejects `BindMemory` for wrapped objects. **Implication:** OpenXR images must remain externally allocated/bound; do not route them through NRI's placed-image allocation path.
6. [`Source/VK/DescriptorVK.hpp`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorVK.hpp) contains both `CreateImageView` and `DestroyImageView`. **Implication:** non-ownership of `VkImage` does not mean NRI-created views/descriptors can outlive the image or its Vulkan device.

## Result and architectural boundary

**Source-supported, not runtime-validated:** NRI's wrapper route supports a native OpenXR host owning the Vulkan instance, device and swapchain images. NRI may own *wrappers and image views*, but not the underlying adopted device/image. The native OpenXR host should:

1. Create an OpenXR-compatible Vulkan device through the runtime's Vulkan enable2 route; select and pass the actual enabled feature/extension/queue configuration into NRI's device wrapper. The wrapper header explicitly warns that all supported Vulkan 1.0 features must be enabled and documents `unknownFeatures` behavior; do not assume an arbitrary OpenXR-created device satisfies this.
2. Enumerate OpenXR swapchain `VkImage` handles and wrap each index separately, using the exact runtime format, usage flags, layer count and sample count. No NRI WSI swapchain is needed.
3. Preserve the previously documented acquire/wait/release state machine independently of NRI. NRI's `CreateTextureVK` does not acquire, wait, release, synchronize or present OpenXR images.
4. On swapchain recreation or destruction: retire GPU work, destroy NRI-created image views/descriptors and wrappers, then destroy the OpenXR swapchain. On session/device teardown: destroy NRI device wrapper before the host destroys the native Vulkan device.

**Not proven:** successful graphics rendering into an OpenXR image, compatibility of the exact Vulkan feature/extension set, correct Vulkan layout transitions on OpenXR release, queue-family handoff, array-layer multiview, NRI validation cleanliness, or actual headset runtime support. These require a real Vulkan/OpenXR test. This result does not establish a UE4 RHI hook, shader provenance, or a native game frame.

## Next falsifiable experiment

Build a **title-independent** host harness with a real `XR_KHR_vulkan_enable2` runtime and Vulkan validation layers. Use runtime-created Vulkan instance/device, create an OpenXR swapchain, enumerate `XrSwapchainImageVulkan2KHR` images, wrap the device and images through NRI, create per-layer render-target views, clear each eye differently, submit and release with explicit Vulkan synchronization, then recreate/destroy the swapchain. Assert:

- NRI never calls `vkDestroyImage`, `vkDestroyDevice` or `vkDestroyInstance` for adopted objects (instrument dispatch).
- NRI-created image views are destroyed before swapchain destruction.
- No writes before `xrWaitSwapchainImage == XR_SUCCESS`; no GPU references survive teardown.
- Vulkan/OpenXR validation reports zero errors; test both array-layer and separate-eye paths when supported.

**Blocker:** no native OpenXR/Vulkan runtime was exercised in this source-only run. This is a **device ownership proof from code**, not an executable compatibility claim. Keep game/runtime recovery and shader translation as separate tracks.
