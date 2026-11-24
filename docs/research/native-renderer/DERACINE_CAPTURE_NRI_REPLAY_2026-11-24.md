# Déraciné Capture Replay Through Native NRI

**Date:** 2026-11-24  
**Branch:** `research/native-nri-textured-descriptors-20261124`  
**Base:** `eb06ae8f` (`research/native-nri-p0-corrections-20261124`)  
**Status:** Real captured Déraciné geometry and textures render through NRI.

---

## 1. Executive verdict

**A real Déraciné classroom mesh, sampling a real Déraciné BC3 texture, now renders through the native NRI renderer.**

The decisive numbers, against the existing standalone Vulkan reference:

| Workload | Coverage (NRI) | Coverage (reference) | Differing channel samples | Max delta |
|---|---|---|---|---|
| SC-09H classroom mesh | 79528 px (19.42%) | 79528 px (19.42%) | 1 of 1,228,800 | 1 |
| SC-10S textured draw | 29197 px (7.13%) | 29196 px (7.13%) | 12 of 1,228,800 | 6 |
| RHI-01R tape-driven draw | 79528 px (19.42%) | 79528 px (19.42%) | 1 of 1,228,800 | 1 |

The tape-driven replay is **byte-identical** to the package replay.

### 1.1 What is and is not claimed

**Claimed:** captured geometry, captured indices, captured texture bytes and captured sampler state execute through NRI and produce the same image as the reference replay.

**Not claimed:** agreement with the original game's material rendering. The shaders are diagnostic substitutes, the camera is diagnostic, and the original shaders do not execute. Section 7 states exactly what blocks that.

---

## 2. Capture artifacts consumed

All packages were found on disk and validated with the existing validators before use.

| Package | Provenance | Indices | Triangles | Streams | Texture | SHA-256 (package.json) |
|---|---|---|---|---|---|---|
| `_build/rhi01-tape` | 82 | 3036 | 1012 | 5 | none | `E162D97D67E0D8EB…` |
| `_build/sc10s-package` | 40 | 1101 | 367 | 4 | 64×64 BC3 sRGB | `FD72904FAF02BE93…` |
| `_build/sc10s-ab-package` | 23 | 1101 | 367 | 4 | 64×64 BC3 sRGB | `9678E6A6405A50DA…` |

Validator output:

```
rhi01-tape:      VALID schema=2 provenance=82 resources=11 vertex_bindings=5 indexed=True
                 bytes=50040 three_d=True triangles=1012 diagnostic_ready=True
sc10s-package:   VALID schema=2 provenance=40 resources=10 vertex_bindings=4 indexed=True
                 bytes=56638 three_d=True triangles=367 textures=1 samplers=128 textured_ready=True
sc10s-ab-package VALID schema=2 provenance=23 ... textured_ready=True
```

### 2.1 The packages describe different draws

This was checked rather than assumed. SC-09H and SC-10S are **not the same draw**:

| | SC-09H | SC-10S |
|---|---|---|
| Provenance | 82 | 40 / 23 |
| Index count | 3036 | 1101 |
| Triangles | 1012 | 367 |
| Vertex streams | 5 | 4 |
| Texture | none | 64×64 BC3 sRGB |

They are replayed separately and never joined. The report's provenance-43 figure from SC-09H does not match the package on disk (82), which the SC-09H report itself warned about: ids shift between runs.

Two other packages were rejected by the validator and not used: `sc10r-package` ("mip 1 layer 0 is missing") and `sc10-package` ("texture 1 has no block geometry"). These are the defective captures the task warned about.

---

## 3. Architecture

```
  capture package (JSON + blobs)
        │
        ├──► replay::Package  ──► geometry, indices, texture bytes, sampler
        │
        └──► tape::Tape       ──► draw arguments, command ordering
                    │
                    ▼
            tape::CommandSink
                    │
                    ▼
              semantic_rhi.h
                    │
                    ▼
              NRI backend
                    │
                    ▼
              Vulkan / GPU
```

Three executables:

| Executable | Input | Purpose |
|---|---|---|
| `deracine_nri_replay` | package | Renders captured geometry and texture directly |
| `deracine_nri_tape` | package + tape | Drives the same render through `tape::CommandSink` |
| (existing) `deracine_scene_replay` | package | The standalone Vulkan reference, unchanged |

---
## 4. Backend capabilities added, and why

Every addition below was demanded by a captured workload. None is speculative API expansion.

