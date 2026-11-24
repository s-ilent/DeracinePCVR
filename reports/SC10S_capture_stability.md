# SC-10S: texture capture stability and noninterference

## Result

The three SC-10R defects are fixed, and capture noninterference is now measured rather than
asserted: the rendered frame is bit-identical with the instrumentation off and on.

| Item | SC-10R | SC-10S |
| --- | --- | --- |
| Partial-subresource readback | whole-image transition, scoped restore | **scoped transition and per-subresource restore** |
| Multi-stage bindings | committed inside `BindTextures` | **committed at the draw boundary, incremental** |
| Partial mip/layer readiness | any subresource counted as complete | **every required subresource required** |
| Noninterference | not measured | **bit-identical frame, SHA-256 match** |
| Adversarial cases | 42 | **45** |
| Package tests | 23 | **26** |

## 1. Partial-subresource state restoration

`Image::Download` calls `GetBarriers(..., {})`, which requests a whole-image transition to
transfer-source. SC-10R then restored only the selected range, so every other mip and layer was left
in transfer-source. Restoring the whole image to one aggregate state would have been equally wrong
for an image whose subresources were already in different states.

`Image::DownloadScoped` is the fix. It records the state of exactly the subresources being copied,
transitions only those through `GetBarriers` with a range, copies, and restores each one to the state
it was found in with one barrier per subresource. The per-subresource table is updated to match the
barriers emitted, so shadPS4's tracked state stays consistent with what Vulkan was told.

A subresource found in `eUndefined` or `ePreinitialized` is restored to a sampling layout rather than
back to undefined, because leaving it undefined would make the next sample of it invalid.

The aggregate state is updated only when the copy covered the whole image and every subresource ended
in the same state. Otherwise the image is genuinely partially transitioned and the per-subresource
table is authoritative.

### The GPU test

`tools/readback_test` gained a scoped-copy case. It builds a 3-mip, 2-layer BC3 image, fills every
subresource with a pattern derived from its (mip, layer), copies one subresource, then reads back
every other subresource and compares.

```
scoped-copy isolation:
  PASS  scoped copy mip 1 layer 1 of 3x2: copied=1024 bytes

negative control (whole-image transition, the SC-10R defect):
  FAIL  whole-image transition mip 1 layer 1 of 3x2: copied=1024 bytes,
        copied_mismatched=0 other_disturbed=0 validation_errors=5
  correctly FAILED  a whole-image transition disturbs subresources it did not read
```

The negative control is the interesting part. A subresource left in the wrong layout still holds the
right bytes, so a data comparison cannot see the defect. What sees it is the validation layer:
reading a subresource from `SHADER_READ_ONLY` when it is actually in `TRANSFER_SRC` is a VUID
violation. The test therefore counts validation messages and fails on either a disturbed byte or a
validation error. The debug-utils messenger is enabled so those messages reach the program.

## 2. Multi-stage binding collection

`CommitPendingImages` latched on the first commit for a draw. `BindTextures` runs once per shader
stage, so the vertex stage's commit discarded the fragment stage's records.

The commit moved to the end of `BindResources`, which is the draw's binding boundary: every stage has
run by then. Committing is now incremental rather than a one-shot latch. A draw is counted once, and
a record already stored for this draw is not stored again, so a repeated stage does not duplicate its
bindings. The duplicate identity is (provenance, stage hash, unified binding, image generation, mip,
layer), which distinguishes a repeated visit from a genuinely separate binding of the same image.

Capacity exhaustion is now reported through `CapacityExhausted()` rather than silently dropping the
rest, and a package built from a truncated set cannot claim content readiness.

### Two further defects found while testing

**The snapshot budget counted commits, not snapshots.** A draw that bound a sampled image but was
rejected by the format or size guards consumed the budget without producing anything. After 96 such
draws the gate closed and no snapshot was ever taken. The budget now counts actual snapshots through
`NoteSnapshotTaken()`.

**The record store filled with every draw's bindings.** Only the draw that is actually snapshotted
contributes records now. A binding record for a draw whose contents were never captured cannot be
replayed, and keeping every draw's bindings filled the 256-record budget before the selected draw was
reached.

## 3. Partial texture readiness

`texture_contents_ready` previously meant "this texture has some subresources". A view asking for
seven mip levels and receiving one was reported as complete.

The SC-10R package demonstrates the defect exactly:

```
mip_first 0  mip_count 7  layer_first 0  layer_count 1
subs: [(0, 0)]
contents_ready True
```

