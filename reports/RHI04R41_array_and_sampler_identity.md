# RHI-04R4.1: array observation and sampler provenance erratum

## Status

**`READY_FOR_SECOND_CAPTURE_WITH_EXPLICIT_LIMITATIONS`**

Three production-path defects repaired, the RHI-04R4 test suite corrected, and the offline gate
re-verified. The corrected emulator builds and the full suite passes. It has not been run.

| Item | Value |
| --- | --- |
| Branch | `research/rhi04r41-array-and-sampler-identity-20261125` |
| Base | `de281a8f5322a7903eed94ba7cfb5d356e93d084` |
| Corrected emulator SHA-256 | `D93A6B4A94624FC0FA341C9ADE0F5B918DCBE278EEEEB289E1E7B620876DB232` |
| Size | 77,658,112 bytes |
| C++ tests | 377 across twenty-one suites |
| Provenance cases | 39 |
| GPU mutation cases | 13, enforced |
| Vulkan validation errors | 0 |

## 1. Erratum: what RHI-04R4 overclaimed

The RHI-04R4 report said the array and sampler identity chain was "verified end to end". That was
not true, and the report should have said so.

**What RHI-04R4 actually verified.** The package validator's contract. The fourteen
`EndToEndIdentity` tests constructed package objects by hand and asserted that the validator accepted
or refused them. That is a real check of the consumer, and it caught nothing wrong with the consumer.

**What it did not verify.** The producer. No test called `NoteImageBinding`, `CommitPendingImages`,
`NoteSnapshotTaken` or the exporter's sampler filter. Three defects lived in exactly that gap:

| Defect | Where | Why the R4 tests missed it |
| --- | --- | --- |
| Only the first array element was observed | `vk_rasterizer.cpp` write loop | the tests built packages directly |
| Snapshots derived bindings from flattened indices | `vk_rasterizer.cpp` snapshot loop | the tests never ran a snapshot |
| The image-sampler association searched every sampler | `shader_param_capture.cpp` | the tests never ran the association |

One R4 test was worse than useless. `ASamplerFromAnotherDrawCannotSatisfyTheDeclaration` built a
package with a foreign sampler, then asserted a *different* package — one with no sampler at all —
was refused. It never tested the invariant it named. It is now a real test.

The lesson is specific: a validator test proves the validator, not the pipeline. The R4 report
presented the second as the first.

## 2. Defect: only the first array element was observed

### The defect

The descriptor-write loop recorded one observation per descriptor write:

```cpp
if (Core::TextureCapture::Enabled() && image_id) {
    // one record, using image_bindings[image_binding_idx]
}
```

A write with `descriptorCount = array_size` covers `array_size` elements, each with its own image in
`pImageInfo`. Every element after the first was never observed, and the package would then claim a
complete array it had not seen.

### The repair

The loop now iterates every element, taking its image from `image_bindings[image_binding_idx +
element]` and recording `array_element = dstArrayElement + element` with `array_count = array_size`.

The Vulkan descriptor write itself is unchanged. This is capture instrumentation only.

## 3. Defect: snapshots derived bindings from flattened indices

### The defect

```cpp
u32 binding_index = 0;
for (const auto& [image_id, desc] : image_bindings) {
    const u32 unified = first_image_binding + binding_index;
    ++binding_index;
```

A flattened image index is not a Vulkan binding number. A two-element array at binding 4 produced
snapshots at bindings 4 and 5, so the late content could never join its binding observation.

### The repair

The snapshot loop rebuilds the mapping from the same `image_descriptor_array_sizes` the write loop
used:

```cpp
u32 slot = 0;
u32 unified = first_image_binding;
for (const u32 array_size : image_descriptor_array_sizes) {
    for (u32 element = 0; element < array_size; ++element) {
        // element_binding = unified, element_index = element
    }
    unified += array_size;
}
```

`SnapshotImageForCapture` now takes `array_element` and `array_count`, and the content record carries
them, so the late-content join uses the same identity the binding observation recorded.

## 4. Defect: the image-sampler association ignored provenance

### The defect

The exporter filtered the exported sampler list by draw provenance, but the association that sets
`sampler_id` and `sampler_verified` searched the unfiltered collection:

