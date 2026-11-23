# NRI dimension narrowing at the native OpenXR target boundary

**Date:** 2026-11-24  
**Track:** Native PCVR Renderer Foundation; independent of Binary Atlas, shadPS4 graphics, and live-hook work  
**Source pin:** NVIDIA-RTX/NRI `06ff6f643777e72eda88bc6fb8d56267b85d872f`  
**Status:** Exact-revision header audit plus standalone C++ conversion test; no Vulkan/OpenXR execution

## Question

Can the host safely copy OpenXR/Vulkan image dimensions and projection sub-image rectangles into NRI's Vulkan image wrapper and render rectangles without validating integer ranges?

**Answer: no.** The public NRI API narrows image extents and rectangle coordinates relative to Vulkan/OpenXR. The native host must validate before conversion, including when importing runtime-owned images. This is a host API contract, **not** evidence that current headsets exceed NRI's limits.

## Exact source evidence

1. [`Include/NRIDescs.h` lines 53–55](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L53-L55) defines `Nri(Dim_t)` as `uint16_t`. Its maximum representable dimension is **65,535**.
2. [`Include/NRIDescs.h` lines 143–148](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L143-L148) defines `Rect.x/y` as **signed 16-bit** and `Rect.width/height` as `Dim_t`. Consequently positive offsets above **32,767** are not representable, independently of width/height.
3. [`Include/NRIDescs.h` lines 772–786](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L772-L786) uses `Dim_t` for created texture width, height, depth, mip count, and layer count.
4. [`Include/Extensions/NRIWrapperVK.h` lines 61–72](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L61-L72) uses the same `Dim_t` for the metadata supplied to `CreateTextureVK` for an **existing** `VkImage`. Importing a native image does not bypass this representational limit.
5. [`Include/NRIDescs.h` lines 132–140](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L132-L140) uses float coordinates/dimensions for `Viewport`; viewport representability does **not** make `Rect` or texture metadata safe.

The OpenXR projection sub-image uses `XrRect2Di` (32-bit signed offset and extent), and Vulkan `VkExtent3D` uses 32-bit unsigned dimensions. The source type mismatch alone establishes the need for checked conversion; it does not establish a runtime failure.

## Focused local test

A standalone C++17 test using the exact public field types was compiled with `-Wall -Wextra -Wconversion`. Explicit casts of `uint32_t width=65536` and `uint32_t x=32768` produced `Rect.width=0` and `Rect.x=-32768` on this Linux host. The executable exited successfully after checking those results.

This verifies that an **unchecked conversion can silently corrupt** target dimensions/offsets on the test platform. It does **not** exercise NRI validation, Vulkan, a headset, or OpenXR.

## Native host contract (proposal)

At the OpenXR-to-NRI boundary, retain a wide canonical description (`uint32_t` dimensions and `int32_t` offsets), and only construct NRI descriptors after all checks:

- Width/height/depth, mip count, layer count: positive where required, and <= `UINT16_MAX`; preserve zero only for documented optional fields.
- NRI render rectangles: x/y in `[INT16_MIN,INT16_MAX]`; width/height in `[0,UINT16_MAX]`, plus application-specific nonzero rules.
- Check **offset + extent** using a widened integer type before comparing with the actual image extent; do not permit signed overflow.
- Validate eye array slice, sub-image offsets and bounds independently from the acquired OpenXR swapchain image index.
- On an unrepresentable OpenXR image, **fail the NRI path explicitly** or use a separately validated direct-Vulkan fallback. Never truncate or silently change the runtime-selected image.
- Do not conflate the limits of an imported external swapchain with UE4 internal render-target formats or shadPS4's resource handling.

A 32,768-pixel positive sub-image x-offset can fail the `Rect` limit **even though** a 32,768-pixel texture width itself fits `Dim_t`. Both checks are needed.

## Evidence limits

- No evidence was collected that Déraciné, Summer Lesson, Happy Manager or an available OpenXR runtime uses dimensions/offsets near these thresholds.
- This audit covers the pinned public NRI API type contract, not every internal validation path.
- The conversion test is a C++ type behavior test, not a Vulkan/NRI runtime validation test.
- No game binaries, GPU backend, OpenXR presenter, or repository runtime code were modified.

## Next falsifiable experiment

Add a **headless checked-descriptor adapter** in an isolated host-only harness. Feed a boundary matrix (widths 65535/65536, offsets 32767/32768, negative offsets, two-eye array slices, and `offset + extent` overflow cases). Assert that all out-of-range inputs are rejected *before* `CreateTextureVK` or command recording, and that valid descriptors retain exact values. Then repeat against actual `xrEnumerateSwapchainImages` dimensions when a runtime is available. A failing acceptance case is any silent narrowing or out-of-bounds eye rectangle.

**Architectural implication:** NRI remains viable for ordinary stereo targets, but the host needs an explicit wide-to-narrow validation seam; a successful OpenXR swapchain allocation does not imply its metadata is representable by NRI.