Seven mip levels requested, one captured, reported complete. The corrected derivation compares the
requested (mip, layer) set against the captured set and requires every pair. The same draw now
reports:

```
mip_first 0  mip_count 1  subs [(0, 0)]
contents_ready True
```

That is accurate: the view asks for one mip and one was captured.

The claims stay distinct. `texture_binding_ready` means the bindings were identified.
`texture_contents_ready` means every required subresource is present. `sampler_ready` means the
sampler state is available. `diagnostic_textured_replay_ready` means at least one complete usable
texture/sampler/UV combination exists, which does not require capturing every texture of a material.
`original_material_ready` remains false.

The supported scope is a single-mip snapshot. That is documented rather than expanded into a general
image exporter.

## 4. Noninterference

This is the runtime experiment the task called the most important, and it was run.

Two configurations of the same executable, same scene, same presentation path, same frame gate:

- **A, baseline:** every SC-10 instrumentation flag off.
- **B, instrumented:** provenance, vertex input, shader params and texture capture on, with a draw
  package written.

Both dumped the presented frame at the same scene gate.

```
off: 012_205c000000_2880x3240_R8G8B8A8Unorm_s1.png 1280x1440 channels=4
on:  013_205c000000_2880x3240_R8G8B8A8Unorm_s1.png 1280x1440 channels=4
off sha256: d34f8e6c0e282a53c169e84e352a25405a3147b8a226d6f00d29b69704f0a43f
on  sha256: d34f8e6c0e282a53c169e84e352a25405a3147b8a226d6f00d29b69704f0a43f
IDENTICAL: the capture did not change the rendered frame
```

The instrumented run did capture: it wrote a package with one texture, one subresource, and
`diagnostic_textured_replay_ready: true`. So the comparison is between a run that did nothing and a
run that took a synchronized image copy, and the frame is unchanged.

`tools/compare_capture_ab.py` performs the comparison. It reports the SHA-256 of both frames, and
when they differ it reports the differing pixel count, the maximum channel delta and the mean delta
over differing pixels, so a difference is characterised rather than merely detected.

## 5. Tests

| Suite | Result |
| --- | --- |
| `deracine_readback_test` | 8/8 byte-for-byte, scoped isolation passes, both negative controls fail correctly |
| `shadps4_draw_package_test` | 26 passed |
| `test_validator_adversarial.py` | 45/45 rejected |
| Capture A/B | bit-identical frame |
| Default OFF | zero records |

The three new adversarial cases cover a partial mip chain, a missing array layer, and a duplicate
subresource masking a missing one. The three new package tests cover a partial mip chain claiming
readiness, a complete mip chain claiming readiness, and a missing array layer.

## 6. Honest limits

**The supported capture scope is one mip level.** A view requesting more is reported as incomplete
rather than expanded into a general image exporter. That is a deliberate scope decision, not an
oversight.

**The mesh is a particle or foliage element.** It renders as a scattered field of small triangles.

**Only one texture is captured per draw.** The draw's other binding is not captured, so
`texture_contents_ready` reflects the captured texture's own completeness, not the material's.

**The sampler association relies on the shader declaring `associated_image`.** A shader that does not
declare it leaves the association unresolved, which is reported rather than guessed.

**The A/B comparison is one frame at one scene gate.** It establishes that the capture does not
change that frame. It does not prove noninterference across every scene or every frame.

**No Vulkan validation layers were enabled in the emulator runs.** They were enabled in the
independent readback test, which is where the copy-region and layout correctness is established.

## 7. What SC-10S does not do

No RHI integration, no original material reconstruction, no shader translation, no NRI, no OpenXR.
The package stays backend-neutral.

## Reproducible checks

```
# independent readback and scoped-copy proof
cmake -S tools/readback_test -B build-readback-test -G Ninja
cmake --build build-readback-test
./build-readback-test/deracine_readback_test --verbose

# capture A/B
$env:SHADPS4_DBG_DUMP_HMD_AT_SCENE='400,400,rt_dump-off'   # with all DERACINE flags 0
$env:SHADPS4_DBG_DUMP_HMD_AT_SCENE='400,400,rt_dump-on'    # with the capture flags 1
python tools/compare_capture_ab.py <lab>/user/rt_dump-off <lab>/user/rt_dump-on

# validate
python tools/validate_draw_package.py <output dir>
python tools/test_validator_adversarial.py <output dir>

# replay
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
./build-geometry-replay/deracine_textured_replay <output dir> textured
```
