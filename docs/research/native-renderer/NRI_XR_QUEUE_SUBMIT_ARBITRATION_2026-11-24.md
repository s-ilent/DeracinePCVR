# NRI Vulkan queue submission and OpenXR external queue arbitration
Date: 2026-11-24
Scope: exact-source audit of NVIDIA-RTX/NRI at commit `06ff6f64`, host architecture only. No game binary, headset, Vulkan execution, or shadPS4 backend changes.

## Question
Can the native OpenXR host submit its final swapchain writes through NRI without implicit CPU completion waits, and does NRI's queue lock also serialize OpenXR runtime access to the same VkQueue?

## Pinned evidence
1. [NRI QueueVK.hpp lines 82–106](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/QueueVK.hpp#L82-L106): `QueueVK::Submit` takes `ExclusiveScope lock(*m_Lock)`, builds Vulkan wait/command/signal arrays, and maps each `FenceSubmitDesc::stages` through `GetPipelineStageFlags`, masked by unsupported stages.
2. [QueueVK.hpp lines 108–170](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/QueueVK.hpp#L108-L170): if synchronization2 is unavailable on a wrapped device, NRI uses `vkQueueSubmit` with `VkTimelineSemaphoreSubmitInfo`; otherwise `vkQueueSubmit2` with `VkSubmitInfo2`. Both call the Vulkan queue under the lock, check the result, and return success without a queue-idle or fence wait.
3. [QueueVK.h lines 16–21, 30–49](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/QueueVK.h#L16-L49): `QueueVK` exposes its native `VkQueue` conversion and an internal `GetLock()`; the lock is an NRI implementation detail, not an OpenXR-visible synchronization mechanism.
4. [ConversionVK.h lines 356–362](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/ConversionVK.h#L356-L362): `StageBits::ALL` (numerically zero) maps to `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`; `NONE` maps to Vulkan NONE. This is important: zero-initialized `FenceSubmitDesc::stages` means ALL, not NONE.
5. [NRIDescs.h lines 1903–1920](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Include/NRIDescs.h#L1903-L1920): `FenceSubmitDesc` includes a fence, timeline value and stage mask; `QueueSubmitDesc` has explicit wait/signal arrays and command buffers.
6. [ImplVK.cpp lines 491–493](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f64/Source/VK/ImplVK.cpp#L491-L493): public `QueueSubmit` delegates directly to `QueueVK::Submit`.
7. [OpenXR 1.1 Vulkan binding](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html): runtime can access the session-bound queue during `xrBeginFrame`, `xrEndFrame`, `xrAcquireSwapchainImage`, and `xrReleaseSwapchainImage`; application must externally synchronize access.

## New findings
**NRI submission itself does not force GPU completion before OpenXR release.** Its Vulkan path submits commands and returns on success, so a host can sequence `QueueSubmit` then `xrReleaseSwapchainImage` without an intervening CPU wait, subject to the OpenXR layout/ownership/queue contract established in the previous report.

**NRI's internal queue lock is insufficient for OpenXR queue arbitration.** It only guards operations entering NRI and sharing that lock. The OpenXR runtime cannot be assumed to acquire it. The host must provide a separate *session-bound queue arbiter* encompassing all NRI queue operations on that VkQueue and the four OpenXR functions. No claim is made that an external mutex alone supplies Vulkan GPU dependencies.

**NRI's default signal stage is suitable for whole-submission retirement on the sync2 path.** `StageBits::ALL = 0` maps to ALL_COMMANDS. Explicitly setting ALL is preferable for auditable retirement tickets; rejecting non-ALL signal stages remains appropriate. For the legacy `vkQueueSubmit` path, signal stage masks are not forwarded (legacy signal semantics apply), but timeline semaphore capability and wrapped-device feature reporting still require independent validation.

## Proposed native-host boundary
- Bootstrap the Vulkan instance/device/queue through OpenXR, then wrap exactly that device and session-bound queue in NRI. Assert native handle, family and index match; don't create a second XR-present queue.
- Introduce a host-owned `XrQueueArbiter` whose critical section surrounds calls to NRI `QueueSubmit`/`QueueWaitIdle` (and any other host queue operations), plus the four runtime calls above. Keep the arbiter lock separate from NRI internals; never assume NRI's private lock is shared with OpenXR.
- Acquire/wait the XR image, record final barriers and writes, submit via NRI with explicit `StageBits::ALL` retirement signal, then release after submission success without waiting for GPU completion. Retire host-owned resources only after polling actual timeline completion.
- A failed submission is not proof of execution or release eligibility. Surface error and enter an explicitly tested abandoned-frame/session recovery path.

## Limitations / hypotheses
- Source inspection establishes the call path and lock scope, not runtime compatibility or absence of all driver blocking.
- This audit did not establish whether `CreateDeviceVK`/NRI wrapped queue selection returns the *exact* OpenXR queue index under all configurations. That remains a required runtime assertion.
- It did not test concurrent runtime calls or `vkQueueSubmit2` validation. The legacy path may behave differently on wrapped devices with unknown feature flags.
- No UE4.18 PS4 executable interface or non-rendering runtime behavior is inferred from this host-only result.

## Next falsifiable experiment
Implement a tiny `XrQueueArbiter` harness with a mock NRI submit callback and four mock XR callbacks. Instrument entry/exit and assert **zero overlapping critical sections**, including two competing producer threads, failure propagation, and reentrant callback detection. Then, on a Vulkan/OpenXR runtime, wrap the runtime-selected VkQueue with pinned NRI and log queue handles; submit a color clear with an ALL-stage timeline signal and call XR release before waiting for GPU completion. Acceptance: handle equality, no overlapping queue operations, valid final image layout, no validation errors, and host resource retirement strictly after observed GPU completion. A mismatch or unsupported queue wrapping falsifies this integration design.

## Separation
No shadPS4 graphics, PM4/GNM, resource alias workaround, performance claims, copyrighted binaries, or game asset changes. Native UE4 semantic RHI recovery and non-rendering runtime are separate tracks.
