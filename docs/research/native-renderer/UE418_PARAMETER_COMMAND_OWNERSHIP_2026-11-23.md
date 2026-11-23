# Native PCVR Renderer — UE4.18 deferred parameter-command ownership contract

**Date:** 2026-11-23  
**Investigation:** A bounded public-source audit of the *semantic command-recording boundary* for constant parameters, uniform buffers, texture/SRV/sampler bindings. This is distinct from shader-object lifetime, NRI descriptor-pool reuse, shader provenance and PS4 binder-slot recovery.  
**Source revision:** [UE4.18.3 mirror `4c9fc5d3a2bf51029aa7d7295e09620d298739ef`](https://github.com/aceyan/UE4.18.3_release/tree/4c9fc5d3a2bf51029aa7d7295e09620d298739ef); reference source, **not verified identical to Déraciné's customized shipping engine**.  
**Project starting point:** `research/native-rhi-foundation-20261123`, initial commit `313f277c7ac5a19a5de50cdd460c6a8568103225`. Independently checked `main=e2f981c40cba7c8490177c20c179cb3608cc1442` before branching. No executable, assets, runtime hooks, emulator integration or GPU tests.

## Verdict

**A host renderer must distinguish command-time *copied scalar bytes* from command-time *opaque resource references*.** In public UE4.18, `SetShaderParameter` makes a command-list-owned copy of `NumBytes` in deferred mode, whereas `SetShaderUniformBuffer`, `SetShaderTexture`, `SetShaderResourceViewParameter` and `SetShaderSampler` record typed **raw RHI pointers** in their deferred command objects. This is a source-proven semantic difference and a potential use-after-free / stale-binding trap in a future independent native capture adapter. The source does **not** prove the full execution-time lifetime guarantees or what Déraciné's PS4 fork does.

A separate special case, `BuildLocalUniformBuffer`, copies its *entire supplied contents* into command-list storage and delays creating the RHI buffer. Its work area retains a **pointer to the supplied layout**, rather than copying the layout itself. Do not confuse it with a persistent `FRHIUniformBuffer` or assume that a command list's pointer identities remain valid across executions.

## Evidence: four distinct representations

| Operation | Immediate/bypass path | Deferred command fields | Source-established payload ownership |
|---|---|---|---|
| `SetShaderParameter(Shader, BufferIndex, BaseIndex, NumBytes, NewValue)` | Calls `RHISetShaderParameter` directly | shader pointer, 3 indices/sizes, pointer to **copied bytes** | `Alloc(NumBytes,16)` and `Memcpy` happen *at record time* |
| `SetShaderUniformBuffer(Shader, BaseIndex, UniformBuffer)` | Calls `RHISetShaderUniformBuffer` | shader pointer, base index, **raw uniform-buffer pointer** | No contents copy in this command constructor |
| `SetShaderTexture / SetShaderResourceViewParameter / SetShaderSampler` | Calls corresponding RHI method | shader pointer, index, **raw texture/SRV/sampler pointer** | No texture contents copy in these constructors |
| `BuildLocalUniformBuffer(Contents, ContentsSize, Layout)` | Creates a single-frame RHI uniform buffer immediately | command-list work area holding **copied contents** and **layout pointer** | `Alloc(ContentsSize, UNIFORM_BUFFER_STRUCT_ALIGNMENT)` + `Memcpy`; later RHI creation |

Exact source:
- [`RHICommandList.h:516–630`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L516-L630): deferred parameter, uniform-buffer, texture, SRV, UAV and sampler command member fields/constructors.
- [`RHICommandList.h:1785–1867`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L1785-L1867): graphics command-list bypass/deferred branches; `SetShaderParameter` copies bytes, other commands enqueue handles.
- [`RHICommandList.h:1420–1490`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L1420-L1490): local uniform work area and command-list ownership checks.
- [`RHICommandList.h:1751–1777`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L1751-L1777): local uniform bypass versus deferred creation.
- [`RHICommandList.h:2392–2424`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHICommandList.h#L2392-L2424): second graphics command-list variant with `IsOutsideRenderPass()` guard; don't assume all command-list interfaces have identical call preconditions.

### Uniform-buffer layouts are *not* just constant-byte counts

`FRHIUniformBufferLayout` carries `ConstantBufferSize`, `ResourceOffset` and a typed `Resources` array. Its hash mixes these attributes; its equality operator compares all three, not just byte size. `FRHIUniformBuffer` stores a **pointer to** a layout and inherits `FRHIResource`: [`RHIResources.h:196–301`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHIResources.h#L196-L301).

This makes `{constant_byte_size}` alone an invalid key for a UE4-compatible uniform-buffer resource. Resource-table layout is semantic metadata and should survive the UE4→host boundary, even if the host eventually packs buffers and descriptors differently.

## Consequence for a native UE4→host command stream

Proposed, **not yet implemented** event shapes:

```text
SetConstants {shader_instance, buffer_index, base_index, byte_count,
              owned_byte_payload}
BindUniformBuffer {shader_instance, base_index, uniform_buffer_generation}
BindTexture {shader_instance, slot, texture_generation}
BindSRV {shader_instance, slot, view_generation}
BindSampler {shader_instance, slot, sampler_generation}
CreateLocalUniformBuffer {command_list_epoch, layout_id, copied_contents}
```

Requirements:
1. **Copy scalar parameter bytes at observation/record time**, not when the host GPU eventually executes. Otherwise the caller may mutate or release `NewValue` before deferred execution.
2. **Resolve and retain resource references through a generation-aware registry.** Never persist a raw guest pointer as the host cache key, nor silently treat it as a valid lifetime token. The public UE4 command structs store raw pointers, but this audit did not prove the upstream retention or completion guarantees.
3. **Preserve shader stage/instance + binding slot.** A numeric `BaseIndex` or `TextureIndex` is not itself a Vulkan descriptor binding; translation needs recovered shader metadata and a host pipeline layout.
4. **Record layout identity and resource-table metadata** for uniform buffers. The layout pointer in `FLocalUniformBufferWorkArea` is a lifetime constraint; do not store that pointer in a cross-frame capture.
5. **Separate command-list recording, RHI execution, and GPU completion epochs.** The code demonstrates immediate and deferred branches. Neither branch proves when a GPU read finishes.
6. **Do not infer a draw's active bindings from creation events alone.** Command ordering and state changes must be associated with the same command-list/context.

These rules complement the independent NRI descriptor-pool `{pool,epoch,slot}` requirement: UE resource generation protects the *guest/semantic* reference, while NRI descriptor generation protects the *host descriptor allocation*. They are separate namespaces.

## Falsifiable experiment: command-time snapshot versus execution-time pointer

Use a **host-only**, fake RHI command-recorder test; do not hook the game.

1. Record `SetShaderParameter` with a four-byte array `[1,2,3,4]`; mutate the caller's array to `[9,9,9,9]` before execution. Expected command payload remains `[1,2,3,4]`.
2. Record `BindTexture` with resource identity `(id=7,generation=1)`, destroy it, then reuse numeric id 7 with generation 2. Expected: execution/capture rejects the stale generation 1 binding; it must not silently resolve generation 2.
3. Record a local uniform buffer with a known 16-byte payload and layout id; mutate original payload before execution. Expected local contents unchanged. Destroy/reuse the layout identity before execution: expected a deterministic invalid-layout rejection (or an explicit retained ownership contract), not implicit pointer reuse.
4. Record two `SetShaderParameter` updates to the same shader/slot before a draw, separated by a resource bind. Expected final draw snapshot uses the last constant update and the intervening resource bind, with correct command ordering.
5. Run in both immediate and deferred modes and compare semantic output. **Not** a performance benchmark.

A pass would validate the **proposed host adapter's** ownership rules, not Déraciné's actual ABI. Target validation requires a later independent static proof of corresponding PS4 RHI command paths and a separately authorized passive runtime observer.

## Limitations and next step

**Verified:** public-source fields, copy operations, branch distinctions, uniform-buffer layout semantics and deferred local work-area ownership.

**Unverified:** Déraciné target call sites; whether shipping RHI command recording uses bypass/deferred modes; resource retention elsewhere in UE4; PS4-specific shader register layouts; actual GPU execution ordering. No original binary, private asset or runtime log inspected.

**Next independent target-specific static pass:** identify the original game's `SetShaderParameter` and `SetShaderUniformBuffer` call families above the PS4 backend, then test whether the scalar-byte copy and resource-reference paths are preserved. Only then attach addresses and confidence levels to the event contract. Avoid changing the separate shader-lifecycle correction or any shadPS4 code.
