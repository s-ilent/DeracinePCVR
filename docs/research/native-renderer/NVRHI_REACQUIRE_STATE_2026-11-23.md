# Native PCVR — NVRHI texture state on external OpenXR reacquisition

**Date:** 2026-11-23. **Scope:** one bounded, independent source-level audit of NVRHI's *state tracker*, following the earlier non-owning Vulkan-image audit. No shadPS4 integration or GNM/PM4 work.

## Provenance and isolation

Independently queried GitHub before editing: `main` was `e2f981c40cba7c8490177c20c179cb3608cc1442`; preceding native research branch `research/native-nvrhi-import-ownership-20261123` was `e0e0001623b4aecc1cde0a0f6d25253d3fe5ada6`. This report lives on a new branch `research/native-nvrhi-state-reacquire-20261123`, forked from that exact prior commit. No shared branch or executable was edited.

Upstream source pinned to NVIDIA-RTX/NVRHI commit **`bf0bbd9b666a7297492beb1999a4ddc0dfded143`**. The findings below are direct code observations, not a runtime test.

## Question

Does NVRHI's `keepInitialState` / `initialState` automatically synchronize the state of a non-owning OpenXR `VkImage` wrapper at each successful swapchain-image wait, including first use and nonsequential reacquisition? Or must the native host seed NVRHI's per-command-list state explicitly?

## Exact-source evidence

1. [`src/common/state-tracking.cpp:67–89`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L67-L89): `beginTrackingTextureState` **overwrites the local tracker state** for the selected whole texture or subresources. It does not itself emit a barrier. This is a candidate host-side handoff API.
2. [`src/common/state-tracking.cpp:135–143`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L135-L143) and [`:410–427`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L410-L427): with `keepInitialState=true`, a *new* tracker starts in **`Common` before `stateInitialized`**, and only later starts in `initialState`. Without `keepInitialState`, it starts in `Unknown`. Merely setting `initialState` does not make the first recorded use start there.
3. [`src/common/state-tracking.cpp:163–210`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L163-L210): `requireTextureState` compares tracked before/after states and queues a barrier when they differ. Unknown state triggers an error message, not an external-state query.
4. [`src/common/state-tracking.cpp:350–360`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L350-L360) and [`src/vulkan/vulkan-commandlist.cpp:82–89`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-commandlist.cpp#L82-L89): command-list close requests transitions back to `initialState` for tracked, non-permanent textures with `keepInitialState=true`, then commits barriers.
5. [`src/common/state-tracking.cpp:363–402`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L363-L402) and [`src/vulkan/vulkan-commandlist.cpp:138–143`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-commandlist.cpp#L138-L143): on submission, NVRHI sets `stateInitialized=true` for textures it tracked with `keepInitialState`, and clears the command-list tracking maps. The state flag is attached to the texture wrapper, not an OpenXR acquire/wait event.
6. [`src/common/state-tracking.cpp:163–169`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L163-L169) and [`:363–378`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/common/state-tracking.cpp#L363-L378): permanent texture state bypasses ordinary transitions and persists after submission; changing it later reports an error. Do **not** use a permanent state for externally reacquired OpenXR images.
7. [`src/vulkan/vulkan-texture.cpp:805–823`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-texture.cpp#L805-L823): the non-owning imported-image wrapper path is separate from this tracking mechanism. Ownership support does not imply acquire/release awareness.

## New finding: first-use and reacquisition are different state problems

The first use of a wrapper with `keepInitialState=true` begins from NVRHI's `Common`, **not** the configured `initialState`. Subsequent command lists use `initialState` only because submission toggled `stateInitialized`. Neither event is linked to `xrWaitSwapchainImage`.

This establishes that `initialState` is a command-list lifetime convenience, **not a general external-image synchronization protocol**. The earlier NVRHI ownership report identified a possible stale-state risk; this source audit pinpoints the precise initialization/close/submit mechanisms that cause it.

### Minimal state-model table (source-derived, not executed)

| Situation | Default tracker entry | Required host action |
| --- | --- | --- |
| Imported wrapper, first command list, `keepInitialState=true` | `Common` | Seed actual OpenXR post-wait state explicitly or prove `Common` mapping safe |
| Same wrapper after first submission, `keepInitialState=true` | `initialState` | Ensure prior close restored it; still serialize OpenXR/queue use |
| Imported wrapper, `keepInitialState=false` | `Unknown` | Call `beginTrackingTextureState` for the acquired image before use |
| Permanent texture state | Fixed, bypasses transition | Avoid for OpenXR swapchain images |
| Shuffled OpenXR image index | Per-wrapper state, not per-acquisition callback | Select exact wrapper by returned index and seed on each wait |

## Candidate native-host contract (hypothesis to test)

For the *initial* host adapter, use non-owning per-image wrappers with `keepInitialState=false`. After **successful** `xrWaitSwapchainImage`, call `beginTrackingTextureState(wrapper, AllSubresources, AttachmentCompatibleState)` on the command list for exactly that acquired image. Record any intermediate transitions and explicitly return to the OpenXR-required attachment-compatible layout before release. Never call this handoff on a timeout. Track queue submission, synchronization and teardown outside NVRHI.

This proposal assumes NVRHI's resource-state-to-Vulkan-layout mapping matches the specific OpenXR color/depth attachment requirement. **That mapping and the actual emitted barriers were not audited here**, so the proposal is not yet verified.

An alternative `keepInitialState=true` path may work if the first-use state is explicitly seeded and every close/submit/restore is verified; it is not justified as the default yet.

## Boundaries and blockers

No native OpenXR session, Vulkan validation layer, GPU command submission, shader translation, title rendering or UE4 RHI lifecycle was tested. No source or binary was modified outside this research report. The original runtime's non-rendering systems remain a separate problem. The code evidence establishes tracking behavior, **not** that the proposed Vulkan layouts are valid or that external synchronization is automatic.

## Next falsifiable experiment

Build a title-independent harness with a fake image-acquire sequence `[2,0,2,1]` and a real Vulkan device. For each successfully waited image, invoke NVRHI's `beginTrackingTextureState` with the host's expected attachment state; transition to a transfer or shader-read state and back, close/submit, and inspect the actual `VkImageMemoryBarrier` old/new layouts. Run two variants: `keepInitialState=false` with explicit seeding, and `keepInitialState=true` without seeding. Include a first-use case, wait timeout (must record no commands), and a per-image reacquire. Compare emitted transitions against the OpenXR Vulkan contract and validation output.

**Pass:** explicit-seed variant uses correct old/new layouts on first use and reacquisition, restores the required release layout, does not destroy external images, and produces no validation errors. **Fail:** a wrong first-use old layout, a stale reacquire state, a barrier recorded after timeout, or external ownership violation. Do not infer performance or Déraciné compatibility.
