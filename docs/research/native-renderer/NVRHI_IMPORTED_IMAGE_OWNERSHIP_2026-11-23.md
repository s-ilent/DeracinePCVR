# Native PCVR — NVRHI adopted Vulkan device and imported OpenXR image boundary

**Date:** 2026-11-23. **Scope:** one independent source-level investigation of **NVRHI as an alternative** to the already audited NRI imported-image ownership path. This is not a new NRI finding, a PS4 GPU translation task, or an emulator integration.

**Repository provenance:** Main was independently checked at `e2f981c40cba7c8490177c20c179cb3608cc1442` via recent commits. The immediately preceding native research commit `ee3ccefe79d1f9ae4bfff07e873e3c757ae52cf2` was verified and used as the base for isolated branch `research/native-nvrhi-import-ownership-20261123`. No existing branch or main was modified.

## Question

Can NVRHI operate on Vulkan objects created by a native OpenXR host, wrap runtime-owned swapchain images without taking ownership, and safely express their states? Does its higher-level state-tracking model introduce a distinct contract from NRI's explicit barriers?

## Pinned source observations

Audited **NVIDIA-RTX/NVRHI** at pinned revision `bf0bbd9b666a7` (the foundation comparison's pin). These are **source observations**, not compiled/runtime validation.

1. [`include/nvrhi/vulkan.h:50–88`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/include/nvrhi/vulkan.h#L50-L88): `vulkan::DeviceDesc` accepts an existing `VkInstance`, `VkPhysicalDevice`, `VkDevice`, graphics queue and queue-family index, plus optional compute/transfer queues. The API is designed to adopt the host's Vulkan handles, not create a separate OpenXR-incompatible device.
2. [`src/vulkan/vulkan-device.cpp:36–64`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/src/vulkan/vulkan-device.cpp#L36-L64): `createDevice` constructs an NVRHI `Device` around supplied handles and queue objects. No native instance/device creation appears in this constructor path.
3. [`src/vulkan/vulkan-device.cpp:274–293`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/src/vulkan/vulkan-device.cpp#L274-L293): `Device::~Device` destroys NVRHI-created query pool, pipeline cache and descriptor-set layout, but not the supplied `VkDevice` or `VkInstance`. The native OpenXR host remains responsible for device lifetime; NVRHI-owned child objects must be released first.
4. [`src/vulkan/vulkan-texture.cpp:805–823`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/src/vulkan/vulkan-texture.cpp#L805-L823): `Device::createHandleForNativeTexture` accepts `ObjectTypes::VK_Image`, creates an NVRHI texture wrapper and sets `managed = false`.
5. [`src/vulkan/vulkan-texture.cpp:779–803`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/src/vulkan/vulkan-texture.cpp#L779-L803): texture destruction always destroys wrapper-created image views, but only destroys `VkImage` and frees memory when `managed` is true. An OpenXR image wrapped by the above function is **not** destroyed by NVRHI.
6. [`include/nvrhi/nvrhi.h:525–556`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/include/nvrhi/nvrhi.h#L525-L556): texture descriptions expose `initialState`, `keepInitialState`, and `enableAutomaticStateTracking`. This is a meaningful difference from NRI's explicitly caller-specified before/after barrier contract.
7. [`include/nvrhi/nvrhi.h:3734–3808`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7/include/nvrhi/nvrhi.h#L3734-L3808): command lists expose `setEnableAutomaticBarriers`, `setTextureState`, `setPermanentTextureState`, and `commitBarriers`. This provides a way to express explicit transitions; it **does not by itself prove** correct OpenXR reacquisition semantics or cross-queue state ownership.

## New architectural distinction

**NVRHI is a viable source-level alternative for device adoption and image ownership**, not just an engine that can create its own Vulkan resources. This narrows the NRI-vs-NVRHI comparison: both expose non-owning Vulkan-image wrappers and accept a host-owned device. The choice must now turn on command/state synchronization semantics, multiview, descriptor model, and ease of preserving recovered UE4 RHI meaning, rather than on assumed incompatibility with OpenXR-owned images.

The important unresolved risk is **state-tracking authority**. OpenXR's successful wait/release contract supplies attachment-compatible image states, while NVRHI's `initialState` and `keepInitialState` describe the library's logical tracking. Those values must be synchronized at each acquire/release boundary; a stale per-command-list or permanent state must not override the host's external image lifecycle. Merely calling `enableAutomaticStateTracking` does not establish correctness.

**Proposed host contract (not implemented):**
- The native OpenXR host owns `VkInstance`, `VkDevice`, graphics queue, swapchain and all `VkImage` handles.
- NVRHI wrappers are created after image enumeration, with exact format, array-layer, sample and usage metadata; wrapper-created views and GPU work are retired before host destroys swapchain/device.
- On successful wait, the host hands the image to the renderer in its OpenXR-specified attachment-compatible state; command recording may transition to intermediate states, but must restore the release-compatible state.
- The host serializes OpenXR queue-using calls and Vulkan submissions as required by the OpenXR contract. No unconditional `vkQueueWaitIdle` is assumed.
- NVRHI's automatic barriers must be disabled or proven correct for external reacquisition; avoid setting permanent texture state until that behavior is understood.

## Comparison to existing NRI result

| Requirement | NRI (prior research) | NVRHI (this audit) |
| --- | --- | --- |
| Adopt OpenXR-owned Vulkan device | Source-supported | Source-supported |
| Wrap OpenXR `VkImage` without destroying it | Source-supported | Source-supported |
| Own/destroy wrapper-created image views | Source-supported | Source-supported |
| Barrier authority | Explicit before/after states | Automatic tracking available, explicit state commands also exposed |
| Correct OpenXR reacquisition behavior | Not runtime-tested | Not runtime-tested |
| Multiview and full UE4 semantic mapping | Separate earlier NRI audit | **Not investigated here** |

**No selection reversal is justified.** NRI remains the provisional preference because its explicit state contract has already been mapped in detail, not because NVRHI cannot adopt OpenXR resources.

## Evidence limits

No OpenXR runtime, GPU, headset, NVRHI compilation, Vulkan validation, or synthetic draw was executed. This audit does not prove that all imported image formats/usage flags work, that automatic state tracking handles acquire/release correctly, or that NVRHI is faster or easier to integrate. The original UE4 4.18 shader/resource lifetime and non-rendering runtime remain unresolved. No shadPS4 source or graphics backend was changed.

## Next falsifiable experiment

Build an asset-free native Vulkan/OpenXR harness that imports the **same acquired swapchain image** through NVRHI and NRI in separate test modes (never simultaneous owners of the same recording), using the exact same Vulkan device and queue. Exercise acquire → successful wait → clear/render → restore attachment-compatible layout → release → reacquire, including shuffled image indices. For NVRHI, compare automatic barriers with explicit `setTextureState` + `commitBarriers`; instrument the actual emitted `VkImageMemoryBarrier` layout transitions and validate against OpenXR requirements. Destroy wrappers before swapchain teardown and assert no `vkDestroyImage` or `vkDestroyDevice` occurs from either library.

**Pass:** valid image lifecycle, no stale-layout transition, correct ownership, zero validation errors. **Fail:** incorrect transition, image/device destruction by wrapper, use-before-wait, or unsafe GPU/queue ordering. Do not extrapolate FPS or title compatibility.

This experiment is independent of shadPS4 GPU translation, texture cache, VR presenter and resource-alias workaround.
