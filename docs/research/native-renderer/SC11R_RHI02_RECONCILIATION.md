# SC-11R to RHI-02 reconciliation

**Date:** 2026-11-24
**SC-11R branch:** `research/sc11r-original-shader-execution-20261124`
**RHI-02 branch:** `research/rhi02-semantic-resource-integration-20261124` at `78f2db67`
**Common ancestor:** `da969d4a` (SC-10S)

Both branches descend from SC-10S and neither contains the other. This document is a plan, not a
merge. No integration has been performed.

## 1. What each branch owns

| Concern | SC-11R | RHI-02 |
| --- | --- | --- |
| Package schema | 4 | 2 (its own reader) |
| Package reader | inline in each tool | `package_reader.h/.cpp`, shared |
| Draw identity | not addressed | `draw_identity.h/.cpp`, tape-versus-package join |
| Semantic tape | not present | `tape.h/.cpp`, `tape_replay.cpp` |
| Textured replay | `textured_replay.cpp` (inherited) | `textured_replay.cpp` (inherited) |
| Original shader binaries | `shader_binary_capture` | absent |
| Descriptor interface | `shader_interface_capture` | absent |
| SPIR-V verifier | `spirv_interface.h/.cpp` | absent |
| Original-shader pipeline | `original_shader_pipeline.cpp` | absent |
| Reference output oracle | `reference_output` | absent |

The two branches are complementary rather than conflicting. RHI-02 owns the semantic-tape and
draw-identity layer; SC-11R owns the original-shader execution layer.

## 2. Schema compatibility

RHI-02's reader accepts schema 2. SC-11R writes schema 4. A schema 4 package is refused by RHI-02's
reader, and a schema 2 package is refused by SC-11R's validator.

**Resolution:** RHI-02's `package_reader` should become the single reader, extended to schema 4.
The fields it needs already exist in schema 4; the additions are `shader_stages`,
`shader_interfaces`, `pipeline_state` and `reference_output`, none of which RHI-02's diagnostic
paths read. A reader that ignores unknown fields would work unchanged, but the explicit version
check would refuse it, so the version constant must be raised and the new fields parsed.

## 3. Shared package-reader ownership

SC-11R currently has three independent readers: `original_shader_replay.cpp`,
`original_shader_pipeline.cpp` (which does not read packages yet), and the inherited
`textured_replay.cpp`. RHI-02 has one, `package_reader`.

**Resolution:** adopt RHI-02's `package_reader` as the only reader. SC-11R's tools should link it
rather than carry their own. This is the change that removes the duplication the task warns about.

## 4. Semantic versus package-derived draw arguments

RHI-02 resolves draw arguments from the semantic tape and cross-checks them against the package via
`draw_identity`. SC-11R's original-shader path takes draw arguments from the package's
`index_binding`.

**Resolution:** the original-shader pipeline should take its draw arguments from the tape when one is
present, and use the package's `index_binding` only as the consistency check. That is exactly what
`draw_identity` already does for the diagnostic path, so the original-shader path should reuse it
rather than introduce a second rule.

## 5. Original-shader stage identity

SC-11R identifies a stage by `(program_hash, permutation_index)`, with `permutation_hash` as the
lookup key. RHI-02 has no concept of a shader stage identity.

**Resolution:** the stage identity is additive. RHI-02's `Package` gains
`std::vector<ShaderStage>` and `std::vector<ShaderInterface>`; nothing existing changes meaning.

## 6. Resource identity mapping

SC-11R's descriptor interface refers to guest shader-resource indices
(`guest_resource_index`), and the package's `shader_buffers[].vk_binding` is the host binding.
RHI-02's `draw_identity` already maps tape resource ids to package resource refs.

**Resolution:** the descriptor interface's `binding` is the host binding, which is what a pipeline
layout needs. The `guest_resource_index` is provenance, not identity. No new mapping is required;
the existing `vk_binding` correspondence is sufficient.

## 7. Pipeline-state ownership

SC-11R captures `pipeline_state` with an explicit `has_topology` presence flag. RHI-02's reader has
no pipeline state.

**Resolution:** additive. RHI-02 gains the field; its diagnostic paths continue to use their own
fixed state.

## 8. Existing diagnostic shader fallback

RHI-02's replay uses diagnostic GLSL shaders. SC-11R adds an original-shader path.

**Resolution:** keep both, and keep them distinguishable. The diagnostic path must never claim
original-shader readiness, and the original-shader path must refuse a package whose
`original_shader_replay_ready` is false. The readiness flags already enforce this.

## 9. Readiness and validation

SC-11R's validator has six original-shader dimensions and a descriptor-aware
`shader_bindings_ready`. RHI-02's validator has the SC-10S dimensions.

**Resolution:** the union. RHI-02's validator should adopt SC-11R's original-shader checks; they are
strictly additive and do not weaken the existing ones.

## 10. Concrete integration sequence

1. Raise RHI-02's `package_reader` to schema 4 and parse the four new blocks.
2. Delete SC-11R's inline readers; link `package_reader` instead.
3. Move `spirv_interface.h/.cpp` and `original_shader_pipeline.cpp` into RHI-02's
   `tools/geometry_replay/`.
4. Have the original-shader pipeline take draw arguments from `draw_identity`'s tape path.
5. Adopt SC-11R's validator checks in RHI-02's validator.
6. Re-run RHI-02's 58 adversarial cases and SC-11R's 62 against the merged validator.

## 11. Files involved

| File | Branch | Action |
| --- | --- | --- |
| `tools/geometry_replay/package_reader.h/.cpp` | RHI-02 | extend to schema 4 |
| `tools/geometry_replay/draw_identity.h/.cpp` | RHI-02 | reuse for original-shader draw args |
| `tools/geometry_replay/tape.h/.cpp` | RHI-02 | unchanged |
| `tools/geometry_replay/spirv_interface.h/.cpp` | SC-11R | move to RHI-02 |
| `tools/geometry_replay/original_shader_pipeline.cpp` | SC-11R | move to RHI-02 |
| `src/src/core/shader_interface_capture.*` | SC-11R | unchanged |
| `src/src/core/shader_binary_capture.*` | SC-11R | unchanged |
| `src/src/core/reference_output.*` | SC-11R | unchanged |
| `src/src/core/draw_package.h` | SC-11R | schema 4 is the merge target |

## 12. Identity vocabulary

The task asks that these stay distinct. They do, and the package keeps them in separate fields:

| Identity | Where it lives | What it is |
| --- | --- | --- |
| Original UE4 shader/resource identity | not recovered | the game's own object identity |
| Original GCN program identity | `program_hash` | the guest program's own hash |
| shadPS4 permutation identity | `permutation_hash` | `HashCombine(program_hash, perm_idx)` |
| Effective SPIR-V module identity | `effective_sha256` | the bytes actually compiled |
| Vulkan descriptor interface | `shader_interfaces[]` | the layout the module requires |

**The permutation hash is not an original-game shader identifier.** It is a shadPS4-internal value
derived from the program hash and a compilation-order index. A-028R established that the GNM
metadata carries the original program identity; the permutation index is an artefact of this
emulator's specialization strategy and has no counterpart in the game.

## 13. What is not authorised

No merge into RHI-02. No merge into `main`. The integration above requires separate authorisation.
