# Native PCVR Renderer Foundation — Research Track 1

**Date:** 2026-11-23  
**Status:** Source audit / design experiment; no native GPU prototype, executable hook, build, or headset test has yet passed.  
**Base repository:** `s-ilent/DeracinePCVR` at `e2f981c40cba7c8490177c20c179cb3608cc1442`.  
**Branch:** `research/native-rhi-foundation-20261123`. This document is independent of the shadPS4 renderer and performance ledger.

## Goal and explicit non-goals

Goal: identify and experimentally validate a reusable **native PC rendering RHI** for the shipped UE4-derived PS4 **x86-64 game executable**. Target future titles include Déraciné, Summer Lesson, Happy Manager and other UE4 PSVR games. The intended boundary is a recovered **UE4-level resource/state/draw command stream**, interpreted by our own per-engine adapter and rendered through a host GPU library. OpenXR belongs to a separately owned *native* PCVR runtime.

**Out of scope:**
- Replacing or integrating into shadPS4's Vulkan backend, texture cache, GPU thread, or VR presenter.
- GNM/PM4 emulation, compatibility improvements, GPU command translation, or shadPS4 benchmarking.
- Assuming the existing shadPS4 OpenXR device, PSVR side-by-side frame/export protocol, or guest GPU allocations are native-renderer APIs.
- Modifying the shipped game executable, distributing game assets, uploading SELF, or adding game-specific binary payloads.
- Refactoring the existing player-facing emulator port.

shadPS4 may remain a **read-only experimental reference / execution oracle** until an independent game runtime is viable; no new renderer dependency on it is allowed.

## Evidence anchors — target-specific versus library-specific

The pinned Déraciné 1.00 SELF is SHA-256
`8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`.
Earlier A2 static research identified:

| Candidate operation | Inner-ELF VA | Evidence | Runtime status |
|---|---:|---|---|
| `RHIDrawIndexedPrimitive`-like method | `0x0cbb9c0` | FDE + `sceGnmDrawIndex` caller at `0x0cbbd55`, relocation table slot `0x03ee0190`, retained `GnmCommands.cpp` literal | High-confidence static attribution; NOT executed/intercepted |
| Direct compute dispatch | `0x0cadc00` | Same vtable-shaped object, GNM `sceGnmDispatchDirect` call | Candidate; method name not proven |
| Graphics shader/state setup | `0x0cb5130` | Same table; `sceGnmSetVsShader`, `sceGnmSetPsShader350` | Candidate; method name unresolved |
| GNM submission | `0x0cd69a0` | Separate `GnmManager.cpp`-correlated function | Below desired boundary; do **not** implement a native renderer here |

Source: `A2_internal_rhi_recovery.md` and `B2_semantic_capture.md` (uploaded atlas workstream). No runtime interceptions exist, and higher-level resource creation, updates, deletion, descriptor bindings, synchronization, shader provenance, scene/view state, and stereo rendering still require recovery.

**Critical distinction:** historic shadPS4 `R32_SFLOAT` / `D32_S8` representations at GPU VA `0x2068000000` are GPU-translation behaviour, *not* a verified UE4-native resource contract. Correctly intercepting above PS4 GNM may remove the need to recreate those format alias copies. Preserve the UE-visible depth, stencil and resource lifetime semantics; do not copy shadPS4's workaround into the design without UE evidence.

## External library source audit (2026-11-23)

All revisions are public repository snapshots, not locally built/tested versions.

