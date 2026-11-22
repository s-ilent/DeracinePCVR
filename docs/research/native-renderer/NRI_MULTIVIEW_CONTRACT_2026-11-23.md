# Native PCVR — NRI Vulkan multiview pipeline contract

**Date:** 2026-11-23. **Scope:** bounded source audit, no GPU execution. **Branch:** `research/native-nri-multiview-contract-20261123`; base `798329748eaced31d5c69557dec7483aa8b5d487`. Independently checked main at `e2f981c40cba7c8490177c20c179cb3608cc1442`. No game executable, emulator, renderer, or private assets modified.

## Question

Does NRI's Vulkan backend preserve the view-mask contract needed to render distinct eyes into a two-layer OpenXR swapchain image? What is the valid fallback for separate eye images? This is *not* a repeat of external-image/device ownership, already covered in `NRI_EXTERNAL_IMAGE_OWNERSHIP_2026-11-23.md`.

## Pinned exact-version evidence

NRI commit `06ff6f643777e72eda88bc6fb8d56267b85d872f`:

1. [`Include/NRIDescs.h:1455-1463`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1455-L1463): `Multiview::LAYER_BASED` is the Vulkan-style mode; its documentation requires the rendering `viewMask` to match pipeline creation. Flexible and viewport-based modes are different contracts.
2. [`Include/NRIDescs.h:1599-1610`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1599-L1610): pipeline `OutputMergerDesc` carries both `viewMask` and `multiview`. [`RenderingDesc:1744-1753`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1744-L1753) carries a separate render-time `viewMask`. [Feature fields:2294-2299](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L2294-L2299) expose layer-based support; [view maximum:2233-2238](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L2233-L2238) exposes `viewMaxNum`. Two-layer texture support alone is insufficient.
3. [`Source/VK/PipelineVK.hpp:316-331`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/PipelineVK.hpp#L316-L331) forwards the pipeline mask to `VkPipelineRenderingCreateInfo.viewMask` or fallback `RenderPassDesc.viewMask`.
4. [`Source/VK/CommandBufferVK.hpp:1732-1745`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1732-L1745) forwards render-time mask to `VkRenderingInfo.viewMask`; [1790-1801](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1790-L1801) forwards it to the legacy render-pass path. [1926-1937](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1926-L1937) sets fallback framebuffer `layerNum = 1` for a nonzero mask: **this does not imply one eye**, because multiview selects layers by view index.
5. [`Source/VK/DescriptorVK.hpp:109-151`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorVK.hpp#L109-L151) constructs `VkImageView` using explicit `layerOffset` and `layerNum`; [18-35](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorVK.hpp#L18-L35) selects `VK_IMAGE_VIEW_TYPE_2D_ARRAY` for a 2D texture view exposing multiple layers.

These are source observations, not an OpenXR compatibility or headset test.

## Proposed host adapter invariants

**Array-layer stereo:** only when the OpenXR runtime actually provides a suitable image with two layers and Vulkan reports compatible multiview support, wrap the runtime-owned image non-owningly, create an attachment view spanning layers 0 and 1, build an NRI graphics pipeline with `multiview=LAYER_BASED` and `OutputMergerDesc.viewMask=0b11`, and begin rendering with `RenderingDesc.viewMask=0b11`. A host-native shader must use per-view indexing and distinct per-eye matrices; two-layer output does not arise automatically.

**Separate-image fallback:** use two independent mono draws with `viewMask=0`, each to its own acquired/waited image. Never pretend two unrelated `VkImage` handles form one layered view.

**Pipeline cache:** our adapter must include view mask and multiview mode in its pipeline identity, alongside shader, format, samples and depth/blend state. NRI's own cache internals were not audited here. The host still owns OpenXR acquire/wait/release, synchronization and teardown; NRI's mask does not handle them.

## Limits and next falsifiable experiment

No runtime, Vulkan validation, NRI build, shader view-index behavior, MSAA resolve, depth-array validation, or Déraciné shader was exercised. No UE4 resource hook, PC shader conversion or native game frame is claimed. shadPS4 remains read-only reference only; its GPU alias workaround is irrelevant to this contract.

Build a standalone game-asset-free OpenXR/Vulkan/NRI harness. Query actual `XR_KHR_vulkan_enable2` device configuration, OpenXR image array size, Vulkan multiview support and attachment formats. With a two-layer image, compile a simple SPIR-V shader using per-view index, draw with pipeline/render masks `0b11`, verify different left/right output and zero validation errors. Deliberately mismatch masks as a **negative validation test**. Test independent mono swapchains with `viewMask=0`. Check destruction order: GPU idle/retired, NRI views and wrappers destroyed, then OpenXR swapchain. Record unsupported configurations as unsupported, not success.

**Status:** evidence-backed contract only. No FPS claims; no rendering runtime executed.
