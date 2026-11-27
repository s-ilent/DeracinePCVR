# NRI timeline fence ownership — 2026-11-27

Scope: native host RHI only, independent of emulator graphics. Foundation HEAD verified 2fc67bdaa514178a34b02b172cc9ad8930c2be02; main e2f981c40cba7c8490177c20c179cb3608cc1442.

## Evidence
Pinned NRI 06ff6f643777e72eda88bc6fb8d56267b85d872f, [FenceVK.hpp](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/FenceVK.hpp#L1-L73), full 73-line implementation inspected. Destructor and both Create overloads (first portion of file) distinguish owned semaphores from borrowed timeline handles. Ordinary fences create timeline semaphores; the special swapchain sentinel selects binary instead. The wrapper overload stores the supplied timeline handle and disables native ownership without querying its type. The final GetFenceValue and Wait methods call Vulkan timeline counter and wait operations; the latter returns failure on unsuccessful wait. These methods do not implement OpenXR image acquisition or release.

## Proposed contract
Keep XR image lease state separate from GPU completion tokens. Host-created timeline semaphores may be borrowed by NRI; host must ensure type, device, lifetime and completion ordering. Destroy borrowed wrapper before host semaphore; do not treat wrapper destruction as a wait. Do not pass a binary semaphore to timeline counter/wait operations. Do not use NRI's WSI sentinel as an XR image lease. Explicitly negotiate timeline support even in conservative wrapping mode.

## Falsifiable next experiment
Create a Vulkan timeline semaphore at 0; wrap it, submit a signal to 1, wait via NRI and verify counter >=1. Destroy wrapper, query native counter again and destroy native semaphore once. Negative cases: reject binary handles and wrong-device handles in host metadata before wrapping. Separately exercise XR acquire/wait/render/release with completion tracking; verify the applicable XR synchronization contract before choosing a release policy. Require zero validation errors. No GPU or XR tests executed in this source-only run.

Public foundation retrieval failed: Failed to fetch restricted URL. Authenticated commit retrieval succeeded for both branches. No game binaries, emulator changes or performance claims.
