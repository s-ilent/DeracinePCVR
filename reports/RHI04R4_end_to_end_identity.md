# RHI-04R4: end-to-end capture identity repair

## Status

**`READY_FOR_SECOND_CAPTURE_WITH_EXPLICIT_LIMITATIONS`**

Four source-confirmed integration defects repaired, one audit resolved as a non-defect, and the
producer-to-consumer identity chain now verified end to end. The corrected emulator builds and the
full offline suite passes. It has not been run.

| Item | Value |
| --- | --- |
| Branch | `research/rhi04r4-end-to-end-identity-20261125` |
| Base | `01032226b99e158dbcc83fc283964592f19a4130` |
| Corrected emulator SHA-256 | `53D0CBA2A1E6D9F74070CB0C1E1DD69A0CD9B382C5D77D3E4E3596898D502F3B` |
| Size | 77,657,088 bytes |
| C++ tests | 366 across twenty suites |
| Provenance cases | 39 |
| GPU mutation cases | 13, enforced |
| Vulkan validation errors | 0 |

## 1. SC-R4: the pipeline mapping was never consumed

### The defect

`vk_pipeline_cache.cpp` recorded each descriptor's shader-local binding and set
`binding_resolved = false`. `BuildDescSetLayout` correctly built pipeline-specific mappings through
`ResolvePipelineBindings`. But the exporter copied the declaration's local number:

```cpp
entry.binding = descriptor.binding;   // the shader-local number
```

So the mapping machinery never reached the package. Worse, the two identities were not even the same
value: the resolver keyed on a FNV hash of the key's stage hashes, while the draw carried
`std::hash<GraphicsPipelineKey>{}(key)`. A mapping stored under one identity could never be found
under the other.

### The repair

The resolver now uses the exact expression the draw carries:

```cpp
const u64 pipeline_id = std::hash<GraphicsPipelineKey>{}(key);
```

The exporter resolves each descriptor through `(pipeline_id, logical_stage, local_binding)` and
records both identities:

| Field | Meaning |
| --- | --- |
| `local_binding` | the shader's own declaration order |
| `binding` | the effective Vulkan binding |
| `binding_resolved` | whether the effective number was established |
| `pipeline_id` | the pipeline whose layout resolved it |

The validator refuses any descriptor whose `binding_resolved` is false. Zero is a legitimate
binding, so the flag is the only way to distinguish "binding 0" from "never resolved".

## 2. SC-R5: array elements did not survive export

### The defects

Five separate gaps, all confirmed:

| Gap | Location |
| --- | --- |
| Only the first element of an array was recorded | `NoteImageBinding` |
| Deduplication ignored the array element | `CommitPendingImages` |
| The join key ignored the array element | `shader_param_capture.cpp` |
| The exporter did not transfer `array_element` or `array_count` | `DrawPackage::Texture` |
| The JSON writer and reader did not round-trip them | `draw_package.cpp`, `package_reader.cpp` |

### The repair

The capture records `array_element` and `array_count` from the write's own `dstBinding`,
`dstArrayElement` and `descriptorCount`. The element is part of the dedup identity, the join
identity, the package record, the JSON and the reader.

A descriptor with `descriptorCount = N` remains one binding with N elements. It never becomes
bindings B through B+N-1.

## 3. SC-R6: sampler observations crossed draw boundaries

### The defect

`NoteSampler` preserved binding and provenance during deduplication, but the exporter copied every
retained sampler regardless of which draw was being exported. A sampler observed in another draw
could satisfy the selected draw's declaration.

### The repair

The exporter now skips any sampler whose `provenance_id` differs from the selected draw's. The
package's `Sampler` carries `provenance_id`, so a consumer can check the association rather than
trusting it.

## 4. SC-R7: exact permutation identity — not a defect

The audit asked whether `FindDeclaration`'s match on program hash, stage and binding count could
confuse two permutations of one program with identical counts.

**It could, and the exact identity was available.** `GetProgram` returns
`perm_hash = HashCombine(params.hash, perm_idx)` as the key's `stage_hashes` entry, which is the same
value the interface capture records as `permutation_hash`. The match is now on that exact value, so
two permutations of one program cannot be confused regardless of their counts.

