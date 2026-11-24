# NRI image view wrapper lifetime — 2026-11-25

Foundation HEAD 6b359f520b9f41f854b2aac41bb74526f6c60e87; main e2f981c40cba7c8490177c20c179cb3608cc1442, independently retrieved. Public requests failed (restricted URL for foundation, HTTP 503 for NRI); authenticated retrieval succeeded.

## Observation
Pinned NRI 06ff6f643777e72eda88bc6fb8d56267b85d872f, [DescriptorVK.hpp](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorVK.hpp): lines 103–104 remove cached framebuffers and destroy the image view; line 165 stores the texture wrapper address; lines 259 onward reconstruct image-view creation info by dereferencing that wrapper and recovering native image and description. Thus native image retention alone does not preserve reconstruction validity. No caller reachability or runtime defect established.

## Proposed contract
Views retain the exact texture wrapper generation, not merely logical resource ID or VkImage. Retire GPU references before destroying views, then wrappers, then host-owned native images. Runtime-owned OpenXR images are never destroyed by the host; release and swapchain teardown follow OpenXR requirements. This is independent of shadPS4.

## Next falsifiable experiment
Identify callers of FillImageViewCreateInfo; use instrumented dispatch and ASan to exercise view reconstruction after logical resource replacement while retaining the old generation. Verify old and new native image handles independently, and teardown ordering. No GPU or sanitizer tests executed this run. No game assets or executable modifications.
