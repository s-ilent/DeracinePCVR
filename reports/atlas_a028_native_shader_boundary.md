# Native PC renderer boundary: shadPS4 is an oracle, not a required graphics runtime

**Status:** Research/architecture decision for the Déraciné PCVR project. This note accompanies independent Binary Atlas A-028 and does not itself change implementation.

## Goal

Preserve and execute Déraciné's original x86-64 game/UE4 logic, while replacing the PS4-specific graphics path with a **native Vulkan/OpenXR renderer**, not accelerating its emulated PM4/GNM renderer indefinitely.

Target:

```text
Original game code (x86-64)
  -> native UE4/PS4 RHI semantic adapter
  -> native Vulkan renderer
  -> OpenXR stereo presentation

Original PS4 shader artifacts
  -> standalone GNM shader-metadata parser
  -> isolated GCN -> SPIR-V translation (offline/cacheable where possible)
  -> native pipeline and resource layout
```

The graphics adapter must preserve guest-visible state, resource lifetime and synchronization, and VR-specific pacing. A native shader resource can still carry original PS4 descriptors and shader hashes; using PS4 *data formats* is not equivalent to running a PS4 GPU emulator.

## Explicit non-goals for the native renderer

- Depend on shadPS4 `Rasterizer::Draw` as the canonical draw-command source.
- Reconstruct the original RHI scene by interpreting completed GNM PM4 packets in production.
- Require `AmdGpu::Regs`, `Liverpool` GPU register state, `TextureCache`, `BufferCache`, or emulator command submission as stable ABI components.
- Assume shadPS4 `sharp_idx` / unified Vulkan descriptor binding is the game's original UE4 binding slot: these are *resolved backend values* and differ from GNM logical `apislot`.
- Depend on full shadPS4 runtime solely to locate `OrbShdr` or decode `GnmInputUsageSlot`.

## Things we can still reuse

- **Platform facts:** GNM `GnmShaderBinaryInfo` and `GnmInputUsageSlot` structures; GCN ISA, user-data register banks, resource descriptor layouts, tiling formats.
- **Algorithms/source, selectively:** shadPS4 GCN-to-SPIR-V translation, graphics formats, shader legalization and scheduling approaches, if modularized behind a narrow interface and with compatible licensing.
- **Validation oracle:** shadPS4's existing runtime can provide a bounded, read-only shader-memory/metadata sample, or GPU-side draw/resource fingerprints to compare against our native path. This does not make shadPS4 a runtime dependency.
- **Guest compatibility substrate:** initial PS4 SELF loading, syscall shims and memory mapping may remain emulator-derived while host services are replaced incrementally; they are separate from the graphics backend.

## A-028 methodology

1. Implement a **standalone** shader-input-usage parser with no shadPS4 includes or GPU renderer dependency. It accepts a complete *authorized* shader-memory snapshot. Output only metadata: shader hash, stage/type, raw GNM 4-byte records and usage-class histogram.
2. Reject invalid bounds and incomplete trailers; don't infer usage slots from SPIR-V or shadPS4 Vulkan descriptors.
3. Use synthetic fixtures and public OpenGNM/GNM container examples to test format handling. These establish format correctness, **not Déraciné runtime reachability**.
4. If a live Déraciné sample is needed, first exploit the existing host-side `SearchBinaryInfo()` capture boundary as an optional read-only instrumentation point, without guest detours or GPU readback. The produced snapshot should be sufficient to rerun the independent parser.
5. Compare decoded GNM `usagetype/apislot/startregister/flags` and parsed resource layout with SC-series effective `sharp_idx` and Vulkan bindings; discrepancies are diagnostic information, not automatically bugs.
6. **Do not** introduce any production dependency on `AmdGpu::SearchBinaryInfo()`, `AmdGpu::GetParams()` or `PipelineCache::GetProgram()`. The eventual native runtime uses its own shader-artifact discovery and metadata parser.

## Native handoff contract (future)

```text
ShaderArtifact {
    original_shader_hash,
    stage,
    gcn_program_reference,         // asset/guest memory while valid, not an emulator cache pointer
    input_usage_declarations[],    // raw GNM ABI: kind, api_slot, start_reg, flags
    translation_key,               // GCN code identity plus target capabilities/options
    translated_spirv,
}

DrawPacket {
    guest_command_sequence,
    vertex_index_resources,
    render_target_refs,
    shader_artifact_refs[],
    typed_logical_resources[],      // resolve from actual UE4 RHI context
    viewport, blend, depth, rasterizer,
}
```

The adapter does **not** have to be Déraciné-only: stage-specific original binary layout and hooking remain per-title, but GNM shader metadata and the Vulkan backend can be shared with Summer Lesson, Happy Manager, etc.

## Constraints and current evidence

A-021x–A-027 recover Déraciné's original RHI command ownership and PS4 GNM metadata decoder at `0x02F5D290`. A-028 has **no actual Déraciné shader-memory snapshot** in the supplied evidence; it only identifies an optional shadPS4 host-side collection point and independently tests a metadata parser on synthetic examples. No real title shader usage frequency or native-renderer correctness is established.

This is a boundary decision, not a claim that direct renderer replacement is currently feasible for every draw. Keep the original GNM path as a temporary fallback/test oracle until equivalence is measured, then remove it from the native rendering path.
