# Native renderer — NVRHI explicit state-seeding ordering and subresource scope (2026-11-23)

## Scope and evidence

One bounded follow-up to `NVRHI_OPENXR_LAYOUT_MAPPING_2026-11-23.md`: determine whether NVRHI's `beginTrackingTextureState` is itself a Vulkan layout transition, when it must be called, and how array layers are treated. This is **not** a repeat of the Vulkan layout mapping, ownership, or OpenXR device bootstrap audits.

- Exact upstream revision: NVIDIA-RTX/NVRHI `bf0bbd9b666a7297492beb1999a4ddc0dfded143`.
- Research branch: `research/native-nvrhi-seeding-order-20261123`, based on `09c28685ede38b4ecec031a08692a861c4bcc766`.
- Repository main read independently before branch creation; no change to main.
- Method: read exact pinned upstream source. No local build, runtime, validation layers, headset or game executable access.

## Finding 1 — seeding changes bookkeeping; it emits no barrier

[Common tracker `beginTrackingTextureState`, lines 77–104](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L77-L104) assigns the tracked state directly, or overwrites selected per-mip/per-array-slice entries. It does **not** enqueue a `TextureBarrier`. The [Vulkan forwarding method, lines 303–309](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-state-tracking.cpp#L303-L309) only forwards to that tracker.

Consequently `beginTrackingTextureState` is a declaration of **an already true state**, not a command that makes the image enter that state. Seeding after an image has been used, or while a pending barrier still assumes an older state, can create contradictory bookkeeping. The host must seed immediately after a successful OpenXR wait, **before any `requireTextureState`, `setTextureState`, framebuffer binding or other image use in that command list**.

This also means calling `beginTrackingTextureState` after an OpenXR wait timeout is invalid host logic: the runtime has not made that image available.

## Finding 2 — seeding can be subresource-specific, but incomplete seeds remain hazardous

[Common tracker, lines 77–104](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L77-L104) resolves the requested `TextureSubresourceSet` and either sets the whole-image state or expands `mipLevels * arraySize` entries. [`requireTextureState`, lines 169–270](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L169-L270) emits transitions based on these tracked entries and reports unknown prior states.

For an OpenXR multiview image, an all-subresource seed is the simpler initial contract **only if all represented layers and mips have the same guaranteed attachment-compatible state**. Otherwise seed the exact layer/mip ranges before use; never silently assume layer 1 inherits layer 0. A mixed-state image needs per-subresource tracking. This is a requirement on our adapter, not evidence that an OpenXR runtime returns mixed layouts.

## Finding 3 — command-list close and submission are separate state boundaries

The [Vulkan command list `close`, lines 89–111](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-commandlist.cpp#L89-L111) calls `keepTextureInitialStates` and commits barriers before ending the Vulkan command buffer. [Common tracker, lines 350–366](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L350-L366) only enforces the configured initial state if `keepInitialState` is enabled. The [`executed` method, lines 145–165](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-commandlist.cpp#L145-L165) invokes `commandListSubmitted`, which clears per-command-list state and marks tracked textures initialized under its rules ([common tracker, lines 368–411](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L368-L411)).

Thus `keepInitialState=false` avoids an implicit close-time return transition, **but requires the host to explicitly restore the OpenXR-required attachment layout before `close()`**. A new command list must be seeded again after the next successful wait; the previous command list's tracking is not a substitute for runtime acquisition state.

## Proposed host invariant and test

State machine, keyed by (swapchain, image index, aspect and relevant subresource):

1. `xrAcquireSwapchainImage` yields index; do not record rendering yet.
2. `xrWaitSwapchainImage` succeeds; only now enter **Waited**.
3. `commandList.open()`; `beginTrackingTextureState(image, range, attachmentState)` **before any image use**.
4. Record intermediate transitions/draws as necessary; explicitly `setTextureState(..., attachmentState)` for every used range and `commitBarriers()` (or rely on close to commit pending barriers) before submission.
5. `close()`, submit with host-managed queue serialization; release following OpenXR's synchronization and image-layout contract.
6. On reacquisition, repeat from step 1 with the newly returned index. Never carry command-list tracking across acquisition epochs.

Negative cases: (a) wait timeout must not open/seed/submit an image; (b) seed only layer 0 then use layer 1, expecting unknown-state diagnostics; (c) use image before seeding, expecting unknown-state diagnostics; (d) omit return transition and detect non-attachment release layout in the host ledger; (e) attempt to seed after a pending transition, which the host should reject before calling NVRHI.

**Falsifiable next experiment:** implement a small title-independent, mock-OpenXR acquisition ledger plus an NVRHI Vulkan command-list probe, with image order `[2,0,2,1]` and a timeout between acquisitions. Instrument emitted `VkImageMemoryBarrier2` and errors. Pass if every successful wait seeds exactly its returned image before use, all used layers end in the required layout, and timeout produces no GPU work; fail if an unseeded layer is silently used, `UNDEFINED` appears in a seeded preservation path, or an image is released with incorrect tracked state. Then validate against a real OpenXR runtime.

## Limits and isolation

All findings are exact-source observations and host-design deductions, **not** Vulkan runtime results. This report neither modifies nor integrates shadPS4, nor uses its alias workaround as a native requirement. No PM4/GNM work, performance claims, binary changes, copyrighted assets, or game-runtime conclusions. The non-rendering runtime remains a separate architectural problem.
