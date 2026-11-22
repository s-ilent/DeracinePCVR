# Native PCVR — NRI placed texture memory and aliasing boundary

**Date:** 2026-11-23. **Scope:** one new source-level investigation of *native host-owned transient resources*, distinct from OpenXR-imported swapchain images. **Research branch:** `research/native-nri-placed-memory-alias-20261123`, based on `9715c8864d781ef753abb72639d84def31696ce1`. GitHub main was independently checked at `e2f981c40cba7c8490177c20c179cb3608cc1442` before branch creation. No emulator or game code changes.

## Question

Can NRI support an explicit transient render-target allocator for a future UE4-level native RHI, and can the same allocator legally manage OpenXR swapchain images? Distinguish *memory aliasing capability* from *automatic UE4 lifetime reconstruction*.

## Exact source and observations

Audited **NVIDIA-RTX/NRI** commit [`06ff6f643777e72eda88bc6fb8d56267b85d872f`](https://github.com/NVIDIA-RTX/NRI/tree/06ff6f643777e72eda88bc6fb8d56267b85d872f), rather than the changing main branch.

1. [`Source/VK/DeviceVK.hpp:1814–1826`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1814-L1826): NRI-created ordinary (non-video) textures set `VK_IMAGE_CREATE_ALIAS_BIT` alongside mutable-format/extended-usage flags. This enables compatible image aliasing at creation, **not** automatic lifetime or hazard management. Video resources take a different flag path.
2. [`Source/VK/DeviceVK.hpp:1718–1724`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1718-L1724): Vulkan backend advertises `features.resourceAliasing = true`. This is a capability declaration, not evidence that the renderer's resource lifetimes are known.
3. [`Source/VK/DeviceVK.hpp:1917–1956`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L1917-L1956): buffer/image memory requirements are queried from Vulkan, including dedicated-allocation requirements. Native allocator must respect memory type, size, alignment and dedicated constraints rather than group textures merely by nominal byte count.
4. [`Source/VK/DeviceVK.hpp:3156–3189`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L3156-L3189): `CreatePlacedTexture` accepts an NRI memory object plus offset and binds the newly created texture; `CreateCommittedTexture` instead allocates/binds its own memory at lines 3142–3154. Placed textures give the host an explicit memory-sharing primitive.
5. [`Source/VK/TextureVK.hpp:183–197`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L183-L197): the single-texture binding path checks `m_OwnsNativeObjects`, then calls `vkBindImageMemory2` using the memory object's base offset plus requested offset. **Wrapped** textures cannot use this binding path.
6. [`Source/VK/TextureVK.hpp:121–129`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L121-L129) and [lines 3–15](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/TextureVK.hpp#L3-L15): Vulkan image wrappers set `m_OwnsNativeObjects = false`; destruction skips native image destruction. This is the opposite ownership contract from host-created transient images.
7. [`Source/VK/MemoryVK.hpp:36–70`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/MemoryVK.hpp#L36-L70): allocated memory may use VMA with `VMA_ALLOCATION_CREATE_CAN_ALIAS_BIT`; dedicated allocations are deferred to binding when required. This enables reuse of allocation ranges but does not establish non-overlapping resource use.
8. [`Source/VK/MemoryVK.hpp:3–12`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/MemoryVK.hpp#L3-L12): memory wrappers are non-owning; allocated memory is freed by its NRI owner. Therefore allocator lifetime must outlast every bound resource and outstanding GPU reference.
9. [`Source/VK/DeviceVK.hpp:3902–3924`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DeviceVK.hpp#L3902-L3924): batch `BindTextureMemory` submits `vkBindImageMemory2` for caller-specified bindings. This is an explicit API, not a lifetime inference engine.

## Architectural decision (proposal, not runtime verification)

Keep **two non-interchangeable texture classes** in the native host:

| Class | Memory owner | NRI representation | Aliasing policy |
| --- | --- | --- | --- |
| UE4-created transient render targets | Native host allocator | NRI-created placed textures | Optional after proven disjoint lifetimes and Vulkan compatibility |
| Persistent UE4 resources | Native host allocator | Committed or separately placed textures | No implicit aliasing |
| OpenXR swapchain color/depth images | OpenXR runtime | Non-owning NRI Vulkan image wrappers | **Never** bind/rebind or include in host transient alias heap |

For native transient targets, a proposed `ResourceLifetime` record should carry stable logical resource ID, create/destroy events, usage flags, format, dimensions, mip/layer/sample counts, Vulkan memory requirements, first/last submitted use, and completion fence/timeline value. A placed-memory allocator may overlap allocations **only when GPU lifetimes are proven non-overlapping**, memory constraints are compatible, and required synchronization/layout transitions are supplied. This does not imply the original UE4 PS4 renderer used the same allocation strategy.

The imported OpenXR image is already allocated by its runtime; NRI's wrapper is an *image access object*, not permission to manage its storage. Host may create views and record rendering, but must honor OpenXR wait/release and queue/layout rules established by the preceding reports.

## What is not established

- No UE4 4.18 transient resource creation/destruction addresses were recovered in this run. Shader maps, render-target pools, and RHI resource refcounts remain separate reverse-engineering work.
- No proof that two specific Déraciné targets have disjoint lifetimes; therefore **no aliasing recommendation for game resources yet**.
- No Vulkan driver execution, NRI build, GPU validation, headset run, or performance measurement. All findings above are source observations, not test results.
- Vulkan aliasing hazards and memory requirement compatibility still require an actual focused validation harness. Do not treat `VK_IMAGE_CREATE_ALIAS_BIT` as a synchronization mechanism.
- The Vulkan `BindTextureMemory` batch path deserves a targeted validation test if used; the individual placed-resource path is the simpler initial harness.

## Next falsifiable experiment

Create an asset-free Vulkan/NRI harness, using the pinned NRI commit. Allocate one NRI device-local memory range satisfying the queried requirements for two compatible ordinary color textures, create two **placed** images bound to the same range, and submit sequential clear/render/readback operations with explicit layout and alias synchronization. Validate deterministic readback and zero validation errors, including a deliberately *non-overlapping* control pair. Reject incompatible memory types/alignment and dedicated-only allocations before binding. Separately wrap an externally owned `VkImage` and assert that the placed-memory binding API rejects it without modifying native image ownership. Keep this as a synthetic host test, **not** a shadPS4 resource-alias workaround or GPU benchmark.

**Pass:** correct memory ownership, explicit and valid synchronization, predictable sequential image results, no premature frees. **Fail:** illegal bind, validation hazard, image destruction of an external handle, or reuse before completion. Only after this test should a UE4 RHI lifetime trace be used to decide which actual resources could share memory.

## Boundary

This research concerns the **PC-native host allocator**. It does not implement or modify shadPS4's renderer, GNM/PM4, GPU translation, texture cache, or VR presenter. The non-rendering game/runtime architecture remains an independent unsolved problem. No copyrighted game binaries or shaders are included.
