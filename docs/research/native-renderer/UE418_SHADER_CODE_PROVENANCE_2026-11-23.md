# Native PCVR Renderer — UE4.18 shader-code provenance above the RHI boundary

**Date:** 2026-11-23  
**Scope:** One *independent* static source comparison: where a UE4.18-derived cooked game might preserve shader identity, stage, bytecode and parameter information **before** a PS4 RHI shader-state binder. This is **not** the concurrent binder/allocation correction, a native renderer, a GCN translator, or an emulator integration.  
**Project base:** `research/native-plume-openxr-ownership-20261123` @ `a9ddfe5bef36f4087d70f096810ce9c748764321`; `main` independently checked at `e2f981c40cba7c8490177c20c179cb3608cc1442`.  
**Public reference:** UE4.18.3 source mirror [`aceyan/UE4.18.3_release` @ `4c9fc5d3a2bf51029aa7d7295e09620d298739ef`](https://github.com/aceyan/UE4.18.3_release/tree/4c9fc5d3a2bf51029aa7d7295e09620d298739ef). **Reference, not a verified exact match for FromSoftware's shipped PS4 build.**  
**Target identity:** Déraciné 1.00 PS4 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, verified independently against the privately supplied copy. No SELF, ELF, shaders, extracted game assets or source dumps committed.  
**Execution:** source audit plus a private, read-only byte-string scan; no game process, runtime hook, build, GPU experiment or OpenXR session.

## Executive result

**There are two distinct UE4 shader-resource layers; conflating their names would misidentify the native port seam.** Public UE4.18 has:

1. A *ShaderCore* `FShaderResource` (inherits `FRenderResource` and `FDeferredCleanupInterface`) which carries the **compiled platform shader bytes**, `FShaderTarget` (platform/frequency), `OutputHash`, code-sharing flag, and typed RHI shader references.
2. An *RHI* `FRHIShader` (inherits `FRHIResource`) which carries an RHI hash and gets **intrusive atomic refcount + deferred deletion** from `FRHIResource`.

The intrusive `+0x08` count / delete-pending / deferred-delete contract recovered in the separate Déraciné shader-lifecycle report is **consistent with `FRHIResource`**, but does **not** by itself show that the recovered object is public UE4's higher-level, bytecode-owning `FShaderResource`. This is a **source-based classification caution**, not an address-level correction of the concurrent investigation.

**New candidate investigation:** locate the Déraciné equivalent of the *creation edge*, where a typed shader resource/hash and platform code pass to `RHICreate*Shader` or a native shader library, and then follow the returned `FRHIShader` object into the already recovered binder `0x0CBE210`. This would give a deterministic link between PS4 compiled code and the semantic resource bound at each draw. It **would not** turn GCN into a PC-executable shader.

## 1. The public UE4.18 resource split — verified source facts

| Layer | Public code evidence | What is guaranteed by this public version | What remains unproven for Déraciné |
|---|---|---|---|
| `FShader` | [`Shader.h:487–558`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/Shader.h#L487-L558) and [`Shader.cpp:918–1008`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L918-L1008) | Carries shader type, material shader-map hash, vertex-factory association, parameter bindings and a pointer to `FShaderResource`; serializes much identity/binding data | Original-game class identity, presence, derived binding shapes and stripping |
| `FShaderResource` (ShaderCore) | [`Shader.h:108–129`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/Shader.h#L108-L129) and [`:236–279`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/Shader.h#L236-L279) | Owns platform/frequency, compiled code, uncompressed size, `OutputHash` and typed RHI references; extends `FRenderResource`, not `FRHIResource` | Exact target object layout, whether code is inline or in a library |
| `FRHIResource` / `FRHIShader` | [`RHIResources.h:29–124`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHIResources.h#L29-L124) and [`:162–182`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHIResources.h#L162-L182) | RHI resource has an atomic counter, pending-delete CAS, deferred-delete machinery; `FRHIShader` has `SetHash/GetHash` and stage-specific subclasses | Which Déraciné concrete RHI object corresponds to the recovered refcounted object |

In public `FShaderResource::Release`, `NumRefs` is changed in the **game-thread** path and the object is sent to `BeginReleaseResource` and `BeginCleanup`: [`Shader.cpp:536–563`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L536-L563). Contrast the `FRHIResource::Release` interlocked count and delete queue in [`RHIResources.h:42–66`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/RHI/Public/RHIResources.h#L42-L66). These are materially different lifecycles.

**Inference:** the currently recovered refcount/deferred-delete behavior is a more direct type-family discriminator for *RHI* resources than for ShaderCore `FShaderResource`. It does not establish any particular object size or offsets in the shipped binary.

## 2. Shader identity exists before the RHI bind in the public engine

Public UE4.18's `FShaderCompilerOutput::GenerateOutputHash` hashes **compiled platform code followed by the compiler parameter map**; it *excludes optional shader-code attachments*: [`ShaderCore.cpp:187–200`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCore.cpp#L187-L200). The parameter map itself associates *named* parameters with buffer/base/size allocations: [`ShaderCore.h:143–178`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/ShaderCore.h#L143-L178).

`FShaderResource::FShaderResource(const FShaderCompilerOutput&...)` saves target, compresses compiled code and assigns that output hash: [`Shader.cpp:390–417`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L390-L417). `FShaderResource::GetId()` contains platform/frequency, output hash and potentially specific shader type: [`Shader.cpp:764–770`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L764-L770).

The public `FShaderResource::InitRHI` flow:
1. Checks shader platform compatibility.
2. Retrieves/decompresses cooked code if stored inline.
3. Dispatches by `Target.Frequency` to `FShaderCache::CreateVertexShader`, `CreatePixelShader`, `CreateComputeShader`, etc., passing `Target.Platform`, `OutputHash` and platform code.
4. Releases code from runtime memory after initializing RHI resources on no-editor-data platforms.

Source: [`Shader.cpp:645–733`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L645-L733).

Further, `FShaderCodeLibrary::Create*Shader` either requests an RHI shader from a **native platform archive** by hash or falls back to `RHICreate*Shader(Code)`, then assigns that hash to the returned RHI shader using `SafeAssignHash`: [`ShaderCodeLibrary.cpp:781–821`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L781-L821), [`:1123–1180`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L1123-L1180), [`Shader.cpp:635–643`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L635-L643).

**Implication:** shader **frequency** and the stable compile-output **hash** may be recoverable at shader-creation time, *even if* a later platform state binder carries only PS4 register/program objects and ambiguous per-stage pointer slots. No Déraciné symbol or equivalent creation edge has been recovered in this pass.

Important qualifications:
- `OutputHash` is a *compiled-output identity*, not an HLSL source hash, and is not generally recoverable from a small `program_data+0x06` scalar.
- Public `FShader` also tracks material, shader type and vertex-factory identity, but that richer `FShaderId` is **not automatically present** on a later RHI object or in a stripped shipping executable.
- Shader creation occurs less often than draw submission. It can establish resource provenance and a cache key but **cannot alone reconstruct per-draw parameter values**.

## 3. Cooked shader storage is conditional — not always inline

Source-verifiable alternatives:

| Path | Source contract | How it affects investigation |
|---|---|---|
| Inline cooked shader code | `FShaderResource::Serialize` writes `Code`, either for pre-code-sharing versions or when `bCodeShared=false` | Look for package deserialization and creation path |
| Shared `ShaderArchive-<format>.ushaderbytecode` | `SerializeShaderCode` can store code separately from material assets; archive index maps output hashes to compressed code; `FShaderCodeArchive` loads/decompresses and creates typed RHI shader | Asset inventory + archive format identification required; file-name convention alone not proof |
| **Native** platform shader library | `OpenShaderCode` first tries `RHICreateShaderLibrary`; `PackageNativeShaderLibrary` delegates format-specific packing to `IShaderFormatArchive` and may remove fallback archive files | Do *not* assume a `.ushaderbytecode` archive must exist; PS4 format support/packaging was not verified |

Citations: [`Shader.cpp:466–529`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/Shader.cpp#L466-L529), [`ShaderCodeLibrary.cpp:29–112`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L29-L112), [`:112–151`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L112-L151), [`:668–716`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L668-L716), [`:781–801`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Private/ShaderCodeLibrary.cpp#L781-L801).

The public `FShaderCodeReader` can parse optional keyed attachments appended to platform-code bytes; `FShaderCodePackedResourceCounts` contains resource-count fields. It does **not** imply portable IR or full named parameters survive shipping: [`ShaderCore.h:577–585`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/ShaderCore.h#L577-L585), [`:603–745`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/ShaderCore.h#L603-L745). The full `FShaderCompilerOutput::ParameterMap` serialization at [`ShaderCore.h:855–885`](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/ShaderCore.h#L855-L885) is specified as communication with a **shader compile worker**; it is **not** evidence the entire compiler output is packaged in cooked title assets.

**Private target check performed:** the provided SELF matched the above SHA-256 exactly. A raw byte-string scan found one `ShaderCore.cpp` occurrence and found **no literal** `ShaderArchive-`, `.ushaderbytecode`, `ShaderCode-`, `FShaderResource::InitRHI`, `FShaderResource`, or `FShaderCodeLibrary`. Raw string absence is an extremely weak negative: stripping, compile-time folding, archive format changes or non-file-backed compressed data may hide these strings. **No conclusion** about which storage path Déraciné uses follows from the scan.

## 4. Correct architectural seam and proposed data model

```text
[Cooked material/global shader data, maybe shared/native archive]
     │ platform/frequency + compiled PS4 code + OutputHash
     ▼
  FShader / ShaderCore FShaderResource (PUBLIC UE4.18 MODEL)
     │ stage-specific creation  ────────►  RHI FRHIShader-derived object
     │                                         │ returned handle + hash
     │                                         ▼
     │                                 PS4 state/shader binder
     │                                 candidate RVA 0x0CBE210
     │                                         │
     └── parameter and material bindings ──────┴──► later semantic draw
```

For a future *independent* native renderer, use **two identities** rather than conflating one pointer with a shader program:

- **Source shader identity:** `{platform, frequency, output_hash, optional shader_type/material_map/vf evidence}`. This is the prospective **offline translation/cache** lookup key. Any code bytes remain private to the user; hash is recorded only if its provenance is proven.
- **Runtime RHI shader identity:** `{instance_generation, opaque_rhi_shader_handle, source_shader_id_if_proven}`. A pointer alone is not stable across object destruction/reuse. Instance generation/lifetime must be observed or derived.

A prospective `ShaderBind` event would reference the runtime RHI identity and slot-selection predicate; `ShaderCreate` would establish the optional mapping to source identity. `ParameterUpdate` and vertex input events are *separate* work. This record vocabulary is a **design hypothesis**, not an established game ABI.

**Execution is still blocked.** The public UE4.18 creation path holds **compiled PS4 platform code**, not HLSL, DXIL or SPIR-V. Even proving the higher layer exists would not remove the need for a standalone PS4 shader-to-PC strategy (offline translation or independently justified reconstruction). No PS4 SDK-specific code path was available in the public source audited here. We should not adopt or modify shadPS4's GPU translator to resolve this task.

## 5. Falsifiable next experiment — target-specific static provenance

This intentionally avoids touching the concurrent `0x0CBE210` allocation/slot correction.

**H1:** the shipped Déraciné binary has an upstream shader-creation path that (a) distinguishes shader stage, (b) obtains a PS4 compiled shader from inline or archive-backed storage, and (c) creates or retrieves the same RHI shader family eventually bound at `0x0CBE210`.

Bounded method, in an **independent** worktree/branch:
1. In the already verified target ELF, find the **callers** of PS4 shader resource creation, not just GNM shader setters. Look for a typed call family, a stage/frequency branch, output-hash copying/setting, and an `FRHIShader`-shaped returned object. Do not assume public symbols, names, layouts or calling convention.
2. Independently identify the code/asset storage path (inline, shared archive, native archive, or custom); do not infer this from absent strings alone. If package assets are unavailable, record **blocked**, not `no library`.
3. Establish a *static* relation `upstream creation → RHI handle → known shader binder` via complete call/dataflow and ownership edges. Check RHI returned-object/vtable family against the binder without assigning a type by resemblance.
4. If a specific stage + output hash are recoverable, describe a **passive, separately authorized** future observer with only `(stage, output-hash, opaque-RHI-id, generation)` scalar records. Correlation to the draw-associated shader handles, including SC-02's observed fallback to `state+0x1E0`, is a later **runtime test**, not proof in this report.

| Outcome | Meaning |
|---|---|
| Stage and output hash flow into an RHI shader object later selected by the binder | H1 supported: stable shader provenance is available *above* GNM |
| Compiled code is created without retained hash/type metadata | Upstream source link requires code-derived identity; do **not** invent the compile output hash |
| Shader state creation is direct PS4 custom code, no `FShaderResource`-equivalent path found | Public UE4 seam does not transfer as-is; report search coverage and alternatives |
| Assets or indirect callee targets are unavailable | Inconclusive, with explicit blocker |

**Never use `program_data+0x06` as the primary shader hash without independent evidence.** Do not ask Codex to rewrite its correction in parallel or infer that the source-model distinction disproves its binder.

## 6. Verification and limitations

Performed:
- Reviewed the exact-revision public UE4.18.3 type, creation, code-sharing, identity and serialization source paths cited above.
- Confirmed SHA-256 of the private input SELF matches target.
- Completed read-only raw string presence/absence scan described in §3.
- Independently checked `main` and the base commit before branching.

Not performed: ELF function matching, full Déraciné asset inventory, parser for cooked shader archives, GCN translation, native Vulkan/NRI pipeline, GPU validation, runtime capture or game execution. No shader cache key or bytecode extracted from the target.

**Decision:** preserve `0x0CBE210` as a binder candidate while separately prioritizing **creation-time stage/hash provenance**. This complements, rather than duplicates, the concurrent shader-resource lifetime correction.