| Addition | Demanded by |
|---|---|
| `R32G32B32_SFLOAT` vertex format | SC-09H and SC-10S position stream (vk_format 106) |
| `R16G16B16A16_SFLOAT` vertex format | SC-10S UV stream (vk_format 97) |
| `BC3_UNORM` / `BC3_SRGB` textures | SC-10S captured texture (image_format 138) |
| Depth attachment with `LESS_EQUAL` | Both captures need depth; the reference uses D32 + LESS_OR_EQUAL |
| Explicit depth transition | Validation rejected an attachment in `UNDEFINED` layout |
| Sampled view kind, sampler, descriptor set | SC-10S binds a texture through a descriptor set |
| Texture upload via `HOST_TRANSFER` | NRI has no transfer-source/destination usage bit |

### 4.1 Texture upload

NRI's `UploadHostMemoryToTexture` copies host memory directly into a texture. The texture must be in `GENERAL` layout with `HOST_WRITE` access, so the backend:

1. transitions the texture to `GENERAL` / `HOST_WRITE` and waits for that submission;
2. calls `UploadHostMemoryToTexture` with the captured row pitch;
3. transitions to `SHADER_RESOURCE` for the fragment stage and waits.

The captured BC3 bytes are uploaded as-is. They are not decoded to RGBA8, because the captured payload is already exactly what the shader sampled.

---

## 5. Diagnostic substitutions

Every substitution is printed at runtime and listed in the report. None is presented as recovered game state.

| Substitution | Reason |
|---|---|
| Camera and MVP | The capture carries no transform. Diagnostic perspective + look-at, matching the reference. |
| Vertex shader | Diagnostic: colour by object-space position, matching `scene.vert`. |
| Fragment shader (untextured) | Diagnostic: pass through the position colour, matching `scene.frag`. |
| Fragment shader (textured) | Diagnostic: sample the captured texture, alpha forced opaque, matching `tex.frag`. |
| Alpha forced opaque | The captured texture is a mask whose alpha is mostly zero; a transparent diagnostic would show nothing. |
| Vertex stream normalisation | Position and UV are interleaved into one diagnostic stream. The captured bytes are read at their captured stride and offset. |
| Object-space centring and scale | `centre` and `scale = 2/extent` from the captured bounds, matching the reference. |

### 5.1 Tape-specific substitutions

The RHI-01R tape carries only `SetRenderTargets` and `DrawIndexed`. The sink establishes the rest on demand and names it:

```
- render target: supplied by the caller, not carried by the tape
- render target ids from the tape are not mapped to backend objects
- vertex buffer bindings: package geometry used, tape bindings are tentative
- viewport: diagnostic full-target viewport, the tape carries none
- scissor: diagnostic full-target scissor, the tape carries none
- index buffer: package index buffer, the tape carries no bind operation
- graphics pipeline: diagnostic pipeline, the tape carries no pipeline object
```

The tape's own vertex-buffer bindings are marked `tentative` by the exporter, with the note "stream-source payload is an interface pointer, not a resource base". The package's validated bindings are used instead, and that is stated.

22 opaque operations are reported individually rather than skipped silently.

---

## 6. Comparison against the reference

Both paths use identical input packages, identical diagnostic transforms, identical shaders and identical output format (640×640 PPM).

### 6.1 Geometry and draw arguments

| Item | Reference | NRI |
|---|---|---|
| Vertices (SC-09H) | 593 | 593 |
| Indices | 3036 | 3036 |
| Instance count | 2 | 2 |
| Tape draw | index_count=3036, first_index=0, vertex_offset=0, instance_count=2 | identical |

### 6.2 Pixel differences

SC-09H, after matching the reference's row order:

```
differing channel samples: 1 of 1228800 (0.0001%)
max channel delta: 1   mean: 0.000001
```

SC-10S textured:

```
differing=12 (0.0010%) max=6 mean=0.0000
ref coverage=29196  nri coverage=29197
```

The residual differences are single-LSB rounding at triangle edges and, for the textured case, BC3 block-decode rounding. They are not geometry or sampling differences: coverage matches to within one pixel.

### 6.3 A row-order difference worth recording

The first comparison showed 21% of channel samples differing with a mean delta of 12.9. The cause was not rendering: the reference's readback rows are bottom-up relative to ours. Testing the four flip hypotheses resolved it decisively:

