# Plume Vulkan/OpenXR adoption boundary — exact-revision audit

**Date:** 2026-11-23  
**Track:** Native PCVR Renderer Foundation (independent of Déraciné Binary Atlas and shadPS4 renderer)  
**Target library:** renderbag/plume @ `230c502dfe9c2e02a1ad4321b3e1ada46c45f876` (GitHub main resolved to this SHA during audit)  
**Evidence:** pinned public source only. No GPU, headset, game executable, or runtime test.

## Question

Can unmodified Plume's public Vulkan backend adopt an OpenXR-created `VkInstance`, `VkPhysicalDevice`, `VkDevice`, queue and runtime-owned `VkImage`, instead of creating its own device and WSI swapchain?

## Direct evidence (pinned source and line ranges)

1. **Public interface is creation-oriented.** `RenderInterface::createDevice(const std::string&)` has no native-handle arguments; `RenderDevice::createTexture(const RenderTextureDesc&)` has no external-image argument. `RenderCommandQueue::createSwapChain(const RenderSwapChainDesc&)` exposes Plume's own swapchain. [`plume_render_interface.h` lines 193–260](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_render_interface.h#L193-L260). (Line anchors should be verified against the linked revision; signatures are source-confirmed.)
2. **Instance creation is internal.** `VulkanInterface::VulkanInterface` initializes Volk, configures a Vulkan 1.2 application and calls `vkCreateInstance` at line 4626. The destructor calls `vkDestroyInstance` at 4655–4659. `createDevice` constructs `VulkanDevice(this, preferredDeviceName)` at 4661–4663. [`plume_vulkan.cpp` lines 4503–4663](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4503-L4663).
3. **Logical device creation is internal.** `VulkanDevice` enumerates physical devices from `renderInterface->instance` at 3776–3785, calls `vkCreateDevice` at 4091, and unconditionally calls `vkDestroyDevice` when its handle is non-null at 4480–4483. [`plume_vulkan.cpp` lines 3776–4095](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L3776-L4095), [lines 4475–4484](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4475-L4484).
4. **Important nuance: non-owning image support *does* exist internally.** `VulkanTexture(VulkanDevice*, VkImage)` stores the external image without setting `ownership=true` (1026–1032); `ownership` defaults to false in `plume_vulkan.h` line 89. Its destructor destroys image views, but only calls `vmaDestroyImage` when `ownership` is true (1034–1041). [`plume_vulkan.cpp` lines 972–1041](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L972-L1041), [`plume_vulkan.h` lines 78–99](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.h#L78-L99).
5. **That internal path is used for Plume's own WSI swapchain.** `VulkanSwapChain` calls `vkCreateSwapchainKHR` at 2361, enumerates its `VkImage` handles at 2378–2388, constructs `VulkanTexture(commandQueue->device, images[i])` at 2395–2408, and populates the texture descriptor and view. [`plume_vulkan.cpp` lines 2355–2409](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L2355-L2409).
6. **Plume's Vulkan backend requires WSI-related extensions by default.** `RequiredInstanceExtensions` includes `VK_KHR_surface` and a platform surface extension; `RequiredDeviceExtensions` includes `VK_KHR_swapchain`. These are not evidence that OpenXR itself requires WSI. [`plume_vulkan.cpp` lines 42–82](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L42-L82).

## Verdict

**Verified:** Plume's pinned Vulkan backend has a non-owning `VkImage` wrapper, but exposes no public adopted-device or external-image creation path in the audited `RenderInterface`/`RenderDevice` interfaces. The stock device path creates and destroys its own instance/device, and the stock swapchain is WSI-based.

**Inference:** An OpenXR-compatible Plume backend would require a separately maintained adapter/fork or new API for adopting host handles and managing ownership; simply constructing an internal `VulkanTexture` is insufficient because its `VulkanDevice` and command queues must operate on the runtime-compatible device.

**Not established:** Whether a small, safe upstream-compatible adapter is feasible, how Plume handles external-image layout/reacquisition, or whether its existing barrier model correctly handles OpenXR attachment layouts and queue ownership. No compile or validation run was performed.

## Concrete adapter requirements

- **Adopted device descriptor:** `VkInstance`, `VkPhysicalDevice`, `VkDevice`, graphics `VkQueue` and family index, enabled features/extensions, ownership flags. Never enumerate and replace the OpenXR-selected physical device.
- **Ownership flags:** independently control instance/device/queue/image destruction; externally owned objects must survive Plume wrapper teardown.
- **External texture descriptor:** `VkImage`, format, extent, layers, mip levels, samples, usage and known current layout, with explicit non-owning semantics. Construct the image view with the correct array-layer range; don't hardcode the WSI single-layer description.
- **Submission contract:** OpenXR acquire/wait/release belongs to the native host, not `RenderSwapChain::present`. Explicitly track transitions, graphics queue serialization and completion before wrapper teardown.
- **Validation:** reject unsupported queue family/feature combinations and missing external metadata instead of silently creating a second device or copying images.

## Focused checks actually performed

- Read pinned `plume_render_interface.h`, `plume_vulkan.h`, and complete 4,692-line `plume_vulkan.cpp` through GitHub.
- Located the sole `vkCreateInstance` (4626), `vkCreateDevice` (4091), `vkDestroyDevice` (4481), `vkCreateSwapchainKHR` (2361), `vkGetSwapchainImagesKHR` (2378, 2388), and non-owning texture constructor (1026).
- Confirmed public `createDevice` and `createTexture` signatures and the internal WSI wrapper call.
- No binary/GPU tests: these checks establish source structure, not runtime correctness.

## Next falsifiable experiment

Implement a **standalone, minimal host-only Plume adapter prototype** on a separate branch, without Déraciné or shadPS4 code:

1. Create a Vulkan instance/device externally (OpenXR-compatible configuration, or ordinary host Vulkan for the first pass).
2. Adopt handles into a minimally modified Plume device, with ownership flags; wrap a two-layer external color image.
3. Record a draw to one layer and then both layers; run Vulkan validation.
4. Destroy the Plume wrappers; confirm host `VkImage`, `VkDevice` and `VkInstance` remain usable.
5. Reject the candidate if it creates a second Vulkan device, destroys externally owned handles, cannot represent two layers, or requires a hidden image copy.

**Architectural decision:** NRI remains the lower-risk initial native OpenXR candidate because it publicly supports device/image wrapping. Plume remains a valid competing renderer for a host-owned device workload, but OpenXR adoption is a concrete integration gap, not just an unspecified feature. This is a source-based compatibility judgment, not a performance ranking.

**Non-goals:** no PM4/GNM work, no shadPS4 renderer integration, no PS4 binary modifications, no shader-code extraction, no FPS claims. Non-rendering guest runtime compatibility remains a separate problem.
