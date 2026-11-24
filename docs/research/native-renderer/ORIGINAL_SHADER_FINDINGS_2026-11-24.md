# Original-Shader Execution: Findings and Blocker

**Date:** 2026-11-24  
**Branch:** `research/native-nri-original-shader-20261124`  
**Base:** `1c3067a6`  
**Status:** Original shaders located and reflected. SC-11 package does not exist.

---

## 1. Executive verdict

**The original translated shaders for both captured draws exist on disk as valid SPIR-V, and their interfaces are now reflected.**

**The SC-11 execution package does not exist.** No branch, no report, no package. I did not invent one.

**The decisive blocker is narrower than a missing package.** Both draws that have complete geometry use pixel shaders that write only `gl_SampleMask` and no colour. Of the 127 colour-writing fragment shaders in the cache, only two appear in any package, and both of those packages lack geometry. There is no draw on disk that has both a colour-writing original shader and the geometry to execute it.

This means the milestone cannot be completed as specified. What I can report is exactly what exists, what it requires, and what is missing.

---

## 2. Same-draw identity: established

Phase 1 was not called complete on provenance ids alone. A verifier was written that establishes identity from the data.

`native-renderer/tools/verify_draw_identity.py` — **17 checks, 0 failed**:

```
[PASS] scene agrees  tape=classroom-title package=classroom-title
[PASS] provenance agrees  tape=82 package=82
[PASS] index count agrees  tape=3036 package=3036
[PASS] first index agrees  tape=0 package=0
[PASS] base vertex agrees  tape=0 package=0
[PASS] instance count agrees  tape=2 package=2
[PASS] first instance agrees  tape=0 package=0
[PASS] index bytes match declared count  bytes=3036 declared=3036
[PASS] tape index count matches index bytes  tape=3036 bytes=3036
[PASS] highest index inside vertex range  max=592 tape.num_vertices=593
[PASS] triangle count agrees  count/3=1012 tape.num_primitives=1012
[PASS] package triangle count agrees  count/3=1012 package=1012
[PASS] vertex stream 0 yields 593 vertices  stride=12 bytes=7116
[PASS] vertex stream 1 yields 593 vertices  stride=16 bytes=9488
[PASS] vertex stream 2 yields 593 vertices  stride=16 bytes=9488
[PASS] vertex stream 3 yields 593 vertices  stride=4 bytes=2372
[PASS] vertex stream 4 yields 593 vertices  stride=16 bytes=9488
```

The strongest check is that the tape's draw arguments are consistent with the package's **actual index bytes**: 3036 indices, highest index 592 inside a 593-vertex range, 1012 triangles. A tape describing a different draw could not satisfy all of these simultaneously.

### 2.1 What identity does NOT establish

The verifier reports these explicitly rather than glossing them:

- **Vertex-buffer resource linkage.** All 34 tape vertex bindings are marked `tentative` and record interface pointers (`0x1013…`), not resource bases (`0x201D…`). The tape cannot be used to select vertex data.

- **Shader permutation identity.** The tape carries no shader identity at all.

SC-10S has no tape, so no tape identity question arises there.

---

## 3. The original shaders exist

shadPS4's shader cache at `pc-vr/user/cache/CUSA11919` holds **280 `.spv` and 280 `.meta`** files.

The cache names them differently:

| File | Named by | Source |
|---|---|---|
| `{pgm_hash}_{perm}.spv` | program hash | `RegisterShaderBinary` |
| `{perm_hash}.meta` | permutation hash | `RegisterShaderMeta` |

The draw packages record `vs_hash`/`ps_hash`, which are **permutation** hashes. So a direct filename lookup finds the `.meta` and no `.spv`. That is why the first search appeared to fail.

The `.spv` name is recoverable because `Shader::InfoPersistent` begins with `pgm_hash`, and `Info::Serialize` writes that struct verbatim. Scanning the meta for a value that names an existing `.spv` resolves it.

### 3.1 Resolved shaders

| Draw | Stage | Perm hash (`.meta`) | Program hash (`.spv`) | Size | spirv-val |
|---|---|---|---|---|---|
| SC-09H | vertex | `0x3eed6feab3afa088` | `0x905a5b18f401a367_0` | 15932 B | **pass** |
| SC-09H | pixel | `0x990f862abe3eed54` | `0xd373d45153639efc_0` | 4096 B | **pass** |
| SC-10S | vertex | `0x30ab35bf0f231a71` | `0x34800e33aa8cccf9_0` | 15404 B | not run |
| SC-10S | pixel | `0xb4e931930ad62d4d` | `0x439f33c0f174e540_0` | 7048 B | not run |

All four carry the SPIR-V magic `0x07230203`. The two SC-09H modules validate cleanly with `spirv-val`.

---

## 4. Reflected shader interface

### 4.1 SC-09H vertex shader

