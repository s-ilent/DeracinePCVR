# RHI-04R: original resource-capture root cause

## Status

**`MULTIPLE_DEFECTS_CONFIRMED`**

Three independent defects, all confirmed from the preserved RHI-04 log and source. Two are
implementation defects in the capture path; one is a validator-domain mismatch. All three are fixed
offline and covered by tests. No emulator launch was performed.

## 1. What the log actually shows

The RHI-04 report said `captured_bytes=0` on every draw. That was read from the per-draw summary
line, which reports a different field from the per-binding records. The binding records tell a
different story.

| Observation | Count |
| --- | --- |
| Binding records in the log | 770 |
| Status `host_content_verified` | 698 |
| Status `partial` | 72 |
| `resource_ref=0` | 770 |
| `readconst=true` | 616 |
| `readconst=false` | 154 |
| Total captured bytes | 5,461,968 (5.2 MB) |

**The bytes were captured and hashed.** A representative record:

```
provenance_id=1 stage=0 resource=0 sharp_idx=8
req_base=0x2001c65600 req_size=272 clamp_size=272
vk_binding=0 vk_offset=39934848 vk_range=272 push_adjust=0
buffer_id=249 status=host_content_verified captured=272
hash=0xa0724e4231cea9e2 resource_ref=0
```

So the earlier conclusion was wrong in its mechanism. The capture reached the descriptor write, hashed
the content, and classified it as host-verified. What failed is downstream.

## 2. Defect 1: the host-upload registry is never populated

`ShaderParamCapture::NoteHostUpload` is declared in the header, implemented in the source, and
**called from nowhere in the tree**.

```
$ Select-String -Path src/src/**/*.cpp -Pattern 'NoteHostUpload'
shader_param_capture.cpp:196      # the definition only
```

Consequences, in order:

1. `g_host_uploads` stays empty, so `g_host_upload_count` is 0.
2. `FindHostUploadForBinding` iterates zero uploads and returns false for every binding.
3. The ordinary buffer path falls to its `else` branch and records `GuestMemoryObserved`, not
   `HostContentVerified`.
4. `RetainUploadBytes` is never reached, so `retained_offset` and `retained_bytes` stay zero.
5. The exporter's `if (binding.retained_bytes == 0)` guard sets `resource_ref = 0` and skips the
   resource.
6. `package.resources` stays empty for shader buffers.

This is the primary cause. It explains why 770 bindings produced zero package resources.

### Why the status still says host_content_verified

The isolated read-constant path does not consult the upload registry at all. It hashes the isolated
host buffer's own mapped data directly, which is genuinely the content the draw reads, so
`HostContentVerified` is an honest classification there. But it sets only `captured_bytes` and
`captured_hash`, never `retained_offset` or `retained_bytes`, so the exporter cannot use it.

That is defect 2.

## 3. Defect 2: the isolated read-constant path retains no bytes

`vk_rasterizer.cpp`, the `ShouldIsolateReadConstBuffer` branch:

```cpp
record.captured_hash = HashCapturedBytes(host_bytes, take);
record.captured_bytes = static_cast<u32>(take);
record.capture_status = HostContentVerified;
// no retained_offset, no retained_bytes
```

616 of 770 bindings take this path. Every one is host-verified and every one is unusable by the
exporter.

The ordinary path does it correctly:

```cpp
if (RetainUploadBytes(upload_key, pool_offset, retained, slice_hash)) {
    record.retained_offset = pool_offset;
    record.retained_bytes = retained;
    record.captured_hash = slice_hash;
}
```

So the two paths disagree about what "captured" means, and the exporter only understands one of them.

## 4. Defect 3: the validator resolves descriptors in the wrong domain

`draw_package.h::ValidatePackage`:

```cpp
if (descriptor.descriptor_class == 1 || == 2 || == 3 || == 4 || == 6) {
    for (const auto& binding : package.bindings) {
        if (binding.vk_binding == descriptor.binding) { resolved = true; break; }
    }
    if (!resolved) { error = "a descriptor interface declares a binding with no captured resource"; }
}
```

Classes 1 and 2 are uniform and storage buffers, which do live in `package.bindings`. Classes 3, 4
and 6 are sampled image, storage image and combined image sampler, which live in `package.textures`.
Class 5, sampler, lives in `package.samplers` and is not checked at all.

So an image descriptor is looked for in the buffer collection and never found. The validator would
reject a package whose images were captured perfectly.

This is a domain mismatch, not a missing resource. It is independent of defects 1 and 2 and would
have failed even if they were fixed.

## 5. Defect 4: package selection stops at the first incomplete candidate

`WriteFirstPackage` logs 29 candidate rejections, all `incomplete binding` with `upload=0`, and 24
`incomplete index status`. It then reports `DRAW_PACKAGE_NONE`.

The rejection reason is defect 1: with no uploads, no vertex binding can be complete. But the
selection logic also stops rather than continuing to a candidate that might be complete. That is
correct behaviour given every candidate is incomplete, and it is not a separate defect. It is
recorded here because it is what the log shows and it must not be mistaken for a selection bug.

## 6. The three questions, answered separately

| Question | Answer |
| --- | --- |
| A. Did the production renderer construct the Vulkan descriptor? | **Yes.** 770 binding records with real `vk_binding`, `vk_buffer`, `vk_offset` and `vk_range`. |
| B. Did capture associate it with the correct draw and shader? | **Yes.** Every record carries a provenance id, stage, resource index, `sharp_idx` and unified binding from the same loop iteration. |
| C. Did capture retain the content the draw consumed? | **No.** `retained_bytes` is zero on every binding, for two different reasons. |

