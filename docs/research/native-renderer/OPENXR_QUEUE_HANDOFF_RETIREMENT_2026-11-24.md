# Native OpenXR/Vulkan queue handoff versus GPU retirement
Date: 2026-11-24
Scope: host-only specification audit; no game executable, shadPS4 graphics integration, headset, or GPU test.
Branch: `research/native-openxr-queue-handoff-20261124`
Prior work: `VULKAN_TIMELINE_SIGNAL_SCOPE_2026-11-24.md` (GPU completion ticket scope). This report addresses a *different* boundary: handing an XR image to its runtime.

## Evidence (public, versioned specification)
- OpenXR 1.1 specification (current retrieved 2026-11-24): https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html ; section `XR_KHR_vulkan_enable2`, **Concurrency**, **Swapchain Interactions / Swapchain Image Layout**. The binding names a Vulkan queue family and queue index. The runtime can access that queue during `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage`, and `xrReleaseSwapchainImage`; the application must externally synchronize access.
- Same specification, `xrWaitSwapchainImage` / `xrReleaseSwapchainImage`: successful wait is required before writing and release; release applies to the oldest acquired image and follows command submission. `xrEndFrame` uses the most recently released image.
- Same specification, Vulkan swapchain layout contract: at release, color images must be compatible with `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` (depth with `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`), and image ownership must be on the session binding queue. The runtime explicitly anticipates command buffers submitted on that queue that **have not yet completed** when the image is released.
- Independent explanatory discussion (not normative): https://community.khronos.org/t/do-you-have-to-do-cpu-sync-before-xrendframe/110117 (2023-10-04), distinguishing submission-before-release from CPU waiting for GPU completion.

## New result
**XR release eligibility and resource retirement are different events.** The host may release an XR image after submitting its final writes on the session-bound queue, without waiting on the CPU for the GPU to complete; OpenXR's Vulkan contract explicitly accommodates outstanding GPU work. By contrast, host-owned resources referenced by that submission must remain retained until actual GPU completion, per the earlier timeline-retirement investigation.

This rules out a tempting but incorrect host architecture: blocking on the resource-retirement timeline before every `xrReleaseSwapchainImage`. It also rules out using `xrReleaseSwapchainImage` success as a GPU completion observation.

### Proposed boundary contract
1. Host creates an OpenXR Vulkan session and records its exact `VkQueue`, family and index. NRI or any other abstraction must use the *same* bound queue for final swapchain-image submissions.
2. Acquire, then successfully wait before touching the returned image. A timeout is not success; retry waiting on the same image, without rendering/releasing.
3. Render/resolve/conversion may occur on private queues, but all producer dependencies must be resolved through Vulkan synchronization before final image use on the XR-bound queue. The final queue must own the image at release.
4. Transition the XR image back to the specified compatible attachment layout before final submission ends. Do not assume `PRESENT_SRC_KHR` or general layout is acceptable.
5. Externally serialize host Vulkan queue access and the four OpenXR functions that may access that queue, using one queue-access lock or an equivalent proven ordering protocol. A mutex protects CPU submission order, **not** GPU resource lifetime.
6. Submit final commands and retirement signal with whole-submission scope; only after successful queue submission, call `xrReleaseSwapchainImage`. Never equate release with timeline completion. `xrEndFrame` consumes the released image.
7. Retain host-owned resources until the GPU completion domain advances. OpenXR-owned images must not be destroyed by the host; their wrappers and image views must not outlive the swapchain/session lifecycle.

### Failure and uncertainty
- Failed `vkQueueSubmit*`: do not release as if image rendering succeeded. Define a safe abandoned-frame/error path and validate against actual runtime behavior.
- A release failure or session loss is not a GPU completion event; preserve outstanding resource references until completion/confirmed idle.
- The precise layout and queue-family transitions required when using a private transfer/compute queue depend on queue family and usage; this report does not claim NRI performs those transitions automatically.
- This is a normative OpenXR contract for a host adapter, **not evidence of the shipped UE4 PS4 binary's command sequencing**.
- No pinned NRI source was re-audited in this run; compatibility of NRI's queue submission ownership with this contract remains unverified.

## Falsifiable next experiment
Implement a minimal native OpenXR/Vulkan application with validation enabled. Use the runtime-selected Vulkan device and binding queue; acquire/wait an XR image, submit a color clear and compatible final layout on that queue, then immediately release/end-frame *without CPU waiting for GPU completion*. Signal a separate `ALL_COMMANDS` timeline for host resource retirement and log its counter before and after release. Run a second variant with a private producer queue and explicit semaphore dependency to the XR queue. Instrument external queue synchronization and assert that no XR function listed above overlaps host queue operations. Passing criteria: no validation errors, correct frame colors, and retirement only after timeline completion; a release succeeding while the retirement counter is still pending is permitted and must not trigger early destruction. Requires a Vulkan-capable OpenXR runtime and GPU; not executed here.

## Separation of concerns
No shadPS4 backend, GNM/PM4, cache, VR presenter or binary modification is involved. UE4-level RHI recovery and non-rendering runtime remain independent workstreams.