```
direct: differing=260760 (21.2207%) mean=12.9376
yflip:  differing=1      (0.0001%) mean=0.0000
xflip:  differing=301580 (24.5426%) mean=18.7583
both:   differing=282966 (23.0278%) mean=14.4226
```

The NRI replay now writes rows in the reference's order so a byte diff is meaningful.

---

## 7. The original-shader execution blocker

### 7.1 What the compiler needs

shadPS4's GCN-to-SPIR-V compiler is reachable independently of its PM4 renderer:

```cpp
IR::Program TranslateProgram(const std::span<const u32>& code, Pools& pools,
                             Info& info, RuntimeInfo& runtime_info,
                             const Profile& profile);
```

It takes shader instruction words and three metadata structures. `Profile` is host capability data and can be filled from the Vulkan device. `RuntimeInfo` and `Info` are the interesting ones.

### 7.2 Input inventory

| Input | Available in the capture? |
|---|---|
| Shader instruction bytes | **No** |
| Shader stage | Yes — `shader_buffers[].stage` (0 = fragment, 3 = vertex) |
| Shader hashes | Yes — `pipeline.vs_hash`, `pipeline.ps_hash` |
| Resource descriptors (sharp slots) | Yes — `sharp_idx`, `vk_binding`, `vk_offset`, `vk_range` |
| Shader user data / buffers | Yes — `shader_buffers[]` with verified host content |
| Vertex input layout | Yes — `vertex_bindings[]` with format, stride, offset, semantic |
| Texture and sampler state | Yes — `textures[]`, `samplers[]` with verified association |
| `RuntimeInfo` (stage-specific registers) | **No** |
| `Info` (resource binding layout) | Partially — bindings are captured, but not in the compiler's structure |
| Specialization state | **No** |

### 7.3 The blocker, stated precisely

**The capture packages carry shader hashes and binding metadata, but not the shader instruction bytes.** `TranslateProgram` cannot be called without them.

This is not a compiler limitation. The compiler is present, self-contained and reusable. The missing input is the shader binary itself.

### 7.4 Shortest path

1. Extend the existing capture harness to retain the shader code blob for the selected draw. The harness already resolves the shader object to compute `vs_hash`/`ps_hash`, so it has the address; it does not currently retain the bytes.

2. Capture the stage-specific `RuntimeInfo` registers alongside it.

3. Call `TranslateProgram` with the captured code and a `Profile` filled from the Vulkan device.

4. Feed the resulting SPIR-V into the existing `CreateShader` path.

No new translator is needed. Steps 1 and 2 are capture-side work, not renderer work.

**No compilation experiment was attempted**, because the required instruction bytes are absent from every package on disk. Claiming otherwise would be false.

---

## 8. Bugs found and fixed

Four real defects surfaced during this work. Each was caught by a test or by the validation layer, not by inspection alone.

### 8.1 The vertex loop kept only the Z axis

**Symptom:** the replay reported 0 px coverage while the reference reported 19.42%.

**Cause:** the position vector was reset inside the component loop:

```cpp
for (int c = 0; c < 3; ++c) {
    out.x = 0; out.y = 0; out.z = 0;   // resets every iteration
    (&out.x)[c] = (value - centre[c]) * scale;
}
```

Only the last component survived, so every vertex collapsed onto the Z axis.

**Fix:** accumulate into a local array, then assign once.

### 8.2 The projection matrix had the wrong Y sign

`out.m[5] = -f` where the reference uses `out.m[5] = f`. Corrected to match.

### 8.3 A depth attachment in `UNDEFINED` layout

```
VUID-vkCmdBeginRendering-pRenderingInfo-09588: expects DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
current layout is UNDEFINED.
```

A depth image needs an explicit transition before it is used as an attachment, exactly as a colour image does.

### 8.4 The tape carries no viewport, scissor or index-buffer bind

```
VUID-vkCmdDrawIndexed-scissorCount-03418: never called vkCmdSetScissorWithCount
VUID-vkCmdDrawIndexed-None-07312: no vkCmdBindIndexBuffer call
```

The sink now establishes that state on demand and reports each one as a substitution.

---

## 9. Regression suite

All 14 synthetic tests still pass, unchanged:

```
t01_bootstrap                    15    t08_lifetime_gpu                 13
t02_external_image               17    t09_stale_resource_handles       18
t03_depth_stencil_aspects        12    t10_recording_lifetime           19
t04_stereo                        9    t11_multi_submission             12
t05_submission_lifetime          15    t12_expected_diagnostic_scope     7
t06_semantic_nri_draw            17    t13_p0_lifetime_corrections      25
t07_resource_registry            38    t14_textured_nri_draw            17
```