This was a real ambiguity in the code, eliminated by using an identity the producer already had. It
was not a reproducible runtime defect, because no evidence exists that Déraciné emits two
permutations of one program with identical descriptor counts.

## 5. SC-R8: unknown array counts

`texture.array_count == 0` was tolerated as agreement. Zero is the field's default, so an
uncaptured count passed as a match.

The validator now refuses an unknown count explicitly:

```
an image descriptor's captured array count is unknown, so it cannot be shown to match its declaration
```

A package written before this change parses but fails original-fidelity verification. It is not
silently upgraded.

## 6. Producer-to-consumer identity map

| Identity | Origin | Survives to validation via |
| --- | --- | --- |
| Session | `CaptureSession::TryInstall` | `session.json`, every artifact |
| Draw provenance | `RhiEntryProbe::CurrentDrawProvenanceId` | package, tape, bridge, samplers |
| Pipeline | `std::hash<GraphicsPipelineKey>{}(key)` | `ShaderInterface::pipeline_id` |
| Permutation | `HashCombine(program_hash, perm_index)` | `permutation_hash` |
| Descriptor | pipeline + set + binding | `ShaderDescriptor` |
| Array element | `dstBinding` + `dstArrayElement` + element | `Texture::array_element` |
| Buffer resource | host allocation + range + content hash | `Resource` |
| Image resource | image uid + subresource + content hash | `Texture` + `TextureSubresource` |
| Sampler | provenance + stage + binding + state | `Sampler` |

## 7. Test results

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
| End-to-end identity | 14 |
| **Total** | **366** |

Plus 20 selection cases, 19 integration cases and 13 enforced GPU mutation cases, with zero
validation errors.

### Baseline accounting

The RHI-04R3 baseline was 352 across nineteen suites. This branch adds
`shadps4_end_to_end_identity_test` with 14 cases, giving 366 across twenty. No suite was removed or
renamed.

### Negative controls

| Control | Result |
| --- | --- |
| An unresolved binding | refused |
| The local number presented as the effective one | refused |
| An array as consecutive bindings | refused |
| An unknown array count | refused |
| A mismatched array count | refused |
| A sampler at the wrong binding | refused |
| A sampler from another draw | excluded by the producer |
| One unresolved descriptor in an otherwise complete package | whole package refused |

## 8. Serialization round trip

`test_package_reader.cpp` now writes a descriptor with `local_binding = 0`, `binding = 2` and
`binding_resolved = true`, reads it back through the production reader, and asserts all three. The
round trip is exercised rather than assumed.

## 9. What remains runtime-only

- whether `ResolvePipelineBindings` succeeds against real pipelines, or reports a disagreement;
- whether the derived pipeline identity is stable across a run;
- whether Déraciné actually uses multi-element image arrays;
- whether image descriptors are populated by `TextureCapture` for the draws that matter;
- whether any draw has a complete set of buffer, image and sampler resources;
- whether a package is then written, and a tape for its selected provenance.

## 10. First unresolved invariant

**No runtime evidence exists that any draw has a complete descriptor set.** Every offline gate
passes, but the producer has never been observed writing a package from a real draw. That is the
first invariant the second capture must establish.

## 11. Second-run acceptance criteria

1. No pipeline reported a layout disagreement.
2. `NoteHostUpload` was called, with a nonzero count.
3. At least one draw has every declared descriptor resolved with retained bytes.
4. A package is written, or a precise refusal names the first missing resource class.
5. If a package is written, a tape is produced for its selected provenance.
6. If both exist, the identity gate runs and reports its outcome.

## 12. Limitations carried forward

| Limitation | Consequence |
| --- | --- |
| Colour attachment format is a GNM packed format | the replay renders into its own RGBA8 target |
| Blend factors and operations are not captured | a package with blend enable is refused |
| Effective shader state at the draw boundary is not recovered | the modules are captured, the selection is not |
| Texture and sampler descriptor writers are not fully established | some bindings may remain unresolved |
| Render targets are substituted | the diagnostic target is the replay's |

The second launch requires fresh explicit authorization.
