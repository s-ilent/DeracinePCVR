# OpenXR Vulkan swapchain release: submission, layouts and queue synchronization

Date: 2026-11-23
Scope: independent Native PCVR Renderer Foundation; public-source/specification audit only.
Source pin: KhronosGroup/OpenXR-Docs `18ebc0374da23c020ef4c949f9b144ffae6160ef` (OpenXR 1.1.65, 2026-10-16).
Target: native OpenXR Vulkan session using `XR_KHR_vulkan_enable2`; NRI wraps external images but does not own their lifetime.
No game binary, runtime instrumentation, headset, Vulkan execution or shadPS4 changes.

## New finding: release is a submission boundary, not a GPU-idle boundary

The exact Vulkan graphics binding specification explicitly says that, at `xrReleaseSwapchainImage`, the runtime interprets the image as *being referenced by submitted command buffers which have not yet completed execution*. It also requires a compatible color/depth attachment layout and ownership by the queue supplied in `XrGraphicsBindingVulkan2KHR`. Therefore waiting for all GPU work to finish **before every release** is not mandated by this contract; conversely, merely recording commands without submitting them before release is insufficient.

Evidence: [khr_vulkan_enable2.adoc L461–493](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/extensions/khr/khr_vulkan_enable2.adoc#L461-L493).
The core rendering chapter states release occurs after the application is done **submitting** commands that reference the image, and requires a successful wait before release: [rendering.adoc L519–525](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/rendering.adoc#L519-L525).

### Implications for the host adapter

1. `xrAcquireSwapchainImage` returns an index, **not permission to write**; `xrWaitSwapchainImage` must succeed first. `XR_TIMEOUT_EXPIRED` is a non-error result but **not** a successful image wait; retry waiting on the same image. Evidence: [rendering.adoc L423–427](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/rendering.adoc#L423-L427), [L469–486](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/rendering.adoc#L469-L486).
2. On successful wait, start from the **contractual attachment-compatible layout and graphics-queue ownership**. NRI's internal cached resource-state history must be reconciled with that external handoff, rather than blindly reusing the last state from a previous acquisition.
3. If rendering uses transfer, storage, or shader-read layouts, insert transitions back to compatible `COLOR_ATTACHMENT_OPTIMAL` or `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` before release; ensure the bound graphics queue owns the image. Do not assume `GENERAL` or `PRESENT_SRC_KHR` is the OpenXR release state.
4. Submit Vulkan work referencing the image on the bound queue before `xrReleaseSwapchainImage`. The runtime may synchronize against **outstanding submitted** work; do not require a per-frame `vkQueueWaitIdle` as a general rule.
5. `xrDestroySwapchain` is different: **all submitted commands referring to it must have completed** before destruction. Evidence: [rendering.adoc L281–303](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/rendering.adoc#L281-L303).
6. Vulkan queue access must be externally synchronized against OpenXR calls `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage`, and `xrReleaseSwapchainImage` (the runtime *may* access the bound queue in these functions). `xrWaitSwapchainImage` is **not** on that list. Evidence: [khr_vulkan_enable2.adoc L400–420](https://github.com/KhronosGroup/OpenXR-Docs/blob/18ebc0374da23c020ef4c949f9b144ffae6160ef/specification/sources/chapters/extensions/khr/khr_vulkan_enable2.adoc#L400-L420).

## Proposed host-only state machine (design, not implemented)

Per swapchain, track acquisition FIFO and successful-wait FIFO; per image, track `Available → Acquired → Waited → Submitted → Released`. `WaitTimeout` preserves the acquired state and cannot advance to rendering. A release must be rejected unless a successful wait and the final layout/ownership/submit obligations have been satisfied. Distinguish CPU bookkeeping for `Released` from GPU completion; hold image-dependent host resources until a completion primitive confirms no GPU use. Color and depth swapchains are independently acquired and tracked.

For initial implementation, use one bound graphics queue and serialize all OpenXR calls that may access it with Vulkan submissions and other host queue operations. A queue mutex is a conservative host design, not an OpenXR-mandated specific locking primitive. No queue-family transfer is needed when all relevant work stays on that queue.

## Negative findings and limits

- The spec **does not** require blocking on a GPU fence before each `xrReleaseSwapchainImage`; doing so may be a useful debugging mode, but is not the normative synchronization contract.
- This pass does not verify NRI's actual barrier emission for re-acquired wrapped textures. NRI's external-image state tracking must be tested separately.
- The layout guarantee is stated for `XR_KHR_vulkan_enable2`; do not silently generalize it to another graphics binding without checking its own normative text.
- The runtime may still use an image after `xrDestroySwapchain`; the application must not destroy runtime-owned `VkImage` objects.

## Falsifiable next experiment

Build a small host-only OpenXR Vulkan harness with validation enabled and a logging wrapper around the Vulkan graphics queue. For a runtime-supported color swapchain, test:
- acquire → wait(timeout) → retry wait(success) → render/submit → release;
- transition to `TRANSFER_DST_OPTIMAL` and back to `COLOR_ATTACHMENT_OPTIMAL` before submit/release;
- submit then release *without* `vkQueueWaitIdle`, and wait for a completion fence only before destruction;
- deliberately omit final layout restoration in a validation-only negative test (do not submit this invalid path to a production compositor);
- concurrently attempt queue submission and `xrReleaseSwapchainImage` under an instrumented queue lock, verifying serialization.

Capture exact runtime/driver/validation versions, barrier tuples, submission order, return codes, and teardown diagnostics. A positive result demonstrates a functioning contract on that runtime, not universal performance or portability. No game binaries needed.

## Repository impact

One standalone source-backed specification report. No runtime code or hooks; no changes to shadPS4 or main. This closes the previously open **release-synchronization contract** question at the specification level, while leaving implementation/runtime validation open.