```cpp
for (const auto& sampler : samplers) {
    if (sampler.associated_image == image.shader_image_index &&
        sampler.stage_hash == image.stage_hash) {
```

A sampler from another draw sharing the shader hash and image index could be paired with this draw's
image.

### The repair

The association now requires `sampler.provenance_id == draw.provenance_id`, and an ambiguous match —
two samplers in one draw claiming the same image index — clears `sampler_verified` rather than
picking the first.

The validator additionally checks that a sampler's provenance matches the package's own draw, and
refuses a binding claimed by more than one sampler.

## 5. Tests: production path versus package builders

This distinction is the point of the erratum, so it is stated explicitly.

### Tests that call production capture code

| Suite | Cases | What it exercises |
| --- | --- | --- |
| `shadps4_production_capture_identity_test` | 9 | `NoteImageBinding`, `CommitPendingImages`, `NoteSnapshotTaken`, `NoteSampler` |
| `shadps4_sampler_binding_test` | 10 | `NoteSampler` dedup and capacity |
| `shadps4_pipeline_binding_test` | 12 | `ResolvePipelineBindings`, `ResolveDescriptorBinding` |
| `shadps4_package_reader_test` | 11 | the production `package::Read` |
| `shadps4_texture_capture_accounting_test` | 10 | capture budget accounting |

### Tests that build package objects directly

| Suite | Cases | What it exercises |
| --- | --- | --- |
| `shadps4_end_to_end_identity_test` | 15 | `ValidatePackage` against hand-built packages |
| `shadps4_descriptor_domain_test` | 23 | `ValidatePackage` descriptor domains |
| `shadps4_draw_package_test` | 49 | `ValidatePackage` and serialization |

The second group is still valuable: it pins the consumer contract. It is not evidence about the
producer, and the R4 report should not have presented it as such.

### New production-path cases

| Case | Result |
| --- | --- |
| A two-element array produces two observations at one binding | both at binding 4, elements 0 and 1 |
| Two elements of the same image keep distinct identities | same uid, different element |
| A genuinely identical observation is deduplicated | one record |
| A record from another draw is not committed | zero records |
| The snapshot rejoins the correct binding and element | one record at binding 4, element 1 |
| Identical sampler state at two bindings | two observations |
| A foreign sampler is not selected for association | one candidate, the local one |
| Two samplers claiming one image are ambiguous | two matches, not verified |
| A sampler from another draw is retained but separate | two observations |

## 6. Test results

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
| Package reader | 11 |
| Reference compare | 17 |
| Reference oracle | 8 |
| Pipeline presence | 7 |
| Capture artifacts | 16 |
| Producer readiness | 12 |
| Descriptor domain | 23 |
| Pipeline binding | 12 |
| Sampler binding | 10 |
| End-to-end identity | 15 |
| Production capture identity | 9 |
| **Total** | **377** |

Plus 20 selection cases, 19 integration cases and 13 enforced GPU mutation cases, with zero
validation errors.

### Baseline accounting

RHI-04R4 was 366 across twenty suites. This branch adds
`shadps4_production_capture_identity_test` with 9 cases, extends `test_package_reader` by 1 and
`test_end_to_end_identity` by 1, giving 377 across twenty-one. Nothing was removed.

## 7. What remains runtime-only

- whether Déraciné actually uses multi-element image arrays;
- whether the snapshot loop's rebuilt mapping matches what the write loop recorded in a real draw;
- whether any draw has a complete set of buffer, image and sampler resources;
- whether a package is then written, and a tape for its selected provenance.

## 8. First unresolved invariant

**No runtime evidence exists that any draw has a complete descriptor set.** Every offline gate
passes, but the producer has never been observed writing a package from a real draw.

## 9. Second-run acceptance criteria

1. No pipeline reported a layout disagreement.
2. `NoteHostUpload` was called, with a nonzero count.
3. At least one draw has every declared descriptor resolved with retained bytes.
4. A package is written, or a precise refusal names the first missing resource class.
5. If a package is written, a tape is produced for its selected provenance.
6. If both exist, the identity gate runs and reports its outcome.

The second launch requires fresh explicit authorization.
