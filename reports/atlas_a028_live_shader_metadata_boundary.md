# Binary Atlas A-028 — The already-existing shader trailer capture boundary

**Status:** Infrastructure/boundary finding with read-only static ELF and shadPS4 source verification, plus a tested offline parser. **Not** a validation of actual Déraciné shader instances. A genuine runtime shader-memory sample is unavailable in the materials provided in this session, so the original A-028 validation objective is **not yet satisfied**.

## Original-target evidence

- Déraciné original PS4 1.00 `eboot.bin`: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, rehashed independently this pass. Full raw-byte scan for ASCII `OrbShdr` yields **zero occurrences**. This does not exclude shaders loaded from separate archives or constructed dynamically.
- The available `Deracine-B3-Evidence.zip` contains six RHI probe/preflight/postflight logs plus an address map, but **no shader binaries**. The GitHub `research/sc09-shader-params-20261123` tree contains source and SC-09 measurements, but no committed raw GNM shader-memory snapshots.
- Original SELF decoder at inner ELF VA `0x02F5D290` obtains a stage-derived program address using two words (`[stage+0x08] << 8` and `[stage+0x0C] << 40`), low 23 bits from `[stage+0x00]`, then locates the trailing `GnmShaderBinaryInfo` at `program + (stage_word_0 & 0x7FFFFF) - 0x1C` (A-027). It reads the trailer's `chunk_usage_base_offset_in_dw` and `num_input_usage_slots` at offsets +0x0C/+0x0D, and the 4-byte `GnmInputUsageSlot` array at `info - 4 * (chunk_offset + count)`. **This is the actual static original-game formula**, not a guess based on documentation.

## New cross-source finding: existing host-side access

The **SC-09 shadPS4 fork already reads `OrbShdr` from original guest GPU shader memory**, without an additional PS4 command observer:

- `src/src/video_core/amdgpu/regs_shader.h` defines `AmdGpu::BinaryInfo` with the 7-byte `OrbShdr` signature, trailer flags, `length`, `chunk_usage_base_offset_in_dw`, `num_input_usage_slots`, shader hash and CRC fields.
- In the same header, `SearchBinaryInfo(code)` tests an optional shader marker (`code[0] == 0xBEEB03FF`), otherwise searches up to 0x4000 shader words for the signature and returns a **reference to the actual trailer**.
- `GetParams(sh)` calls `SearchBinaryInfo` and returns `ShaderParams{.user_data, .code=span(code,bininfo.length/sizeof(u32)), .hash=bininfo.shader_hash}`. The hash is the one used in shader compilation and reported in the SC-09 shader-stage observations.
- `src/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` calls `AmdGpu::GetParams` when identifying stage programs; `CompileModule` uses `DumpShader(code,hash,...,"bin")`. `DumpShader` only writes the span passed by `GetParams`, **not an explicit copy of `GnmShaderBinaryInfo` and the preceding input-usage array**. Therefore ordinary `dumpShaders` output is not guaranteed to contain the original trailer/table and should not be treated as a validated input to A-027 without checking for the signature.
- Importantly, `bininfo.length` and the original stage-size field are **distinct fields**. The original decoder's formula uses the latter, while shadPS4's `GetParams()` uses the former. Do not conflate their byte counts or assert they are interchangeable without a real shader sample.

**Best runtime capture boundary:** immediately after successful `AmdGpu::SearchBinaryInfo(code)` while both the trailer pointer and shader-program code address are available. It can be instrumented entirely on the host side. Avoid patching the original Déraciné binary and avoid adding six more RHI resource-command hooks. Read-only, opt-in, once per distinct `(shader_hash, stage)` with bounded record counts and validated guest memory/protection. Since guest pointer safety is not established by a standalone Python script, apply the emulator's memory-range validation before copying.

### Suggested capture schema

```json
{
  "shader_hash": "...",
  "logical_stage": "VS|PS|CS|...",
  "metadata_version": 1,
  "trailer_valid": true,
  "chunk_base_dwords": 0,
  "num_input_usage_slots": 0,
  "srt_flags": 0,
  "usage_type_histogram": {"IMM_RESOURCE": 0, "IMM_SAMPLER": 0, "IMM_CONSTBUFFER": 0},
  "usage_slots": [
    {"usagetype": 2, "apislot": 0, "startregister": 4, "flags": 0}
  ]
}
```

The numbers above are **illustrative**, not real Déraciné samples. The `shader_hash` can be joined to SC-09's stage `pgm` hash. A stage's immediate constant-buffer map should not be confused with Vulkan buffer bindings: the shadPS4 pipeline also resolves shader instruction semantics and pointer-indirect resources.

### What a successful follow-up should prove

1. At least one genuine Déraciné `OrbShdr` footer matches the magic and lies within readable guest shader memory.
2. Captured `num_input_usage_slots` and `GnmInputUsageSlot` records obey the observed metadata bounds and use valid or explicitly unknown usage types.
3. Captured shader hash joins to a named SC-09 stage hash **in the same runtime**.
4. An immediate constant-buffer (`0x02`) declaration's `apislot/startregister` explains the matching shadPS4 `sharp_idx` (with documented transformation); if not, report the mismatch instead of forcing equivalence. Resource-table pointer types must be handled separately.
5. Report the distribution by shader and usage type. If types `0x22`–`0x25` do not occur, conclude only that they are absent from this bounded sample, not the entire game.

## Tested offline parser

`parse_gnm_usage_a028.py` parses captured guest shader-memory blobs containing the complete `OrbShdr` trailer and preceding declaration table, or checks the approved original SELF for embedded literal signatures. It supports the public PS4 GNM usage type names, preserving unknown usage codes.

- `python parse_gnm_usage_a028.py --test`: **11/11 tests passed**, using *synthetic metadata fixtures* only. They cover valid types, unknown kinds, truncation, zero slots, multiple trailing records and offsets.
- `python parse_gnm_usage_a028.py --self /mnt/data/eboot.bin`: approved SHA/size gate; **0** literal `OrbShdr` signatures.
- `python parse_gnm_usage_a028.py --file <actual full shader-memory snapshot> --json`: not exercised on a real Déraciné shader; no such snapshot supplied.

**Do not claim a real-shader test passed.** This phase verifies the collection path and decoder rather than proving a game shader's declarations.

## Cross-game impact

The `GnmInputUsageSlot` and `GnmShaderBinaryInfo` formats are GNM platform structures, not unique UE4 4.18. The source-level method `AmdGpu::SearchBinaryInfo()` in the existing shadPS4 backend is reusable for games where an `OrbShdr` trailer is present. The original Déraciné RVAs and native UE4 command ownership are not reusable verbatim.

## Source references

- Original SELF and previously submitted A-025–A-027 reports/locally verified instruction windows.
- `s-ilent/DeracinePCVR` branch `research/sc09-shader-params-20261123`, `src/src/video_core/amdgpu/regs_shader.h`, `src/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`.
- `PS4-OpenGNM/opengnm` `include/gnm_shader.h` and `include/gnm_shaderbinary.h`.
- `PS4-OpenGNM/opengnm-psbc` `tests/verify_sb.py` for an independent public shader-container validation approach.

## Next bounded work

Obtain one metadata-only host-side capture from Déraciné (no redistribution of shader code required). In the absence of such a run, independent work can audit the original program/trailer sizes and GNM `SRT`/extended-usage flags in static parser logic but cannot establish their real usage frequencies.