| Library | Revision | What is explicitly present | Important gap |
|---|---|---|---|
| [NRI](https://github.com/NVIDIA-RTX/NRI) | `06ff6f643777e72eda88bc6fb8d56267b85d872f` | Vulkan / D3D12 / other APIs; explicit before/after access, layout and pipeline stage barriers; native Vulkan object/device wrapping; resource aliasing support; supplied `NRISamples` includes `MemoryAliasing`, `Multiview`, `Multisample`, `Wrapper` | Highest API surface / learning cost; actual UE4 adapter CPU cost unmeasured |
| [Plume](https://github.com/renderbag/plume) | `230c502dfe9c2e02a1ad4321b3e1ada46c45f876` | Lightweight RHI from RT64; Vulkan, D3D12, Metal; SPIR-V/DXIL/Metal shader ingest; draw, compute, pipelines, resources, views, command queues | Vulkan backend creates its own `VkInstance` / `VkDevice`; barriers use broad access masks and tracked whole-image layouts; no public adopted-device or externally owned swapchain-image contract found; own README says API not stable |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | `bf0bbd9b666a7297492beb1999a4ddc0dfded143` | Vulkan / D3D11 / D3D12; native Vulkan `DeviceDesc` takes existing instance/device/queues; optional automatic barrier/state tracking; deferred lifetime management; validation | Higher-level resource model may hide guest command ordering; explicit mode and overhead must be tested |
| Direct Vulkan (+ VMA) | Khronos Vulkan API, baseline | Full control over native images, barriers, memory, pipelines, queues and OpenXR interoperability | Requires building GPU infrastructure independently; measured baseline needed before declaring an abstraction beneficial |

Primary file evidence:
- NRI [`Include/NRIDescs.h`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h): `GlobalBarrierDesc`, `BufferBarrierDesc`, `TextureBarrierDesc`, `AccessLayoutStage`; `features.resourceAliasing`.
- NRI [`Include/Extensions/NRIWrapperVK.h`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/Extensions/NRIWrapperVK.h): `DeviceCreationVKDesc` and `TextureVKDesc`, `CreateTextureVK`.
- NRI [samples](https://github.com/NVIDIA-RTX/NRISamples) README: `MemoryAliasing`, `Multiview`, `Multisample`, `Wrapper`.
- Plume [`plume_render_interface.h`](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_render_interface.h): `RenderCommandList`, `RenderDevice`, `RenderTexture`, etc.
- Plume [`plume_vulkan.cpp`](https://github.com/renderbag/plume/blob/230c502dfe9c2e02a1ad4321b3e1ada46c45f876/plume_vulkan.cpp): see `VulkanCommandList::barriers` and `VulkanInterface::VulkanInterface`; upstream [barrier-rework issue #11](https://github.com/renderbag/plume/issues/11) remains open.
- NVRHI [`include/nvrhi/vulkan.h`](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/include/nvrhi/vulkan.h) and [programming guide](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/doc/ProgrammingGuide.md).

**Preliminary rank, not performance evidence:** NRI first; direct Vulkan reference; Plume as competing simpler option; NVRHI as convenience/validation alternative. Do not adopt a final winner based on feature lists.

## Why OpenXR affects the baseline

Use **our native OpenXR instance/session**. The `XR_KHR_vulkan_enable2` path creates compatible Vulkan instance/device using `xrCreateVulkanInstanceKHR` and `xrCreateVulkanDeviceKHR`; `xrEnumerateSwapchainImages` yields runtime-owned `VkImage` objects. Render using the correct acquired image and `xrEndFrame` projection layer; never assume a WSI swapchain or shadPS4's packed side-by-side image.

Authoritative references:
- [Khronos OpenXR specification — XR_KHR_vulkan_enable2](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XR_KHR_vulkan_enable2).
- [Khronos OpenXR rendering and swapchains](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/rendering.adoc).

An RHI's ability to use externally created Vulkan images is desirable for OpenXR, but not enough by itself: test image layout, ownership, array slices, depth, synchronization and whether the library forces a WSI swapchain.

## First executable experiment — independent native RHI micro-workload

Implement one standalone, game-asset-free test harness with a **backend-neutral operation tape**:

1. Open a host GPU device, choose a supported colour/depth/stencil format and sample count; no PS4 or shadPS4 dependency.
2. Create a vertex/index buffer, one constant buffer, one sampled texture, and a colour+depth target (including 2x/4x MSAA where supported).
3. Consume compiler-produced host-native shader bytecode (e.g. SPIR-V on Vulkan); execute two indexed draws with distinct bindings and blend/depth/stencil states.
4. Execute one compute dispatch writing an intermediate texture, transition it for sampled graphics use, then composite it.
5. Produce two distinguishable eye images (different per-eye view parameters, not a copy of one image). Test two-array-layer mode plus two-separate-target fallback.
6. Capture deterministic images and API validation diagnostics; measure CPU record/submit, GPU execution and staging separately.
7. Repeat the same work through direct Vulkan, NRI, Plume, and optionally NVRHI. Do not compare SDK overhead in different workloads.

**Follow-on, distinct gate:** implement native `XR_KHR_vulkan_enable2` device ownership and render into actual OpenXR-acquired `VkImage` surfaces. Do not make actual headset availability a prerequisite for the headless first pass.

### Acceptance / stop conditions

- Correct output (deterministic dimensions, per-eye differentiation, blending, depth/stencil, both passes), zero critical validation errors.
- Real Vulkan device and executable GPU work, not just compilation or API declaration existence.
- No dependence on `src/src/video_core/`, GNM, PM4, PS4 texture cache, shadPS4 VR host or PSVR API calls.
- Each backend's new/changed Vulkan device/image ownership or state semantics clearly documented.
- Report measured CPU and GPU latency separately; do not claim improved FPS for a synthetic test.
- Fail or mark unsupported when a library cannot adopt a required image, produce the requested stereo view or preserve a dependency, rather than silently adding an unmeasured host copy.
- Preserve dependency/license/attribution notices. Plume's contribution rules prohibit AI-authored upstream proposals; do not submit any.

## Current blockers and first next tasks

1. **No executable cross-library benchmark performed:** the current research runtime has CMake/Clang/Ninja but neither Vulkan/OpenXR development headers nor network access to clone/build dependencies. This is a *tool environment limit*, not an API failure.
2. Recover additional title-side UE4 RHI operations: resource creation and lifetime, render target binding, shader/descriptor binding, transitions, viewport, stereo views. Keep this independent of Codex's A3/B3 live-hook work.
3. Determine whether UE4 4.18's PS4 shader bytecode can be recreated from cooked shader/material metadata, translated from GCN, or requires per-title shader recovery. No target-specific reusable solution is proven.
4. Evaluate NRI `MemoryAliasing` and `Multiview` samples on a real workstation **as feature tests**, without conflating PS4 memory aliasing with UE4 semantics.
5. Record a per-backend compatibility matrix from the identical runnable harness and decide a minimum common engine RHI subset.

**Provisional conclusion:** NRI is the best-supported first implementation target **on source features only**. Library selection cannot resolve the missing UE4 semantic hook coverage or shader provenance. No renderer implementation, runtime interception, performance improvement or native game-frame proof has been achieved in this pass.