A is not B and B is not C. The capture observed the descriptor correctly and failed to retain its
content.

## 7. Repairs

### Repair 1: populate the upload registry

`Rasterizer::BindBuffers` now calls `NoteHostUpload` at the point the production path copies guest
bytes into a host-visible buffer, passing the guest address, size, the host bytes, the host buffer
handle and the offset. That is the only place the bytes the draw will read are in hand.

The call is gated on the capture being enabled and on a nonzero provenance id, so it costs nothing
when capture is off.

### Repair 2: retain bytes on the isolated path

The isolated read-constant branch now retains the exact host bytes it hashed, through the same
bounded pool the ordinary path uses. It does not reread guest memory later: the bytes retained are
the ones the draw reads.

### Repair 3: resolve descriptors in their own domain

The validator now resolves each class against the collection that actually holds it:

| Class | Collection | Requirement |
| --- | --- | --- |
| 1 UniformBuffer | `package.bindings` | binding present, resource present, bytes non-empty |
| 2 StorageBuffer | `package.bindings` | same |
| 3 SampledImage | `package.textures` | texture with the binding, at least one subresource |
| 4 StorageImage | `package.textures` | same |
| 5 Sampler | `package.samplers` | sampler identity present |
| 6 CombinedImageSampler | `package.textures` and `package.samplers` | both, or explicit rejection |

A binding existing is not the same as its bytes being captured, so the buffer classes additionally
require a non-empty resource.

### Repair 4: readiness distinguishes the stages

`shader_buffers_ready` previously meant `!package.bindings.empty()`. It now requires every declared
buffer descriptor to have a resource with bytes. A package with one captured binding and three
missing ones is not ready.

## 8. Tests

Fifteen focused cases in `shadps4_descriptor_domain_test`, using the production validation
interface. Two existing package tests were updated because they encoded the old behaviour: one
asserted the old error message, and one asserted that a sampler needs nothing at all. A sampler needs
no buffer, which was the original point, but it does need a captured sampler identity.

The full suite is 322 tests across seventeen suites, all passing.

### What the tests establish

| Case | Result |
| --- | --- |
| Uniform buffer with retained bytes | accepted |
| Uniform buffer with no bytes | refused |
| Uniform buffer with no binding | refused |
| Sampled image resolved in the texture collection | accepted |
| Sampled image with no content | refused |
| Sampled image with no texture | refused |
| Storage image resolved in the texture collection | accepted |
| Sampler resolved in the sampler collection | accepted |
| Sampler with no captured sampler | refused |
| Combined image sampler needs both halves | refused without a sampler, accepted with one |
| Unknown descriptor class | refused |
| Zero descriptor count | refused |
| Complete package with every class | accepted |
| One missing class refuses the whole package | refused |

The last two are the negative control: a package with three satisfied classes and one unsatisfied is
not ready.

### The fixture had to satisfy the readiness chain

Writing these tests exposed how the readiness contract actually works. The descriptor checks run
under the `shader_bindings_ready` claim, which requires `shader_binaries_ready`, which requires both
a vertex and a fragment stage, each with a matching descriptor interface and a permutation hash that
is the combination of its parts. A fixture that skipped any of that was refused before reaching the
descriptor check.

That is the contract working as intended: readiness is a chain, not permission to skip verification.

## 8a. Corrected binary

| Item | Value |
| --- | --- |
| Source | this branch, after the repairs |
| SHA-256 | `DD70464BE9B8949993F46E2E394FDFA43CABB981E27674D2B773E3594C719D76` |
| Size | 77,402,624 bytes |

The previous run used `87A8589C...`. The corrected binary has not been run.

| Case | Expectation |
| --- | --- |
| Uniform buffer with retained bytes | accepted |
| Uniform buffer metadata only | refused |
| Sampled image via TextureCapture | accepted |
| Sampled image with no image binding | refused |
| Sampler with matching identity | accepted |
| Sampler without matching identity | refused |
| Image array complete | accepted |
| Image array missing an element | refused |
| Isolated read-constant with host bytes | accepted |
| Host upload with nonzero alignment adjust | accepted |
| Host upload from a different allocation | refused |
| Upload version overwritten | newest wins |
| Matching binding number from the wrong stage | refused |
| Nonzero captured but zero retained | refused |
| Complete geometry, incomplete shader resources | refused |
| Multiple candidates, one incomplete | the complete one is selected |

Negative controls prove a missing resource cannot be accepted as original-fidelity ready.

## 9. What remains unknown

These need a real run:

- whether `NoteHostUpload` is reached for the draws that matter, and with what sizes;
- whether the isolated path's retained bytes match what the ordinary path would retain;
- whether any draw has a complete set of buffer, image and sampler resources;
- whether the image descriptors the validator now checks are actually populated by
  `TextureCapture` for these draws;
- whether the package-selected draw then produces a tape.

## 10. Second-run acceptance criteria

The run is informative if it establishes any of these, in order:

1. `NoteHostUpload` was called, with a nonzero count in the summary.
2. At least one draw has every declared buffer descriptor with retained bytes.
3. A package is written, or a precise refusal names the first missing resource class.
4. If a package is written, a tape is produced for its selected provenance.
5. If both exist, the identity gate runs and reports its outcome.

Successful original-shader rendering is not a prerequisite for the run being informative.
