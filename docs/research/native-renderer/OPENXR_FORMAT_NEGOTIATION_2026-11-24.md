# Native OpenXR swapchain format negotiation and NRI wrapper metadata

**Date:** 2026-11-24  
**Scope:** independent native renderer foundation; source/specification audit, no executable or GPU test.  
**Base:** new branch from repository `main`; original foundation is `research/native-rhi-foundation-20261123` at `313f277c7ac5a19a5de50cdd460c6a8568103225`.

## Question

Can a native OpenXR host select its runtime-owned swapchain format independently of UE4's internal render-target format, then wrap the actual Vulkan image in NRI without inventing unsupported format, usage, or sample-count metadata?

This is distinct from previous investigations of image destruction, queue ownership, release synchronization and multiview layer mapping.

## Pinned evidence

1. **OpenXR 1.1.65 specification**, [swapchain image management](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#swapchain-image-management): `xrEnumerateSwapchainFormats` returns graphics-API-specific formats in runtime preference order; `xrCreateSwapchain` requires an enumerated format. Format lists remain identical during a session. An sRGB Vulkan format signals nonlinear encoding to the compositor; otherwise the compositor treats submitted values as linear. The requested `usageFlags` control which underlying graphics operations the runtime-owned images support. This is normative API behavior, not a device test.
2. **NVIDIA NRI**, commit [`06ff6f643777e72eda88bc6fb8d56267b85d872f`](https://github.com/NVIDIA-RTX/NRI/tree/06ff6f643777e72eda88bc6fb8d56267b85d872f), [`Include/Extensions/NRIWrapperVK.h` lines 73–84](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L73-L84): `TextureVKDesc` requires the *actual* `vkFormat`, `vkImageType`, `vkImageUsageFlags`, dimensions, mip count, layer count and sample count, as well as the external `VkImage`. [Line 123](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h#L123) exposes `CreateTextureVK`.
3. **NRI**, same commit, [`Include/NRIDescs.h` lines 772–789](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L772-L789): the library's native texture descriptor separately represents usage, format, dimensions, layers, samples and sharing mode.

## Finding: format negotiation is an explicit architectural boundary

An OpenXR swapchain is not a general-purpose UE4 render target. Its `VkFormat` is selected from the *runtime's* list, not from the original PS4 framebuffer format. NRI's external wrapper accepts the resulting Vulkan format and metadata, but does not negotiate the format on the application's behalf.

Therefore the native renderer must preserve two different format contracts:

- **Internal scene surface:** semantic UE4 format, encoding, samples, depth/stencil and usage, mapped to a suitable host-native format; may be HDR and multisampled.
- **OpenXR submission surface:** runtime-enumerated `VkFormat`, supported usage flags, view array configuration and sample count; may require a final resolve, tone-map or color-space conversion.

Conflating them would risk treating linear UNORM data as sRGB, requesting storage access from a runtime image that lacks it, or assuming the game target's MSAA/depth representation can be presented directly.

### Host-side negotiation proposal (design, not implemented)

1. After native OpenXR session creation, enumerate the complete ordered `VkFormat` list. Preserve the list and runtime preference order in diagnostics.
2. Intersect with a *tested* NRI/Vulkan output-format allowlist and required Vulkan format features. Select the first compatible runtime-preferred format, subject to an explicit output color-space policy. Do **not** assume sRGB is universally preferable: the final shader must encode appropriately for the chosen format.
3. Request only necessary `XrSwapchainUsageFlags` (at minimum color attachment for direct graphics output). If compute/storage or transfer destination is desired, check support rather than assuming it; use a separate intermediate target if necessary.
4. Create the OpenXR swapchain and enumerate its `VkImage` handles. For each image, construct `TextureVKDesc` with the **exact** selected format, actual usage, width, height, layer count, sample count and Vulkan image type. Never substitute the original UE4 render-target format.
5. Retain the native OpenXR host's metadata record as the source of truth for every wrapped image. Keep color and depth swapchain records independent.
6. Use an explicit final-pass conversion/resolve if the internal scene format and XR output format differ. Do not silently introduce a host copy without documenting its cost and correctness.

### Color-encoding decision table

| Internal scene representation | XR format | Required final behavior |
|---|---|---|
| Linear HDR | `*_SRGB` | Tone-map into linear display-referred values; the sRGB attachment encoding/compositor interpretation must be consistent |
| Linear HDR | `*_UNORM` | Tone-map and explicitly encode for the intended output convention; never assume automatic sRGB handling |
| Linear HDR | `*_SFLOAT` | Establish runtime support and intended output range/color-space contract; do not assume HDR display delivery |
| MSAA scene | Single-sample XR | Resolve before final submission, with correct ordering relative to tone mapping |

This table defines questions the host must answer; it does **not** establish a universal shader transfer function or HDR support.

## Focused verification performed

Read the pinned NRI wrapper descriptor and core texture descriptor through GitHub's repository API, confirming the fields above. Cross-checked the normative OpenXR format-enumeration, usage and sRGB language against the published 1.1.65 specification. Verified that the repository foundation explicitly separates native OpenXR from the existing emulator path.

**Not verified:** Vulkan runtime format lists, NRI's complete internal `VkFormat` mapping table, concrete format-feature support, OpenXR swapchain usage acceptance, colorimetric correctness on a headset, or GPU execution. No claim is made that a particular runtime accepts any specific HDR, storage, or MSAA format.

## Next falsifiable experiment

Build a small standalone Vulkan/OpenXR/NRI host (no game binaries or shadPS4 dependencies):

1. Log actual runtime format preference list and device format-feature flags.
2. Select one supported sRGB and, if offered, one linear UNORM or float format. Create color-attachment swapchains and wrap their images with exact `TextureVKDesc` metadata.
3. Render a calibrated linear grayscale ramp and saturated patches through an explicit final-pass shader; read back *only where usage permits* or use headset/runtime capture with known limitations.
4. Test an intentionally unsupported usage combination and verify that it is rejected rather than silently accepted.
5. Test internal HDR/MSAA to single-sample XR output through an explicit resolve/convert path, under Vulkan validation.

**Pass:** no Vulkan validation errors, exact metadata correspondence, visibly/quantitatively distinct encoding paths consistent with the selected format, and clean failure for unsupported formats/usages. **Fail:** incorrect gamma, mismatched format/usage metadata, invalid NRI views, or a hidden format conversion. **Inconclusive:** runtime lacks the comparison formats, capture cannot distinguish transfer functions, or no headset/runtime is available.

## Boundary

No changes to shadPS4 graphics, PM4/GNM, VR presentation, or game executable. This is a reusable native host contract for Déraciné and subsequent UE4 PSVR titles. Non-rendering runtime reconstruction remains separate.
