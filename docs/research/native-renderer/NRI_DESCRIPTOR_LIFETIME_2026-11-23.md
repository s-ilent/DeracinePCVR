# Native renderer: NRI descriptor binding and pool-lifetime contract

**Date:** 2026-11-23  
**Investigation:** one bounded public-source audit of *descriptor layout, update, bind, and pool reset* for a future UE4 4.18 semantic command adapter. This is independent of shader resource provenance, the concurrent Déraciné binder correction, OpenXR swapchain ownership and shadPS4 GPU translation.  
**NRI pin:** [`06ff6f643777e72eda88bc6fb8d56267b85d872f`](https://github.com/NVIDIA-RTX/NRI/commit/06ff6f643777e72eda88bc6fb8d56267b85d872f) (upstream main verified at this commit; commit dated 2026-11-19).  
**Project:** `s-ilent/DeracinePCVR`; checked `main=e2f981c40cba7c8490177c20c179cb3608cc1442`, `research/native-rhi-foundation-20261123=6b359f520b9f41f854b2aac41bb74526f6c60e87` before branching. No game binary, shaders, hooks, builds or GPU tests used.

## Verdict

NRI provides an explicit, workable **descriptor set + pipeline layout** API for a future recovered UE4 resource/parameter stream. **It does not automatically preserve guest UE4 resource lifetimes or permit descriptor pools to be reset merely because an OpenXR swapchain image was released.** The native adapter needs its own resource-handle generation and GPU-completion tracking, and must decide whether to allocate stable/material sets or frame-local sets.

An important source-level implementation detail: NRI's Vulkan descriptor-set wrappers are allocated from a **fixed-sized vector inside the pool**, and `ResetDescriptorPool` resets the underlying Vulkan pool and sets the wrapper allocation cursor back to zero. Thus an old NRI `DescriptorSet*` can point to a wrapper subsequently **reused for a different VkDescriptorSet**. A pointer-only descriptor identity is invalid across reset.

## Evidence — exact pinned source

| Concern | Public source behavior | Consequence for our adapter |
|---|---|---|
| Pool reset semantics | NRI explicitly documents that `ResetDescriptorPool` wipes out allocated sets; [`Include/NRI.h:136–149`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRI.h#L136-L149) | Reset only after all GPU submissions using those sets are complete, and after dropping all application references to old sets |
| Pool wrapper reuse | `DescriptorPoolVK::Create` resizes `m_DescriptorSets` to `descriptorSetMaxNum`; allocation stores each result in `m_DescriptorSets[m_DescriptorSetNum++]`; reset calls `vkResetDescriptorPool` then sets `m_DescriptorSetNum=0`; [`DescriptorPoolVK.hpp:18–46,61–121`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorPoolVK.hpp#L18-L121) | Store `(pool_id, pool_epoch, set_slot)` as host descriptor identity, not only `DescriptorSet*` |
| Descriptor layout mapping | NRI `DescriptorRangeDesc` encodes type, stage mask, base register and count; `DescriptorSetDesc` adds register space; `PipelineLayoutDesc` lists sets and root parameters; [`NRIDescs.h:1117–1190`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1117-L1190) | Recovered UE4 stage/register semantics must be normalized to explicit host layout before Vulkan pipeline creation |
| Vulkan binding translation | `PipelineLayoutVK::CreateSetLayout` applies configurable `VKBindingOffsets`, emits VkDescriptorSetLayoutBinding entries, and sets per-binding flags; [`PipelineLayoutVK.hpp:320–385`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/PipelineLayoutVK.hpp#L320-L385) | Do not equate guest register index with final Vk binding without offsets; reserve a stable mapping per shader/pipeline |
| Bind ordering | NRI documents `CmdSetPipelineLayout` before resource binding; Vulkan `SetDescriptorSet` indexes the *current* pipeline layout, resolves its register space and calls `vkCmdBindDescriptorSets`; [`NRI.h:152–160`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRI.h#L152-L160), [`CommandBufferVK.hpp:2038–2116`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2038-L2116) | A captured descriptor bind is meaningful only relative to its active pipeline layout and bind point |
| Update-after-bind | `ALLOW_UPDATE_AFTER_SET` requires pool and range flags, and NRI documents updating after command binding but before submit; [`NRIDescs.h:1071–1092`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1071-L1092), [`DescriptorPoolVK.hpp:34–42`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorPoolVK.hpp#L34-L42) | Treat as an opt-in capability with explicit device tier checks, **not** default behavior or permission to rewrite in-flight descriptors |
| Variable-sized arrays | Requires `tiers.bindless >= 1` and `tiers.resourceBinding >= 2`; Vulkan implementation forwards variable counts to `vkAllocateDescriptorSets`; [`NRIDescs.h:1078–1092`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1078-L1092), [`DescriptorPoolVK.hpp:61–104`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/DescriptorPoolVK.hpp#L61-L104) | A basic material-binding implementation should not require bindless; use ordinary fixed ranges first |
| Root constants | NRI `RootConstantDesc` requires 4-byte-multiple sizes; Vulkan path maps to push constants; [`NRIDescs.h:1137–1143`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Include/NRIDescs.h#L1137-L1143), [`CommandBufferVK.hpp:2119–2142`](https://github.com/NVIDIA-RTX/NRI/blob/06ff6f643777e72eda88bc6fb8d56267b85d872f/Source/VK/CommandBufferVK.hpp#L2119-L2142) | Don't assume all UE4 constant-buffer updates fit into Vulkan push constants; preserve ordinary constant buffers |

## Specific hazard: pool epoch aliasing

The following sequence is invalid even if C++ pointers look stable:

1. Allocate set A in pool P: NRI returns wrapper pointer W, backing Vulkan set V1.
2. Record and submit a draw using A.
3. Reset P without waiting for GPU completion (Vulkan lifetime violation), or reset after completion but retain W in a material cache.
4. Allocate set B in P: wrapper slot W is reused, now backing Vulkan set V2.
5. Material cache looks up A by W and binds B by accident.

The **first** failure is an in-flight use-after-reset. The **second** is a host-side stale-reference/ABA bug even when GPU completion was handled correctly. These are distinct failure modes. Source establishes wrapper reuse; actual failure behavior has not been reproduced on a Vulkan device.

## Proposed title-independent adapter contract (design, not implementation)

```text
GuestShaderBinding {
    stage, logical_register_space, logical_register,
    resource_kind, resource_id, resource_generation,
    view_subresource, sampler_id_or_none
}

HostPipelineBinding {
    shader_program_identity, layout_id,
    register_to_vk_binding_map, stage_visibility
}

HostDescriptorSet {
    pool_id, pool_epoch, set_slot,
    layout_id, resource_generation_dependencies,
    last_gpu_submission_token
}
```

Maintain **separate generations** for guest resource identity and host descriptor pool epochs. Cache keys include pipeline layout identity and resource view/sampler generations. A guest resource can be destroyed and recreated at the same address; a descriptor wrapper can also be reused after pool reset.

**Default allocation strategy:** ordinary fixed descriptor ranges; immutable per-material descriptor sets where bindings stay stable; frame-local sets for frequently changing uniforms/resources; do not mutate a bound set unless a proven update-after-set contract applies. For frame-local pools, retire only on **GPU completion** of all dependent submissions. OpenXR image release is not itself evidence that arbitrary host descriptor sets have finished executing.

**Reset precondition:** all command submissions referencing P have completed, and all CPU-side references to sets allocated in P are invalidated. Then increment `pool_epoch`, reset P and allow allocation. In a test harness, assert stale `(pool_id,epoch,slot)` rejection before calling NRI.

## Falsifiable next experiment

Build a tiny **host-only**, title-independent descriptor-lifetime harness with a synthetic vertex/fragment shader and two sampled textures, not PS4 code. Use NRI's Vulkan backend and validation layers if a device is available.

1. Allocate and bind set A; submit a draw and wait for a real GPU completion signal.
2. Reset its pool; allocate B. Log `(pool, epoch, slot, wrapper address, VkDescriptorSet)`. Test whether wrapper address repeats; **reject** stale A via generation check regardless of pointer reuse.
3. Change texture/view generation and assert the material cache creates or updates the correct descriptor set.
4. Change pipeline layout register space and verify bindings target the new Vk set index rather than the prior layout's index.
5. Repeat with ordinary fixed descriptors only. Separately query feature tiers before any bindless/update-after-set variant.

**Pass:** Vulkan validation is clean, stale handles are rejected, descriptor contents match the intended texture after reset/reallocation, and no pool resets while submissions are in flight. **Fail:** stale A can be rebound, a set is used after reset, or a pipeline-layout change preserves an incompatible binding. **Inconclusive:** no Vulkan-capable device/validation tools available.

## Limits and relation to other tracks

This run inspected NRI's **exact-version public source**, not Déraciné's original binary. It does **not** establish that the shipped UE4 PS4 executable exposes a particular descriptor API, uniform-buffer shape or resource lifetime. It does not compare measured performance of NRI/Plume/NVRHI/direct Vulkan, and it does not solve PS4 shader execution. No native renderer or GPU validation was run. Non-rendering game runtime remains a separate prerequisite.

**Decision:** retain NRI as a candidate RHI; require pool-epoch and guest-resource-generation tracking in the future adapter before the first draw harness. Do not build a descriptor cache keyed only by opaque pointers.
