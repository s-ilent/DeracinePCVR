# RHI-04R2: descriptor identity, array completeness and upload coverage

## Status

**`READY_FOR_SECOND_CAPTURE_WITH_EXPLICIT_LIMITATIONS`**

Four bounded defects, all confirmed in source, all repaired and covered by tests. The corrected
emulator builds and the full offline suite passes. It has not been run.

| Item | Value |
| --- | --- |
| Branch | `research/rhi04-first-original-capture-20261125` |
| Corrected emulator SHA-256 | `831B01A24BF6D470F0B8C39C6A0970E560B2D8B74DE2A9F475322542A1984B0E` |
| Size | 77,542,912 bytes |
| C++ tests | 335 across eighteen suites |
| Provenance cases | 39 |
| GPU mutation cases | 13, enforced |
| Vulkan validation errors | 0 |

## 1. Defect: the interface capture used a per-stage binding counter

`GraphicsPipeline::BuildDescSetLayout` allocates one counter across every stage:

```cpp
u32 binding{};
for (const auto* stage : stages) {
    for (const auto& buffer : stage->buffers) {
        bindings.push_back({.binding = binding++, ...});
    }
    for (const auto& image : stage->images) {
        bindings.push_back({.binding = binding, .descriptorCount = num_bindings, ...});
        binding += num_bindings;
    }
    for (const auto& sampler : stage->samplers) {
        bindings.push_back({.binding = binding++, ...});
    }
}
```

The interface capture reset to zero per stage:

```cpp
u32 binding = 0;
const auto add_descriptor = [&](...) {
    descriptor.binding = binding;
    binding += count;
};
```

`Rasterizer::BindBuffers` and `BindTextures` use the global counter, because they write against the
layout. So the captured interface named the wrong binding for every stage after the first.

### The stage order is not the obvious one

`LogicalStage` is declared Fragment, TessellationControl, TessellationEval, Vertex, Geometry,
Compute. Fragment is 0 and Vertex is 3, so the layout allocates the fragment stage's descriptors
first. A vertex stage's first buffer is therefore at a higher binding than a fragment stage's, which
is the opposite of what a reader would assume from the names.

The RHI-04 log confirms it: stage 0 bindings run 0 through 13, and stage 3 bindings run 0 through 19
with the same numbers appearing in both. Both stages genuinely use binding 0 for their own first
descriptor, and only the layout knows they are different bindings.

### Repair

The capture now records both identities:

| Field | Meaning |
| --- | --- |
| `local_binding` | the shader's own declaration order, from zero |
| `binding` | the effective Vulkan binding |
| `binding_resolved` | whether the effective number was established |
| `stage_binding_base` | the global binding the layout started this stage at |
| `stage_binding_base_known` | whether that base was established |

`ResolveGlobalBindings` is called from `BuildDescSetLayout` once the full layout is known, with the
layout's own stage order and per-stage counts. It rewrites each retained interface's descriptors.

A stage the layout did not allocate is left unresolved rather than guessed. A stage whose captured
count disagrees with the layout's count causes the whole resolution to fail, because the two then
describe different interfaces and no number can be trusted.

**A bug in my own first attempt.** The resolution indexed `stage_counts` by logical stage, but that
array is positional. Every test failed until it was keyed by stage. The tests caught it; without them
the fix would have silently done nothing in production.

## 2. Defect: sampler identity was not captured

`DrawPackage::Sampler` had no binding number, and the validator accepted any sampler with a nonzero
id:

```cpp
for (const auto& sampler : package.samplers) {
    if (sampler.id != 0) { resolved = true; break; }
}
```

An unrelated sampler from another stage or draw would satisfy the declaration.

### Repair

The production descriptor write's own `dstBinding` is now captured, after the counter advanced rather
than by predicting it. The identity travels through `SamplerRecord`, the exporter, the JSON, the
package reader and the validator.

A sampler now resolves only when it has a binding, that binding equals the descriptor's, and its id
is nonzero. A sampler with state but no binding identity is refused, because it cannot be shown to
occupy the declaration.

## 3. Defect: image arrays were not checked element by element

The previous test declared a two-element array, supplied one texture, and expected success. It
documented the gap rather than closing it.

### Repair

A descriptor with `count = N` occupies N consecutive bindings, one per array element. Every element
from 0 to N-1 must have a texture at its own binding with content.

This respects Vulkan's distinction between a binding and its array elements. One texture with
several mip levels is not an array of several elements. A duplicated element cannot stand in for a
missing one, because the check is per binding.

The permissive test is now a genuine negative: it asserts the refusal, then adds the second element
and asserts acceptance.

