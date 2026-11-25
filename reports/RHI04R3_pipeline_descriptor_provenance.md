# RHI-04R3: pipeline-scoped descriptor provenance and Vulkan array correctness

## Status

**`READY_FOR_SECOND_CAPTURE_WITH_EXPLICIT_LIMITATIONS`**

Three defects, all confirmed in source, all repaired and covered by adversarial tests. The corrected
emulator builds and the full offline suite passes. It has not been run.

| Item | Value |
| --- | --- |
| Branch | `research/rhi04-first-original-capture-20261125` |
| Corrected emulator SHA-256 | `C4AA9885A6792327BE7B3EBB6B9BCC2B144A2CD2EF5016F4F09FF964ECB5031B` |
| Size | 77,655,040 bytes |
| C++ tests | 352 across nineteen suites |
| Provenance cases | 39 |
| GPU mutation cases | 13, enforced |
| Vulkan validation errors | 0 |

## 1. Defect: descriptor resolution was permutation-scoped

`ResolveGlobalBindings` iterated every retained interface and applied whichever pipeline's binding
bases called it most recently:

```cpp
const u32 count = g_state.count.load(std::memory_order_relaxed);
for (u32 i = 0; i < count; ++i) {
    auto& interface = g_state.interfaces[i];
    ...
    interface.stage_binding_base = base[interface.logical_stage];
    descriptor.binding = interface.stage_binding_base + descriptor.local_binding;
}
```

Shader permutations are reusable. One permutation can participate in several graphics pipelines with
different stage combinations and therefore different binding bases. Writing the base back onto the
shared declaration meant a later pipeline silently corrupted an earlier draw's mapping.

### Repair

The declaration is now immutable and the mapping is its own record:

| Record | Contents |
| --- | --- |
| `Interface` | program hash, permutation, logical stage, local declarations, push constants |
| `PipelineBinding` | pipeline id, per-stage base and count, resolved flag, problem |

`ResolvePipelineBindings(pipeline_id, stages)` stores a mapping keyed by pipeline. A later pipeline
reusing the same permutations gets its own record. `ResolveDescriptorBinding` looks up by pipeline,
stage and local binding, and refuses a local binding beyond the stage's allocation rather than
letting it resolve into the next stage's range.

The pipeline identity is derived from the pipeline key's stage hashes, which is what distinguishes
two pipelines selecting different modules. It is not the permutation identity.

A stage whose declaration count disagrees with the layout fails the whole resolution, and nothing is
stored as resolved. A stage the layout did not allocate has no binding rather than binding zero.

### A bug in my own repair

The first version searched the whole pipeline array for a matching id rather than only the live
entries. After a reset, a stale slot with a colliding id was overwritten and the record landed
outside the live range, where no lookup could find it. The tests caught it. In production it would
have silently lost a pipeline's mapping after any capacity reuse.

## 2. Defect: image arrays were validated as consecutive bindings

The validator computed `element_binding = descriptor.binding + element`. That is not how Vulkan
descriptor arrays work.

The production write is:

```cpp
set_write.dstBinding = binding.unified;
set_write.dstArrayElement = 0;
set_write.descriptorCount = array_size;
set_write.pImageInfo = &image_infos[image_info_idx];
```

One binding with `array_size` elements. The counter then advances by `array_size` for the *next*
descriptor, which is a separate concern from how the elements are addressed.

### Repair

The capture records `array_element` and `array_count` from the write's own `dstBinding`,
`dstArrayElement` and `descriptorCount`. The validator matches elements within the declared binding:

```cpp
if (texture.vk_binding != descriptor.binding) continue;
if (texture.array_element != element) continue;
```

A record whose `array_count` disagrees with the declaration is refused, because that is a
disagreement between capture and layout rather than a superset.

The distinction the tests prove: binding B with elements 0 and 1 is not bindings B and B+1.

## 3. Defect: sampler deduplication discarded binding identity

`NoteSampler` deduplicated on `(guest_hash, stage_hash)`, which describes sampler state reuse rather
than descriptor-binding identity. One shader may bind identical state at several bindings, and the
second was discarded while the capture still reported itself complete.

### Repair

Deduplication now requires the whole observation to match: state, stage, draw provenance, binding,
binding presence, array element and associated image. Anything else is a distinct observation.

Capacity exhaustion increments `sampler_capacity_dropped` rather than silently dropping a binding.
A capture cannot claim completeness it does not have.

## 4. Cross-domain identity

The validator now distinguishes:

| Domain | Identity |
| --- | --- |
| Original UE4 draw | session + provenance |
| Graphics pipeline | pipeline id from the key's stage hashes |
| Shader | program hash + permutation + logical stage |
| Descriptor | pipeline + set + binding |
| Descriptor array element | descriptor identity + element |
| Buffer resource | host allocation + range + content version |
| Image resource | image/view identity + subresource + content version |
| Sampler | binding observation + sampler state |

No descriptor resolves merely because a matching binding number occurs elsewhere in the package.

## 5. Serialization compatibility

Schema 4 is unchanged. The new fields are additive and default to "not established", so a package
written before this change parses but fails original-fidelity verification rather than being
silently reinterpreted. The RHI-02I-R3 session and provenance contract is untouched.

## 6. Regression results

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
| Descriptor domain | 23 |
| Pipeline binding | 12 |
| Sampler binding | 10 |
| **Total** | **352** |

Plus 20 selection cases, 19 integration cases and 13 enforced GPU mutation cases, with zero
validation errors.

### Negative controls

| Control | Result |
| --- | --- |
| One permutation in two pipelines with different bases | both mappings kept, first not overwritten |
| A layout disagreeing with the declaration | refused, nothing stored as resolved |
| A module the capture never saw | refused |
| A local binding beyond the stage count | refused |
| Consecutive bindings presented as an array | refused |
| A duplicated array element | refused |
| An array count disagreeing with the declaration | refused |
| Identical sampler state at two bindings | two observations |
| A genuinely identical sampler observation | deduplicated |
| Capacity exhaustion | reported, not silent |

## 7. What remains runtime-only

- whether `ResolvePipelineBindings` succeeds against real pipelines, or reports a disagreement;
- whether the derived pipeline identity is stable across a run;
- whether image descriptors are populated by `TextureCapture` for the draws that matter;
- whether any draw has a complete set of buffer, image and sampler resources;
- whether a package is then written, and a tape for its selected provenance.

## 8. Second-run acceptance criteria

1. No pipeline reported a layout disagreement.
2. `NoteHostUpload` was called, with a nonzero count.
3. At least one draw has every declared descriptor resolved with retained bytes.
4. A package is written, or a precise refusal names the first missing resource class.
5. If a package is written, a tape is produced for its selected provenance.
6. If both exist, the identity gate runs and reports its outcome.

Successful original-shader rendering is not a prerequisite for the run being informative.

## 9. Limitations carried forward

| Limitation | Consequence |
| --- | --- |
| Colour attachment format is a GNM packed format | the replay renders into its own RGBA8 target |
| Blend factors and operations are not captured | a package with blend enable is refused |
| Effective shader state at the draw boundary is not recovered | the modules are captured, the selection is not |
| Texture and sampler descriptor writers are not fully established | some bindings may remain unresolved |
| Render targets are substituted | the diagnostic target is the replay's |

The second launch requires fresh explicit authorization.
