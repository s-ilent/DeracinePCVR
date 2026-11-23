# Native renderer — NVRHI Vulkan layout mapping for OpenXR reacquisition (2026-11-23)

## Scope and provenance

One bounded exact-source audit of **the missing NVRHI resource-state → Vulkan image-layout mapping**, following `NVRHI_REACQUIRE_STATE_2026-11-23.md`. This is not a new ownership or first-use tracker audit. No game, emulator, renderer, binary or GPU run was modified.

- Upstream: NVIDIA-RTX/NVRHI **`bf0bbd9b666a7297492beb1999a4ddc0dfded143`**.
- Native research base: prior OpenXR bootstrap report commit `60c72f4b`; isolated branch `research/native-nvrhi-openxr-layout-mapping-20261123`.
- Independently queried repository main before writing: `e2f981c40cba7c8490177c20c179cb3608cc1442`.
- Method: exact pinned source read, no Vulkan execution or validation layers.

## Verified mapping

The [pinned `vulkan-constants.cpp` lines 212–281](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-constants.cpp#L212-L281) defines:

| NVRHI ResourceStates | Vulkan image layout | Stage / access |
| --- | --- | --- |
| `Common` | `eUndefined` | top-of-pipe / none |
| `RenderTarget` | `eColorAttachmentOptimal` | color-attachment-output / color read-write |
| `DepthWrite` | `eDepthStencilAttachmentOptimal` | early/late fragment tests / depth-stencil read-write |
| `DepthRead` | `eDepthStencilReadOnlyOptimal` | early/late fragment tests / depth-stencil read |
| `PixelShaderResource` | `eShaderReadOnlyOptimal` | fragment shader / shader read |
| `CopySource` | `eTransferSrcOptimal` | transfer / transfer read |
| `CopyDest` | `eTransferDstOptimal` | transfer / transfer write |
| `Present` | `ePresentSrcKHR` | all-commands / memory read |

The [conversion implementation, lines 320–365](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-constants.cpp#L320-L365) combines state bits and asserts against incompatible image layouts; it special-cases depth read plus shader sampling.

The [Vulkan barrier emitter, `vulkan-state-tracking.cpp` lines 214–260](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-state-tracking.cpp#L214-L260), calls `convertResourceState` for both before and after, emits `vk::ImageMemoryBarrier2` with the returned layouts, derives depth/stencil/color aspect and layer/mip ranges from texture descriptors, and sets **both queue-family indices to `VK_QUEUE_FAMILY_IGNORED`**.

## New finding: `Common` is not a harmless OpenXR acquire state

NVRHI maps `Common` to **`VK_IMAGE_LAYOUT_UNDEFINED`**, not to an attachment-compatible layout. The preceding report proved that a newly tracked imported texture with `keepInitialState=true` can start in `Common` on first use. Thus a first transition from `Common` to `RenderTarget` can emit `UNDEFINED → COLOR_ATTACHMENT_OPTIMAL`, discarding previous image contents. This is *not* a safe generic substitute for OpenXR's post-wait layout contract. The precise consequences depend on whether contents must be preserved and on the normative OpenXR layout rules.

In contrast, the earlier proposed explicit-seeding path now has a **source-supported mapping**:

- After a successful wait on a color image, seed `RenderTarget`, corresponding to `COLOR_ATTACHMENT_OPTIMAL`.
- For a depth/stencil image, seed `DepthWrite`, corresponding to `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`.
- If rendering uses an intermediate sampled or transfer layout, require an explicit transition back to the appropriate attachment state before release.
- Do **not** substitute `Present`: it maps to `PRESENT_SRC_KHR`, which is a window-system presentation layout, not the OpenXR attachment-compatible release layout.
- The emitted barriers use ignored queue-family indices. That is consistent with same-queue use, **not** proof of cross-queue or runtime synchronization. Queue ownership and OpenXR API/queue external synchronization remain the host's responsibility.

This resolves the mapping uncertainty from the NVRHI reacquisition report at the source level, but does **not** verify actual runtime layout on wait, preservation behavior, or barrier validity.

## Design consequence

Use an OpenXR-owned per-image ledger and non-owning NVRHI wrappers, `keepInitialState=false`, and `beginTrackingTextureState` on the command list **only after a successful wait**, selecting the exact returned swapchain image index. Never initialize from `Common` or `Present` by convenience. Track color/depth layouts separately. OpenXR runtime contract remains authoritative over the entry/exit state and queue serialization.

A renderer-agnostic host contract should express `AcquiredAndWaited(index, aspect, expectedLayout)`, `BeginCommands`, `RestoreReleaseLayout`, `Submit`, `Release` rather than exporting NVRHI-specific state enums. That keeps NRI and direct Vulkan possible.

## Limits and falsifiable next experiment

No GPU execution, headset, OpenXR session, Vulkan validation, UE4 RHI interception or native Déraciné draw was performed. This is a source-level result; no FPS inference.

Run a standalone Vulkan test with external non-owning images and shuffled indices `[2,0,2,1]`. Compare `keepInitialState=true` default first-use against `keepInitialState=false` with explicit `beginTrackingTextureState`; instrument `VkImageMemoryBarrier2` old/new layouts, ensure a wait timeout emits no commands, and test preservation of a preinitialized image pattern. Then repeat against a real OpenXR runtime. **Pass:** seeded variant emits attachment→intermediate→attachment transitions for the correct image and preserves contents where required, without validation errors. **Fail:** `UNDEFINED` unexpectedly appears in the seeded path, the wrong index is transitioned, release layout is wrong, or queue synchronization is invalid.

This does not modify or depend on shadPS4 graphics internals. Non-rendering game/runtime systems remain an independent architectural problem.
