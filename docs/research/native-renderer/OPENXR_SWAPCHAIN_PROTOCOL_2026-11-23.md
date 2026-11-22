# OpenXR swapchain protocol as a native renderer boundary

**Date:** 2026-11-23. **Scope:** independent source audit of OpenXR swapchain image state; no game binary, shadPS4 backend or renderer changed. **Status:** normative source observations, not a runtime test.

## Question

What exact host-side state machine must a native UE4 RHI adapter preserve when rendering to runtime-owned OpenXR Vulkan images, independently of the choice of NRI or direct Vulkan?

## Source evidence

Khronos [OpenXR rendering chapter](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc), inspected 2026-11-23 (moving main branch; pin to a commit before implementation):

1. `xrEnumerateSwapchainImages` reports runtime-owned image objects constant for the swapchain lifetime. Image count is implementation-defined; acquire returns an **index**, with undefined order. Therefore no round-robin image-index assumption is valid.
2. The application **must acquire before building graphics commands referring to an image**, and **must successfully wait before writing**. `xrWaitSwapchainImage` implicitly waits for the oldest acquired image not yet successfully waited on.
3. **Subtle correctness trap:** `XR_TIMEOUT_EXPIRED` is reported as a non-error (even `XR_SUCCEEDED(XR_TIMEOUT_EXPIRED)` is true), but does **not** make the image writable. The next wait retries the same index. A generic `XR_SUCCEEDED` guard is insufficient; require `result == XR_SUCCESS` for the transition to writable.
4. Once an image has been successfully waited on, it must be released before waiting on the next acquired image. Release implicitly operates on the oldest acquired image; it is not indexed. Release requires a successful wait.
5. Before `xrDestroySwapchain`, all submitted graphics commands referring to its images **must complete execution**. An NRI wrapper's non-ownership of a `VkImage` does not discharge that lifetime/synchronization obligation.
6. Runtime reports supported formats through `xrEnumerateSwapchainFormats`; swapchain format, array size, sample count and usage flags are explicit creation parameters. An image array is possible but not guaranteed to be supported in the desired configuration. Do not bake stereo array layers into the UE4 adapter.

These requirements are **OpenXR host contract**, not inferred UE4/PS4 resource behaviour.

## Minimal state machine

Per swapchain track a FIFO of acquired indices, with per-image state:

`AVAILABLE -> ACQUIRED -> WAITED -> RELEASED/AVAILABLE`

- `acquire`: append returned index to acquired FIFO; only index returned by OpenXR is eligible.
- `wait`: target the oldest acquired, not-yet-successfully-waited index. On `XR_TIMEOUT_EXPIRED`, retain ACQUIRED and retry (or skip rendering); **do not** record image-writing commands.
- `wait == XR_SUCCESS`: transition to WAITED. Permit native image barriers and draws only now; ensure all work referring to that image is submitted before release.
- `release`: release the oldest acquired index that has been waited successfully, respecting the runtime's ordering rules; image becomes unavailable to app writes. GPU completion and appropriate graphics API synchronization remain a separate obligation.
- `destroy`: ensure GPU work referencing any image is complete before destroying the swapchain and NRI image views/wrappers.

**Implementation note:** this is a proposed *application-side bookkeeping model*, not a full formal restatement of OpenXR's multi-acquire state machine. In particular, model multiple acquired images, wait/release FIFO and frame association separately. Avoid equating an NRI resource state/layout transition with an OpenXR acquire/wait/release operation: these are orthogonal.

## Portable interface consequence

Expose an opaque `ExternalFrameTarget` containing image index, native image identity, format, dimensions, layer count and current acquire/wait eligibility. Only the OpenXR host layer calls `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`. The renderer backend receives a **waited** target and returns submitted-work completion information. Neither the UE4 semantic tape nor a library-specific renderer owns the `XrSwapchain` or Vulkan image.

Do **not** use `XR_SUCCEEDED` to gate drawing; no fixed image count, fixed acquire order, assumed side-by-side eye packing, implicit copies or WSI presentation.

## Next falsifiable experiment

Implement a pure host-only, asset-free fake OpenXR swapchain with 3 images and deliberately permuted acquire order (e.g. 2,0,1). Inject one `XR_TIMEOUT_EXPIRED` followed by `XR_SUCCESS`; assert zero draws before the latter. Exercise multiple outstanding acquires and reject out-of-order wait/release attempts. Record GPU completion token and assert teardown blocks until all referenced submissions retire. Then repeat on a real OpenXR Vulkan runtime with validation; test array and separate-eye swapchains.

**Limitations:** No runtime, compiler or headset validation here. Vulkan-specific final image layout and synchronization at release require separate exact-spec/source audit and runtime validation. No measured CPU/GPU costs or claims of game compatibility.
