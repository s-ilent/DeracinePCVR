# Native OpenXR swapchain image ownership and NRI wrapping — source audit

**Date:** 2026-11-23  
**Track:** Native PCVR Renderer Foundation (independent of Binary Atlas and shadPS4 live work)  
**Status:** Pinned-source analysis; no headset, Vulkan GPU, or runtime test.  
**Repository base:** `research/native-rhi-foundation-20261123` at `6b359f520b9f41f854b2aac41bb74526f6c60e87`; `main` independently observed at `e2f981c40cba7c8490177c20c179cb3608cc1442`.  
**Scope:** Ownership boundary between a native OpenXR session, Vulkan device, swapchain images, and NRI texture wrappers. This does not revisit descriptor-pool epochs or UE4 deferred parameter ownership.

## Primary source revisions

1. Khronos `OpenXR-SDK-Source` commit [`040046ba7934bf01265b6e95b8a2bd674a2f20f9`](https://github.com/KhronosGroup/OpenXR-SDK-Source/commit/040046ba7934bf01265b6e95b8a2bd674a2f20f9), 2026-11-18:
   - [`src/tests/hello_xr/graphicsplugin_vulkan.cpp` lines 469–470, 509–520, 585–653, 783–803, 1063–1100](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/040046ba7934bf01265b6e95b8a2bd674a2f20f9/src/tests/hello_xr/graphicsplugin_vulkan.cpp)
   - [`src/tests/hello_xr/openxr_program.cpp` lines 681–755, 1047–1090](https://github.com/KhronosGroup/OpenXR-SDK-Source/blob/040046ba7934bf01265b6e95b8a2bd674a2f20f9/src/tests/hello_xr/openxr_program.cpp)
2. NVIDIA NRI commit [`06ff6f643777e72eda88bc6fb8d56267b85d872f`](https://github.com/NVIDIA-RTX/NRI/tree/06ff6f643777e72eda88bc6fb8d56267b85d872f):
   - [`Include/Extensions/NRIWrapperVK.h` lines 25–89, 115–140](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h)
   - [`Include/NRIDescs.h` lines 658–690, 987–1005](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h)

These are public source snapshots, not observed Déraciné behavior. The Khronos sample is implementation evidence, **not** a substitute for normative OpenXR specification requirements.

## Verified interface boundaries

### 1. OpenXR must select/create the Vulkan device before NRI adoption

`hello_xr` requests `XR_KHR_vulkan_enable2`, queries `XrGraphicsRequirementsVulkan2KHR`, creates the Vulkan instance through `xrCreateVulkanInstanceKHR`, obtains the runtime-selected physical device via `xrGetVulkanGraphicsDevice2KHR`, creates the device through `xrCreateVulkanDeviceKHR`, and passes instance, physical device, device, queue family and queue index through `XrGraphicsBindingVulkan2KHR` (graphicsplugin lines 470, 515, 585–653). These operations precede `xrCreateSession` (openxr_program line 604).

NRI's `DeviceCreationVKDesc` explicitly accepts `vkInstance`, `vkPhysicalDevice`, `vkDevice`, queue-family descriptions and enabled extension/feature metadata (NRIWrapperVK.h lines 27–46). This supports the **proposed ownership direction**: native OpenXR/Vulkan bootstrap owns the device; NRI adopts it, rather than independently choosing an adapter. Exact NRI feature compatibility with the runtime-created device remains untested.

**Implementation invariant:** never create a separate NRI-owned Vulkan device and assume it can render into the session's `VkImage` objects. Record selected physical device, enabled Vulkan features/extensions and queue-family identity in a bootstrap manifest and validate NRI creation against that manifest.

### 2. OpenXR owns swapchain images; NRI receives an explicit image description

The sample creates OpenXR swapchains with `xrCreateSwapchain`, enumerates runtime images via `xrEnumerateSwapchainImages` (openxr_program lines 687–755), and renders only after `xrAcquireSwapchainImage` and `xrWaitSwapchainImage`, then calls `xrReleaseSwapchainImage` (lines 1050–1090).

NRI's `TextureVKDesc` accepts an external `vkImage` plus `vkFormat`, `vkImageType`, usage flags, width/height/depth, mip/layer counts and sample count (NRIWrapperVK.h lines 73–84). Its `CreateTextureVK` is exposed in the wrapper interface (line 123). Thus the intended wrapper can be created from the **actual enumerated image** and the **matching OpenXR swapchain creation metadata**.

**Proposed ownership table (architecture, not yet tested):**

| Object | Authoritative owner | Native adapter treatment |
|---|---|---|
| `XrInstance`, `XrSession`, `XrSwapchain` | OpenXR host layer | Own lifecycle |
| `VkInstance`, `VkDevice`, `VkQueue` | Native Vulkan/OpenXR bootstrap | Pass existing handles to NRI |
| `VkImage` enumerated from OpenXR | OpenXR runtime | Borrow; never allocate/free image memory or destroy image |
| NRI `Texture` wrapper | NRI adapter | Destroy wrapper on teardown after GPU idle, before destroying its Vulkan device/swapchain |
| Per-image Vulkan image views/framebuffers | Host/NRI adapter (as actually created) | Explicitly destroy after GPU completion; never assume runtime owns these views |
| Image acquisition state | OpenXR host layer | Key by swapchain **and acquired image index**; not just eye number |

A wrapper's destruction behavior with an external `VkImage` must be confirmed in NRI's pinned Vulkan implementation before coding this ownership policy. Header support alone does **not** prove non-destruction of the underlying image.

### 3. Stereo and depth cannot be reduced to one image-index assumption

The pinned `hello_xr` example creates **one color swapchain per view** with `arraySize = 1` (openxr_program lines 681–709). This is an example strategy, not a universal OpenXR rule: our renderer must support both separate-eye swapchains and array-layer rendering where supported.

**Source-level hazard in the sample:** in the optional depth path, after creating `depthSwapchain.handle`, the code at openxr_program line 733 calls `xrEnumerateSwapchainImages(swapchain.handle, 0, &depthImageCount, nullptr)` using the **color** handle, not the depth handle. It then requires `depthImageCount == imageCount` (lines 736–739). This cannot establish the depth swapchain's actual image count; it appears to be a sample bug. The subsequent depth enumeration at lines 745–746 does use the depth handle.

For a portable implementation:
- Enumerate **each** color/depth swapchain using its own handle and independent image count.
- Do not assume independently created color and depth swapchains have identical image counts or index ordering.
- Maintain separate acquisition/wait/release bookkeeping for each swapchain.
- Bind the actually acquired depth image, not a color index reused without verification.
- Handle an unavailable depth swapchain or unsupported depth format with an explicitly selected fallback; never fabricate a matching runtime image.
- Preserve per-view `imageArrayIndex`, `imageRect`, pose and FOV when building projection layer views.

This is a new portability risk distinct from the previous descriptor-epoch investigation.

## Proposed minimal host boundary

```text
NativeOpenXRBootstrap
  -> xrGetSystem / XR_KHR_vulkan_enable2 graphics requirements
  -> xrCreateVulkanInstanceKHR / xrGetVulkanGraphicsDevice2KHR
  -> xrCreateVulkanDeviceKHR / native graphics queue
  -> xrCreateSession with XrGraphicsBindingVulkan2KHR
  -> NRI CreateDeviceFromVKDevice (same native handles)
  -> xrCreateSwapchain + enumerate runtime-owned VkImages
  -> NRI CreateTextureVK per (swapchain, image index)
  -> frame: acquire -> wait -> NRI render -> GPU completion contract -> release
  -> xrEndFrame with independently specified per-eye projection subimages
```

The `GPU completion contract` is deliberately not yet defined as a blanket `vkQueueWaitIdle`: inspect the normative OpenXR release synchronization requirements and validate the concrete NRI submission path before selecting fences, waits or layouts. No copy/blit is assumed between NRI output and the OpenXR runtime image.

## Falsifiable next experiment

Build a **standalone, game-asset-free** native OpenXR/Vulkan/NRI integration smoke test, distinct from the existing headless operation-tape benchmark.

1. Create the Vulkan device using `XR_KHR_vulkan_enable2` and record the runtime-selected physical device and queue family.
2. Adopt that exact device through pinned NRI; fail if required features/extensions are incompatible.
3. Enumerate color images and optional depth images **independently**. Log counts, dimensions, sample counts, formats and per-swapchain acquired indices.
4. Wrap each runtime `VkImage` with NRI `CreateTextureVK`. Verify with Vulkan validation that no external image is destroyed or freed by NRI teardown.
5. Render two distinguishable per-eye colors with trivial host-native shaders, using acquired images and correct image layouts; release and submit projection layers.
6. Exercise separate-eye swapchains and an array-layer mode when runtime supports both. Verify that color/depth index bookkeeping remains independent.
7. Test teardown/recreation after a session/swapchain restart; detect stale wrapper use via a host-side swapchain generation counter.

**Pass:** successful native session, correct selected device, zero Vulkan validation errors, no external-image destruction, correct per-eye output, and no stale wrapper use after recreation.

**Fail:** device mismatch, incorrect image/index/layer mapping, unsupported NRI adoption, image lifetime violation, invalid layout/synchronization, or inability to submit two distinct views.

**Inconclusive:** no compatible OpenXR runtime or headset, no Vulkan device, or missing runtime depth support. Headless NRI wrapper tests can separately cover ownership, but cannot establish OpenXR session correctness.

## Limits and next evidence needed

- No actual GPU or headset run was performed.
- NRI's `CreateTextureVK` implementation/destructor path and the exact release synchronization requirements still require a separate targeted audit.
- The sample's depth-count handle mismatch is verified in pinned source; whether a particular runtime tolerates the resulting equality assumption is unknown.
- No UE4 binary inference or native PC shader execution is claimed.
- This architecture is reusable across Déraciné, Summer Lesson and Happy Manager **only if** their engine-level RHI operations can be recovered independently; game runtime and shader translation remain separate blockers.