```
OpEntryPoint Vertex %75 "main" %push_data %gl_VertexIndex %48 %gl_InstanceIndex
  %vs_in_attr0..4 %gl_Position %gl_ViewportIndex %out_attr0..5 %ssbo_2..5
```

| Kind | Detail |
|---|---|
| Vertex inputs | `Location 0..4`, all `vec4` |
| Vertex outputs | `Location 0..5`, all `vec4` |
| Push constant | `AuxData`, 128 bytes |
| Storage buffers | `Binding 1,2,3,4` at `DescriptorSet 0`, all `NonWritable` |
| Built-ins | `gl_VertexIndex`, `gl_InstanceIndex`, `gl_Position`, `gl_ViewportIndex` |

### 4.2 SC-09H pixel shader

```
OpEntryPoint Fragment %59 "main" %push_data %gl_FragCoord %gl_SampleMask %ssbo_1
```

| Kind | Detail |
|---|---|
| Fragment inputs | `gl_FragCoord` only — **no interpolated varyings** |
| Fragment outputs | `gl_SampleMask` only — **no colour output** |
| Push constant | `AuxData`, 128 bytes |
| Storage buffers | `Binding 0` at `DescriptorSet 0` |

### 4.3 The push-constant block

Both stages declare the same `AuxData` layout:

```
Offset   0  xoffset      Offset  16  ud_regs0     Offset  80  buf_offsets0
Offset   4  yoffset      Offset  32  ud_regs1     Offset  96  buf_offsets1
Offset   8  xscale       Offset  48  ud_regs2     Offset 112  buf_offsets2
Offset  12  yscale       Offset  64  ud_regs3
```

128 bytes total. The current backend's root-constant block is 64 bytes, so it is **too small** for this shader.

---

## 5. The decisive finding: the captured draws use mask-only pixel shaders

I scanned all 280 cached SPIR-V modules. **127 of 144 fragment shaders write a colour output.** The cache is not the problem.

But the two draws that have complete geometry packages do not use any of them:

| Draw | Pixel shader | Outputs |
|---|---|---|
| SC-09H (`rhi01-tape`, `sc09h-package`) | `0xd373d45153639efc_0` | `gl_SampleMask` only |
| SC-10S (`sc10s-package`, `sc10s-ab-package`) | `0x439f33c0f174e540_0` | `gl_SampleMask` only |

Both are depth-prepass or mask shaders. Neither writes colour. The diagnostic replay's image comes from a substitute `scene.frag` that colours by object-space position.

### 5.1 Cross-referencing colour shaders against packages

Of the 128 permutation hashes whose pixel shader writes colour, exactly **two** appear in any package on disk:

| Perm hash | SPIR-V | Package | Usable? |
|---|---|---|---|
| `0x85df3d8770c6720c` | `0xa3d1bd164bf383f4_0` | `sc09r-package` | **No** — schema 1, no geometry, no index binding, no vertex bindings |
| `0xb096888e3aaa51e7` | `0x153a45928c2ef8a4_0` | `sc09g-tamper` | **No** — 48 indices, 1 vertex binding, no geometry block |

`sc09r-package` is a shader-buffer-only capture: two resources of 16 and 64 bytes, no draw geometry at all. `sc09g-tamper` is a validator fixture.

### 5.2 What this means

**There is no draw on disk that has both a colour-writing original shader and the geometry to execute it.**

This is not a renderer limitation. It is a gap in what has been captured. The pieces exist separately:

- 127 colour-writing original shaders, in the cache.

- Two complete geometry packages, with mask-only shaders.

- No package that joins them.

---

## 6. Backend gaps the real shaders expose

Read directly from the reflected modules, not speculated:

| Gap | Current state | Required by |
|---|---|---|
| Push-constant size | 64 bytes | 128 bytes (`AuxData`) |
| Descriptor layout | 1 texture + 1 sampler at set 0 | SC-09H VS: 4 storage buffers at bindings 1–4; PS: 1 at binding 0 |
| Descriptor types | texture, sampler | `StorageBuffer` |
| Vertex inputs | 1 interleaved stream | 5 separate streams, `Location 0..4`, all `vec4` |
| Fragment output | colour | `gl_SampleMask` for the captured draws |
| SC-10S PS | — | 2 storage buffers (bindings 0,1), texture (2), sampler (3) |

The SC-10S pixel shader is the richer interface and is the better target once a colour-writing variant is captured.

---

## 7. Summary

**Established:** the SC-09H tape and package describe the same draw, verified by 17 data checks including consistency with the actual index bytes.

**Located:** the original translated SPIR-V for both captured draws, resolved through the meta's `pgm_hash`. All four modules are valid SPIR-V; the two SC-09H modules pass `spirv-val`.

**Reflected:** the exact shader interfaces, including a 128-byte push-constant block and storage-buffer bindings the current backend cannot express.

**Blocked:** no draw on disk has both a colour-writing original shader and geometry. The SC-11 package does not exist.

**Not claimed:** any original-shader execution.

