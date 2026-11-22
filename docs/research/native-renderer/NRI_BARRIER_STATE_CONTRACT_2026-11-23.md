# Native PCVR — NRI barrier and imported-image state contract

**Date:** 2026-11-23. **Scope:** one bounded source-level investigation; no GPU execution. **Branch:** `research/native-nri-barrier-state-contract-20261123`. Base: `research/native-nri-multiview-contract-20261123` (prior report commit `15867ee6`). GitHub comparison independently showed `main=e2f981c40cba7c8490177c20c179cb3608cc1442`, base branch three commits ahead and zero behind. No changes to main, executable, emulator, or GPU translation.

## New question

When NRI wraps an OpenXR-owned Vulkan image, does it automatically know its current layout, access scope, or queue-family ownership, or does the host have to supply transitions? What exactly does the pinned Vulkan implementation emit? This is distinct from the earlier external-handle ownership and multiview audits.

## Pinned source observations

NRI `06ff6f643777e72eda88bc6fb8d56267b85d872f`:

1. [Include/NRIDescs.h:656-702](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L656-L702): `TextureBarrierDesc` explicitly carries `before` and `after` access/layout/stages, mip/layer ranges and optional source/destination queues. The API contract explicitly restricts barriers inside a render pass to input-attachment transitions. `GlobalBarrierDesc` and `BufferBarrierDesc` are separate; global barriers do not carry image layouts.
2. [Source/VK/CommandBufferVK.hpp:2640-2707](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2640-L2707): `CommandBufferVK::Barrier` directly translates caller-provided scopes to `VkMemoryBarrier2`, `VkBufferMemoryBarrier2`, and `VkImageMemoryBarrier2`. For images it assigns `oldLayout=GetImageLayout(in.before.layout)`, `newLayout=GetImageLayout(in.after.layout)`, and the caller's mip/layer/plane ranges. **No prior image-layout lookup or inferred transition appears in this implementation.** This supports an explicit host-maintained state ledger, not a claim that NRI tracks external image layouts automatically.
3. [Source/VK/CommandBufferVK.hpp:2709-2777](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2709-L2777): If the wrapped device is recorded as lacking `synchronization2`, NRI lowers the same barriers to `vkCmdPipelineBarrier`, unions the stage masks and substitutes TOP/BOTTOM when a scope is empty. Otherwise it emits `vkCmdPipelineBarrier2`. The host must query actual device features and cannot assume the sync2 path just because Vulkan 1.3 headers exist.
4. [Source/VK/CommandBufferVK.hpp:5-9](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L5-L9): `GetQueueFamilyIndex` maps the explicit `EXTERNAL_QUEUE` sentinel to `VK_QUEUE_FAMILY_EXTERNAL`; absent queues and local concurrent-sharing cases become `VK_QUEUE_FAMILY_IGNORED`. [Include/NRIDescs.h:678-691](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L678-L691) specifies that external transfers use `EXTERNAL_QUEUE` plus the local queue. **This does not prove OpenXR swapchain acquisition/release requires such a queue-family transfer**; that must be established from the OpenXR Vulkan interop specification and runtime image contract, not inferred from NRI's available mechanism.
5. [Source/VK/CommandBufferVK.hpp:2709-2722](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2709-L2722): the legacy render-pass path separately tracks input-attachment ranges; this is not general automatic texture-state tracking.

## Architectural consequence: imported swapchain images need a host state ledger

The native OpenXR host should maintain a per-**swapchain image index**, per-subresource state record separate from NRI's non-owning texture wrapper. At minimum record whether the image has been acquired and successfully waited, known layout/access/stage state (or **unknown**, never silently assumed), local queue family when relevant, outstanding GPU submissions, and whether it has been released. An image's index is not necessarily acquisition order. NRI's `Barrier` consumes explicit transitions but does not validate their provenance.

Suggested host contract (design proposal, **not implemented**):

```text
OpenXR acquire(index) -> wait(XR_SUCCESS) -> host verifies known entry state
 -> host emits explicit NRI TextureBarrierDesc for target subresources
 -> NRI records native Vulkan rendering (mono or multiview)
 -> host emits required final image layout/access transition
 -> host ensures GPU use is complete under OpenXR release contract
 -> xrReleaseSwapchainImage -> ledger marks image released
```

The **entry/final layout and queue-ownership rules are intentionally unspecified** here. They require a separate normative OpenXR/Vulkan check and runtime validation. Do not invent an initial `UNDEFINED` state for every reacquisition: that would risk discarding contents and obscuring a real layout contract. Do not interpret `EXTERNAL_QUEUE` as mandatory for OpenXR solely because NRI exposes it.

NRI's explicit barrier interface is sufficient to express per-layer/mip transitions for the proposed two-eye array image, but the source audit alone does not establish correct synchronization, OpenXR release semantics, queue ownership, or presentation behavior. This ledger belongs in the host graphics/OpenXR layer, **not** in a shadPS4 texture cache or GNM resource alias workaround.

## Falsifiable next experiment

Create a **game-asset-free**, isolated Vulkan/OpenXR host harness with validation layers. Use the runtime-selected Vulkan device and query enabled synchronization2, swapchain image count/array layers and queue family. Acquire/wait a swapchain image, record explicit NRI barriers around a simple render, submit, synchronize and release according to the OpenXR Vulkan contract. Log `index, oldLayout, newLayout, src/dst queue family, stages, completion token` per image. Repeat with permuted indices and stereo arrays. As a negative control deliberately submit an invalid known-layout transition and confirm validation catches it; do not perform this negative control against a production headset session. Compare sync2 and legacy paths where supported.

**Pass criterion:** zero validation errors for the valid path, correct per-image bookkeeping across reacquisition, and no premature release/destruction. **Fail criterion:** invalid layout/ownership or incomplete GPU work at release. No runtime or device test was performed in this run.

## Boundaries and confidence

**Observed in pinned code:** explicit barrier conversion and sync2/legacy fallback; queue-family sentinel mapping; render-pass restriction. **Inferred architectural recommendation:** host-owned image state ledger. **Unresolved:** normative OpenXR entry/final layout, queue ownership and runtime behavior; exact NRI feature probing under the selected OpenXR Vulkan device; whether any game RHI path can supply this state. No native UE4 draw, shader conversion, FPS benefit or runtime interoperability is claimed. Non-rendering UE4 runtime remains a separate problem.
