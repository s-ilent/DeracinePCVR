# Native renderer — Plume Vulkan/OpenXR ownership boundary (2026-11-23)

## Scope and provenance

One bounded, source-only investigation: can unmodified `renderbag/plume` adopt the Vulkan instance, device, queue and externally owned images selected/created by an OpenXR runtime? This is **not** a general RHI performance comparison or a repeat of NRI/NVRHI image-state studies.

- Upstream exact commit: [renderbag/plume `230c502dfe9c2e02a1ad4321b3e1ada46c45f876`](https://github.com/renderbag/plume/commit/230c502dfe9c2e02a1ad4321b3e1ada46c45f876).
- Source inspected: `plume_vulkan.cpp`, `plume_render_interface.h`; repository main independently observed at `e2f981c40cba7c8490177c20c179cb3608cc1442`. Research base is prior isolated commit `c16561052ef6c49693849be64427a08abdc717d7`.
- Method: exact-revision source inspection; no compilation, runtime, GPU validation, headset or game binary access.

## Finding A — public Vulkan device creation is internally owned

[`VulkanInterface::createDevice`, lines 4661–4664](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4661-L4664) constructs a `VulkanDevice` with a preferred **device name**, not externally supplied handles. [`VulkanDevice` constructor, lines 3776–3831](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L3776-L3831) enumerates physical devices and scores/selects one itself; [lines 4049–4099](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4049-L4099) chooses queue families and calls `vkCreateDevice` directly. [`VulkanInterface`, lines 4626–4632](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4626-L4632) calls `vkCreateInstance` directly.

This does **not** satisfy the OpenXR Vulkan-enable2 bootstrap model in which the host must use the XR-selected physical device and coordinate device/queue creation with the runtime. Choosing a GPU by name is not equivalent to adopting the exact runtime graphics identity.

## Finding B — destruction assumes native ownership

[`VulkanDevice::release`, lines 4472–4484](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4472-L4484) unconditionally destroys its VMA allocator and then `vkDestroyDevice` when handles are non-null. [`VulkanInterface::~VulkanInterface`, lines 4655–4659](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L4655-L4659) destroys its instance. No non-owning adoption mode is visible on the inspected public construction path.

**Consequence:** simply injecting an XR-created `VkDevice` into an ordinary Plume device object would risk destroying a host-owned device on teardown. A correct adaptation needs explicit ownership flags, a separate constructor, or a different backend implementation; changing destruction alone is insufficient because queue, allocator, features and extension setup are coupled to construction.

## Finding C — imported VkImage wrapper exists internally, but is not a complete public OpenXR import API

[`VulkanTexture(VulkanDevice*, VkImage)`, lines 1026–1032](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L1026-L1032) stores an external image without setting `ownership=true`. [Destructor, lines 1034–1042](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L1034-L1042) only calls `vmaDestroyImage` if ownership is true. This is positive evidence of a **non-owning internal image wrapper**.

However, the imported-image constructor only stores the device and image. Unlike the allocated-image constructor [lines 972–1024](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L972-L1024), it does not populate `desc`, `imageFormat`, `imageSubresourceRange`, or create an image view. [`createTextureView`, lines 1064–1065 and 1082–1090](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L1064-L1065) depends on those fields. The inspected public resource interface exposes `createTexture` through device/pool creation, not an external-`VkImage` import function; [pool path, lines 3735–3741](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp#L3735-L3741) allocates new images. Existing internal swapchain use is not evidence that arbitrary OpenXR images can be imported with correct metadata through the public API.

## Assessment

| Constraint | Unmodified Plume at pinned revision | Evidence strength |
|---|---|---|
| OpenXR-selected physical device adoption | No public adoption path found; constructor selects device | Source-observed construction path |
| Adopt XR-created VkDevice and queue | No public adoption path found; calls vkCreateDevice itself | Source-observed construction path |
| Preserve externally owned VkDevice on teardown | No; unconditional destroy for non-null handle | Direct source |
| Non-owning VkImage wrapper | Internal constructor exists | Direct source |
| Public XR image import with format/layer metadata | Not established; inspected constructor lacks metadata setup | Source-observed limitation |
| Explicit OpenXR queue serialization / image layout protocol | Not evaluated in this pass | Open |

**Decision:** Keep NRI as the provisional foundation and NVRHI as a serious alternative. Plume should not be selected for the first XR-host integration without a separate ownership/adoption patch. This is an engineering-fit conclusion, not a statement about Plume's general rendering quality or performance.

## Next falsifiable experiment

Create an **out-of-tree minimal Plume adoption patch** against this exact revision, without changing the game or shadPS4:

1. Accept an externally supplied `VkInstance`, `VkPhysicalDevice`, `VkDevice`, graphics queue and family index; assert exact identity against the XR host.
2. Add independent ownership flags for instance/device; ensure teardown destroys only Plume-owned allocator, views, command pools and other child objects.
3. Expose a non-owning `VkImage` import API taking explicit format, extent, layers, mips, samples and usage; verify image-view creation and layer selection.
4. Use a fake-host Vulkan fixture or validation-layer test: destroy the Plume wrapper, then successfully use and destroy the original host device/image. Reject mismatch/invalid metadata.
5. Only after those pass, attempt actual OpenXR acquire/wait/render/release. No per-frame queue idle assumption.

**Pass criteria:** imported image never destroyed by Plume; host device/instance survive Plume teardown; no invalid view/subresource creation; XR graphics identity preserved. **Fail criteria:** any implicit device recreation, destruction of host-owned handles, or inability to initialize image metadata.

## Boundaries and blockers

No GPU execution, build, validation-layer output or headset test was performed. The source audit does not prove an adapter cannot be written, nor does it establish every internal usage path. This report does not touch UE4 game/runtime systems, shadPS4 graphics or PM4/GNM; those remain independent architectural problems.