**234 assertions, zero failures, zero unexpected validation errors.**

The red/blue indexed stereo renderer is unchanged: layer 0 red, layer 1 blue, corner untouched.

### 9.1 Replay results

| Executable | Package | Assertions | Coverage | Validation |
|---|---|---|---|---|
| `deracine_nri_replay` | rhi01-tape | 10 passed | 79528 px | clean |
| `deracine_nri_replay` | sc10s-package | 12 passed | 29197 px | clean |
| `deracine_nri_replay` | sc10s-ab-package | 12 passed | 29197 px | clean |
| `deracine_nri_tape` | rhi01-tape | 8 passed | 79528 px | clean |

---

## 10. Remaining limitations

1. **Original shaders do not execute.** The capture packages lack shader instruction bytes. Section 7 gives the exact path.

2. **The tape is partial.** It carries two operation kinds of nine. Viewport, scissor, index bind, pipeline and frame boundaries are substitutions.

3. **Tape vertex bindings are tentative.** The exporter marks them so; the package's validated bindings are used instead.

4. **No material state.** No blend, no depth-stencil state from the game, no render-target format from the game.

5. **Single draw.** One draw per run. The tape's 22 opaque operations are reported, not executed.

6. **SC-10S has no matching semantic tape.** Its replay uses the package adapter path, which is the documented intermediate integration. The two are not joined.

7. **Provenance ids shift between runs.** The on-disk SC-09H package is provenance 82, not the report's 43. Identity was verified by matching tape and package, not by trusting the id.

---

## 11. Interface questions for the RHI-recovery track

These are the descriptor-abstraction questions that only the recovered UE4 binding semantics can answer.

1. **Is one descriptor set per draw correct, or does UE4 bind sets independently of draws?** The current API creates an immutable set per binding group. If UE4 rebinds a set across many draws, per-draw creation is wrong.

2. **How many sampled textures and samplers per set?** The current API is exactly one of each. The SC-10S package has one texture but 128 captured samplers, which suggests sampler state is shared and rebound rather than baked into a set.

3. **Are sampler and texture bound together or separately?** UE4's `FShaderResourceViewRHIRef` and `FSamplerStateRHIRef` are distinct objects. The current API couples them in one `DescriptorSetDesc`.

4. **What is the register-space convention?** The backend uses one set at index 0 with bindings 0 and 1. The captured `vk_binding` values are 0, 1, 2, 4, 5, 6, 7 — a wider layout than the current API can express.

5. **Does UE4 use uniform buffers for material parameters?** The current API has only a 64-byte root-constant block. The captured `shader_buffers` include 2848-byte ranges, which are uniform buffers, not push constants.

6. **Is descriptor update ever required?** The current API makes sets immutable after creation. If UE4 updates a bound set, that model is too strict.

7. **How are vertex streams declared?** The current API takes one interleaved stream. SC-09H has five separate streams with different strides and formats.

---

## 12. Reproduction

```
# Build (NRI pinned at 06ff6f643777e72eda88bc6fb8d56267b85d872f)
cd native-renderer
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DNRI_ROOT=<path>/_deps/NRI \
  -DJSON_INCLUDE_DIR=<repo>/src/externals/json/single_include
cmake --build build

# Replay a captured package
./build/deracine_nri_replay <package-dir>
./build/deracine_nri_tape   <package-dir-with-tape>
```

Packages used: `_build/rhi01-tape`, `_build/sc10s-package`, `_build/sc10s-ab-package`. They contain proprietary game resource contents and are not committed.

---

## 13. Summary

**Working:** a real Déraciné classroom mesh and a real Déraciné BC3 texture render through the semantic RHI and NRI, matching the standalone Vulkan reference to within one channel sample for geometry and twelve for the textured case. The semantic tape drives the same draw through the existing `tape::CommandSink` contract, byte-identically to the package replay.

**Added:** only the backend capabilities the captures demanded — three-component float and half-float vertex formats, BC3 textures, depth attachments, and the descriptor path.

**Blocked:** original shader execution, on missing shader instruction bytes in the capture packages. The compiler itself is present and reusable.

**Not claimed:** agreement with the game's material rendering, or that the tape is complete.