## 4. Defect: the large-transfer upload path recorded nothing

`WriteDataBuffer` has two branches. The ordinary one copies into the mapped staging buffer; the large
one copies into a temporary host buffer. The RHI-04R repair added `NoteHostUpload` only to the
ordinary branch.

### Repair

The large branch now records the upload too, against the **destination** buffer rather than the
temporary source. The descriptor the draw reads names the destination, and the temporary buffer is
destroyed after the copy, so its handle would never match a binding.

### Upload-path coverage

| Path | Records an upload | Notes |
| --- | --- | --- |
| `ObtainBuffer` stream buffer, small read-only | yes | already recorded |
| `WriteDataBuffer` ordinary staging | yes | repaired in RHI-04R |
| `WriteDataBuffer` large temporary | yes | repaired here |
| Isolated read-constant | yes, direct | `RetainHostBytes`, no upload record to look up |
| GPU-modified resources | no | reported as unsupported, not as verified |

The isolated path has no upload to match, so it retains the bytes it already holds through a direct
entry point. That is not a second registry: it writes into the same bounded pool the upload path
uses.

## 5. Readiness chain

The chain is now explicit, and no stage implies a later one:

1. Shader interface observed.
2. Effective module identified.
3. Vulkan descriptor write observed.
4. Correct binding identity established.
5. Resource content captured.
6. Exact content version retained.
7. Resource package internally complete.
8. Original draw identity verified.

`shader_buffers_ready` requires every binding to name a resource carrying verified bytes. A package
claiming original-fidelity readiness is refused when a required descriptor is absent, its binding
identity is unresolved, an image-array element is missing, a sampler is at the wrong binding, a
buffer has no retained content, or a descriptor class is unknown.

## 6. Compatibility

Schema 4 is unchanged. The new fields are additive and default to "not established", so a package
written before this change parses but fails original-fidelity verification rather than being
silently reinterpreted. The RHI-02I-R3 session and provenance contract is untouched.

## 7. Regression results

| Suite | Tests |
| --- | --- |
| Draw identity | 24 |
| Draw bounds | 8 |
| Semantic tape | 79 |
| Draw package | 49 |
| Index bridge | 16 |
| Capture session | 10 |
| Texture accounting | 10 |
| SPIR-V interface | 12 |
| Slot predicate | 13 |
| Bridge production | 16 |
| Package reader | 10 |
| Reference compare | 17 |
| Reference oracle | 8 |
| Pipeline presence | 7 |
| Capture artifacts | 16 |
| Producer readiness | 12 |
| Descriptor domain | 20 |
| Global binding | 8 |
| **Total** | **335** |

Plus 20 selection cases, 19 integration cases and 13 enforced GPU mutation cases, with zero
validation errors.

### Negative controls

| Control | Result |
| --- | --- |
| A count disagreeing with the layout | resolution refused |
| A stage the layout did not allocate | left unresolved, not guessed |
| A sampler at the wrong binding | refused |
| A sampler with no binding identity | refused |
| A duplicated array element | refused |
| An array element with no content | refused |
| One missing descriptor class | whole package refused |

## 8. What remains runtime-only

None of this is confirmed against the game. The second run must establish:

- whether `ResolveGlobalBindings` succeeds against real pipelines, or reports a disagreement;
- whether the resolved bindings match what `BindBuffers` and `BindTextures` actually wrote;
- whether the large-transfer path is reached for the draws that matter;
- whether any draw has a complete set of buffer, image and sampler resources;
- whether image descriptors are populated by `TextureCapture` for these draws;
- whether a package is then written, and a tape for its selected provenance.

## 9. Second-run acceptance criteria

The run is informative if it establishes any of these, in order:

1. `ResolveGlobalBindings` did not report a layout disagreement.
2. `NoteHostUpload` was called, with a nonzero count.
3. At least one draw has every declared descriptor resolved with retained bytes.
4. A package is written, or a precise refusal names the first missing resource class.
5. If a package is written, a tape is produced for its selected provenance.
6. If both exist, the identity gate runs and reports its outcome.

Successful original-shader rendering is not a prerequisite for the run being informative.

## 10. Limitations carried forward

| Limitation | Consequence |
| --- | --- |
| Colour attachment format is a GNM packed format | the replay renders into its own RGBA8 target |
| Blend factors and operations are not captured | a package with blend enable is refused |
| Effective shader state at the draw boundary is not recovered | the modules are captured, the selection is not |
| Texture and sampler descriptor writers are not fully established | some bindings may remain unresolved |
| Render targets are substituted | the diagnostic target is the replay's |

The second launch requires fresh explicit authorization.
