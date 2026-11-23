# NRI multiview layer contract for externally wrapped OpenXR images

**Date:** 2026-11-23  
**Scope:** Independent native-renderer source audit; no shadPS4 integration or binary/runtime modification.  
**Pinned NRI:** `06ff6f643777e72eda88bc6fb8d56267b85d872f` (2026-11-19).  
**Repository baseline:** `research/native-rhi-foundation-20261123` @ `6b359f520b9f41f854b2aac41bb74526f6c60e87`; `main` observed @ `e2f981c40cba7c8490177c20c179cb3608cc1442`.

## New question

Can a native OpenXR host wrap a stereo array image through NRI and select two eyes by a Vulkan multiview mask without conflating *swapchain image index*, *array layer*, and *view index*? Earlier research established non-owning wrapper destruction, device adoption, and queue/layout constraints, but did not trace NRI's multiview render path.

## Direct source evidence

1. [`Include/Extensions/NRIWrapperVK.h`, lines 73–85](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L73-L85): `TextureVKDesc` carries an explicit `layerNum`. [`Source/VK/TextureVK.hpp`, lines 76–85](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L76-L85) copies this into the wrapped texture description. The host, not NRI, must supply the correct layer count.
2. [`Include/NRIDescs.h`, lines 990–1000](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L990-L1000): texture views separately describe their array-layer range. A wrapped 2-layer image does not automatically imply a 2-layer attachment view.
3. [`Include/NRIDescs.h`, lines 1453–1463](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1453-L1463): NRI distinguishes multiview modes, including Vulkan-style statically assigned attachment layers and a matching render/pipeline `viewMask`.
4. [`Include/NRIDescs.h`, lines 1604–1610 and 1746–1753](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1746-L1753): both pipeline output-merger and rendering descriptions have a `viewMask`; nonzero masks require supported multiview capabilities.
5. [`Source/VK/PipelineVK.hpp`, lines 321–333](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/PipelineVK.hpp#L321-L333): dynamic-rendering pipeline creation copies `om.viewMask` to `VkPipelineRenderingCreateInfo.viewMask`; the fallback render-pass path also uses it.
6. [`Source/VK/CommandBufferVK.hpp`, lines 1738–1778](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1738-L1778): dynamic rendering copies `renderingDesc.viewMask` to `VkRenderingInfo.viewMask`, obtains attachment image views, and derives `layerCount` from the view range.
7. [`Source/VK/CommandBufferVK.hpp`, lines 1795–1805 and 1929–1940](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1929-L1940): the legacy render-pass path also uses the render mask and sets framebuffer layer count to 1 for multiview.
8. [`Source/VK/CommandBufferVK.hpp`, lines 1678–1686](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L1678-L1686): clear attachment rectangles use one layer when a view mask is active. This is a distinct multiview clear path, not evidence that only the first eye is cleared.

## Interpretation and contract

The source establishes **three independent axes** that the native host must not collapse:

| Axis | Meaning | Required provenance |
|---|---|---|
| OpenXR swapchain image index | Which runtime-owned `VkImage` was acquired | `xrAcquireSwapchainImage` result |
| Vulkan image array layer | Which subresource layer the attachment view exposes | OpenXR swapchain `arraySize`, NRI wrapper `layerNum`, NRI view layer range |
| Multiview view index | Which active view is emitted by the render pipeline | Matching NRI pipeline/rendering `viewMask`, enabled Vulkan multiview |

For a **hypothetical** 2-layer array swapchain, the candidate Vulkan-style stereo configuration is: wrap each acquired image with `layerNum=2`; create a 2-layer attachment view; create pipeline and render with `viewMask=0b11`; verify that the shader's view index maps to layers 0 and 1. This is a **proposed configuration**, not a tested OpenXR capability claim. The runtime's actual swapchain format, array size, sample count, view configuration, and device feature enablement remain authoritative.

A 2-eye view mask is **not** a substitute for two distinct acquired images when the host uses separate per-eye swapchains. Conversely, two layers of one acquired array image are **not** two swapchain indices. This distinction is especially important for independent depth/color acquisitions and for per-eye pose/projection data.

NRI's dynamic-rendering code passes `layerCount` derived from the attachment view while also setting a nonzero `viewMask`. The Vulkan validation and render semantics of that exact combination should be tested rather than assumed equivalent to the legacy path's framebuffer layer count of 1.

## Focused checks performed

Read pinned NRI source files `NRIWrapperVK.h`, `NRIDescs.h`, `TextureVK.hpp`, `PipelineVK.hpp`, and `CommandBufferVK.hpp`. Checked direct dataflow of layer counts and view masks in dynamic and legacy rendering paths. This is a **source-level inspection**, not a compiled test or runtime observation. No shader binary or game asset was read or exported.

## Proposed falsifiable host-only experiment

Build a tiny Vulkan/NRI test with a 2-layer externally created image and a shader that writes a distinct color per `gl_ViewIndex` (or the appropriate cross-compiled equivalent). Test two NRI paths: dynamic rendering and legacy render pass, using identical `viewMask=3` and a 2-layer view. Verify both array layers by readback, capture validation messages, and inspect generated Vulkan rendering structures. Repeat with a single-layer view as an expected invalid/negative control. Then repeat against a real OpenXR-created array swapchain, but only if the runtime advertises and accepts the necessary usage, array size, and Vulkan feature configuration.

**Pass:** both layers receive distinct expected values, no validation errors, and the OpenXR path respects acquired image ownership and release requirements.  
**Fail:** only one layer changes, mask/pipeline mismatch, validation errors, or unexpected view-to-layer mapping.  
**Inconclusive:** Vulkan multiview unsupported/not enabled, OpenXR runtime rejects array swapchains, or test environment lacks the relevant GPU/runtime.

## Next decision

If both NRI paths pass, standardize a host-facing stereo target descriptor with separate `swapchainImageIndex`, `arrayLayerRange`, and `viewMask`. If dynamic and legacy paths diverge, isolate the Vulkan structure difference and retain a direct-Vulkan fallback for the swapchain render pass rather than forcing a shadPS4-specific workaround.

**Unresolved:** runtime-supported array swapchain layouts; actual enabled Vulkan multiview feature; precise shader translation interface; native UE4 draw-state extraction. None is claimed solved by this pass.
