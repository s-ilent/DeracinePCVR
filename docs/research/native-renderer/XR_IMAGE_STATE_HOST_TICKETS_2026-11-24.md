# OpenXR image lifecycle versus native-host completion tickets
Date: 2026-11-24

## Scope and provenance
Independent native PCVR renderer research. Branch created from `research/native-xr-queue-arbiter-harness-20261124`; `main` untouched. Prior queue-arbiter test established CPU entry serialization, not OpenXR image lifecycle correctness. No PS4 binary, shadPS4 graphics backend, GPU translation, or VR presenter changes.

Primary normative reference: [OpenXR 1.1 swapchain image lifecycle and Vulkan graphics binding](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html), including `xrAcquireSwapchainImage`, `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`. This run did not retrieve an exact pinned OpenXR-Docs commit; do not interpret the model below as a specification conformance proof.

## New bounded investigation: two independent notions of completion
The previous queue-arbitration research separated submission from GPU completion. A further boundary is required: **OpenXR's swapchain image acquisition/release lifecycle is not the host's resource-retirement timeline**.

A naive host state machine would make an image unavailable for reacquisition until its own Vulkan timeline ticket completes. That is an unjustified extra constraint: the runtime arbitrates availability and may return an image through `xrAcquireSwapchainImage` and successful `xrWaitSwapchainImage` independently of whether the host has already polled its own retirement counter. The host must not invent a second availability authority.

Conversely, `xrReleaseSwapchainImage` is not a host retirement signal. Submitted work may remain in flight; command buffers, descriptor allocations, temporary upload resources and other host-owned objects must remain alive until their actual GPU dependencies finish.

### Proposed separation
1. **XR image state:** acquired index; successful wait; final image commands submitted; release; subsequent acquisition and wait controlled by XR runtime. Failed waits and failed Vulkan submissions cannot be treated as successful transitions.
2. **Host submission state:** independent per-queue completion ticket, associated with resources used by submitted commands. A successful XR release does not advance the host completion ticket.
3. **Reacquisition:** runtime-returned index can be acquired again, but host must still honor the prior GPU submission's lifetime and synchronization obligations. Do not clear outstanding resource references merely because the index was reused.
4. **Failure recovery:** the model does not prescribe how to recover from a failed GPU submission after XR wait. Real integration must establish a legal frame/swapchain recovery path rather than silently release uninitialized contents.

## Executable model and observed result
A standalone C++17 state-machine model was compiled and run locally with AddressSanitizer and UndefinedBehaviorSanitizer on 2026-11-24. It exercised invalid wait-before-acquire, double acquisition, failed wait, submit-before-wait, failed submission, release-before-submit, missing ticket, premature host retirement, two image indices, and reacquisition without conflating it with ticket retirement.

Observed: `PASS: 11 lifecycle checks, host tickets independent of OpenXR reacquisition`.

**Limitations:** the test is a deliberately simplified host contract; it does not invoke OpenXR, Vulkan or NRI and does not establish whether a runtime can return a particular image before a given Vulkan submission completes. The local test source was not committed in this report-only revision. The test does not model all legal OpenXR acquisition/wait concurrency or session-loss recovery.

## Next falsifiable experiment
On a real native OpenXR Vulkan session, record the exact sequence of returned image indices, successful waits, queue submissions, releases, and independently observed ALL_COMMANDS timeline completion. Keep host retirement tickets per submission rather than per reusable image slot. Verify that (a) a successful release does not prematurely retire resources, (b) any reacquired index is accepted only after XR wait succeeds, (c) a failed submit never creates a completion ticket, and (d) validation layers report no layout/queue misuse. Compare actual NRI-wrapped Vulkan queue handles with the XR graphics binding.

This work does not solve guest UE4 RHI entry-point recovery, shader translation, or non-rendering game runtime reconstruction.
