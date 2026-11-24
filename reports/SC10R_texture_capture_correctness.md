# SC-10R: texture capture correctness and fidelity

## Result

The three SC-10 defects are fixed, and a deterministic GPU fixture now proves the readback is
byte-for-byte complete.

| Item | SC-10 | SC-10R |
| --- | --- | --- |
| Copy extent | block count | **texel extent** |
| BC3 64x64 payload | 4096 bytes, quarter copied | **4096 bytes, complete** |
| Readback proof | none | **8/8 byte-for-byte, negative control fails** |
| Image state after copy | left in transfer-source | **restored to a sampling layout** |
| Binding join | `vk_binding = 0` | **unified binding number carried** |
| Sampler association | first-sampler fallback | **verified, or explicitly unresolved** |
| Block metadata | `block_width` = block count | **`block_columns` + `block_texels_x`** |
| Adversarial cases | 34 | **42** |

## 1. The copy extent defect

SC-10 computed the block count and passed it to `vk::BufferImageCopy::imageExtent`,
`bufferRowLength` and `bufferImageHeight`. Vulkan takes all three in **texels**, including for a
block-compressed format. A 64x64 BC3 image therefore copied 16x16 texels, a quarter of the image,
and the remaining three quarters were whatever the buffer held.

The fix separates the two representations explicitly. `FormatBlockInfo` in
`src/src/video_core/texture_cache/format_block_info.h` describes a format's block texels, block byte
size and whether it is compressed. Storage geometry is computed from it in blocks; the copy region
uses the texel extent, with `bufferRowLength` and `bufferImageHeight` left zero for a tightly packed
transfer.

The table covers the formats the renderer can produce for a sampled image, and reports an unknown
format as unsupported rather than guessing a block size. BC1 and BC4 are 8 bytes per block; BC2,
BC3, BC5, BC6 and BC7 are 16. Assuming BC3's size for all of them would be wrong.

## 2. The independent readback test

`tools/readback_test/` is a separate CMake project with no dependency on shadPS4 or Déraciné. It
builds an image with known contents, uploads a deterministic payload, transitions it to a sampled
layout, reads it back through the same copy-region construction, waits for that submission, and
compares the entire downloaded range byte for byte.

The download buffer is pre-filled with `0xA5` before the copy, so a copy that writes nothing is
visible rather than passing as valid data. The test reports the first mismatch, the total mismatched
bytes, and the count of bytes still holding the fill pattern.

```
validation layers: enabled
device: NVIDIA GeForce RTX 3090 Ti

readback cases:
  PASS  BC3 64x64: 64x64 texels, 16x16 blocks, 4096 bytes
  PASS  BC3 8x8: 8x8 texels, 2x2 blocks, 64 bytes
  PASS  BC3 7x5 (not block aligned): 7x5 texels, 2x2 blocks, 64 bytes
  PASS  BC3 64x64 mip 2: 16x16 texels, 4x4 blocks, 256 bytes
  PASS  BC3 64x64 layer 1: 64x64 texels, 16x16 blocks, 8192 bytes
  PASS  RGBA8 64x64 control: 64x64 texels, 64x64 blocks, 16384 bytes
  PASS  RGBA8 7x5 control: 7x5 texels, 7x5 blocks, 140 bytes
  PASS  BC1 64x64 (8-byte blocks): 64x64 texels, 16x16 blocks, 2048 bytes

8/8 cases byte-for-byte equal

negative control (block extent, the SC-10 defect):
  FAIL  BC3 64x64 with block extent: 64x64 texels, 16x16 blocks, 4096 bytes,
        mismatched=4017 first_at=64 untouched=3828
  correctly FAILED  the block-extent copy does not reproduce the image
```

The negative control is the point. It runs the same case with the block count as the copy extent,
which is exactly SC-10's defect, and the test fails it: 4017 of 4096 bytes mismatch and 3828 were
never written. A test that only passes on the fix proves nothing about the fix.

Validation layers were enabled and reported no errors on the passing cases.

## 3. Image state restoration

SC-10 called `Image::Download`, which transitions the source to `eTransferSrcOptimal` and does not
restore it. The snapshot runs inside `BindTextures`, before the draw executes, so the draw would
sample an image in the wrong layout.

The fix records the subresource state before the copy and restores a sampling-compatible layout
afterwards through `Image::Transit`, so shadPS4's tracked state matches the barriers that were
emitted. The prior layout is restored when it is already sampling-compatible; otherwise the image
goes to `eShaderReadOnlyOptimal`, which is what the binding path would have transitioned to anyway.

## 4. Deferred readback lifetime

The readback stays deferred through `Scheduler::DeferPriorityOperation`, which runs only after the
GPU has reached the current tick. The download buffer is the cache's own utility buffer, which is
not reused before that point.

Each record now carries a monotonic `transfer_id`, so a late readback is tied to the copy that
produced it rather than to whatever image now occupies the same slot. A hash proves the retained
bytes have not changed; it does not prove they came from the intended generation, and the transfer
identity is what closes that gap.

## 5. Binding and sampler join

SC-10's snapshot caller passed `vk_binding = 0`, and the late record was appended as a separate
texture entry. The corrected capture carries the unified binding number, the stage hash and the
shader image index into the snapshot, and the package builder joins the late record onto the binding
record by that identity rather than appending a second entry.

The sampler association uses the shader's own `associated_image` field, which is the only evidence
that pairs a sampler with an image: shadPS4 binds them as separate descriptors. The sampler store is
now keyed on (guest state, stage), because the same guest state bound in two stages is two records
and joining on state alone would pair a sampler with an image from a different stage.

When the association cannot be established, `sampler_id` stays zero and `sampler_verified` stays
false. The replay refuses to render rather than silently substituting the first sampler; a
`--substitute-sampler` flag makes the substitution explicit and labels it in the output.

## 6. Unambiguous block metadata

SC-10 recorded the block *count* in fields named `block_width` and `block_height`, then expanded them
during replay. That read as the block size and made the byte length impossible to check.

The package now records `block_columns`, `block_rows`, `block_texels_x`, `block_texels_y` and
`bytes_per_block` separately, alongside the texel `width` and `height`. The names are deliberately
distinct. A dimension that is not block aligned makes the texel extent and the block count genuinely
different, so neither is inferred from the other.

The captured texture now reads:

```
64x64 texels, 16x16 blocks of 4x4 texels, 16 bytes/block, format=138, bytes=4096, transfer=52
```

## 7. Validation

The C++ and Python validators now check that a compressed subresource's payload length is exactly
what its block geometry implies, that the recorded block counts agree with the texel extent, that
the row pitch is the block-row pitch, that a captured subresource names its transfer, and that a
verified sampler claim names a sampler.

The adversarial suite grew from 34 to 42 cases. All 42 are rejected:

```
42/42 adversarial cases rejected
```

The new cases cover a wrong block texel size, a wrong block column count, a wrong row pitch, a
truncated final block row, a wrong pixel extent, missing block geometry, a verified sampler with no
id, and a stale transfer identity.

The truncated-row case is worth noting: it shrinks the payload and re-hashes it, so the hash matches
the bytes it does contain. Only the length check catches it, which is why payload integrity alone is
not proof of a complete capture.

## 8. Replay

The three modes are retained and still differ measurably:

| Mode | Distinct colours |
| --- | --- |
| textured | 1,703 |
| untextured | 2 |
| uv | 3,673 |

The textured output changed substantially from SC-10's 19,477 colours. That is expected: SC-10 was
sampling a quarter-copied image with the wrong extent, so its output was noise. The corrected
capture produces a coherent warm-toned particle field. Visual similarity to the old output is not a
correctness criterion, and the old image is kept only as a historical reference.

The diagnostic alpha override is retained and labelled in the shader source.

## 9. Tests

| Suite | Result |
| --- | --- |
| `deracine_readback_test` | 8/8 byte-for-byte, negative control correctly fails |
| `shadps4_draw_package_test` | 23 passed |
| `test_validator_adversarial.py` | 42/42 rejected |
| Default OFF | zero records |

## 10. Honest limits

**The mesh is a particle or foliage element.** It renders as a scattered field of small triangles.
That is the draw's geometry, not a capture error.

**Only one texture is captured per draw.** The package carries one texture with content; the draw's
other binding is not captured. `texture_contents_ready` is false for that reason while
`diagnostic_textured_replay_ready` is true.

**The sampler association is verified for this draw but the mechanism is narrow.** It relies on the
shader declaring `associated_image`. A shader that does not declare it leaves the association
unresolved, which is reported rather than guessed.

**No original shader, no lighting, no material state.** `original_material_ready` is false.

**Non-interference was not measured by image comparison.** The capture is read-only and default OFF,
and the state restoration is verified by construction, but I did not run the same scene with capture
OFF and ON and diff the frames. That comparison remains the strongest available check and is not
done.

**No Vulkan validation layers were enabled in the emulator run.** They were enabled in the
independent readback test, which is where the copy-region correctness is established.

## 11. What SC-10R does not do

No shader translation, no UE4 material reconstruction, no RHI-02, no NRI, no OpenXR. The package
stays backend-neutral.

## Reproducible checks

```
# independent readback proof
cmake -S tools/readback_test -B build-readback-test -G Ninja
cmake --build build-readback-test
./build-readback-test/deracine_readback_test --verbose

# capture
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
$env:SHADPS4_DERACINE_TEXTURE_CAPTURE='1'
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'

# validate
python tools/validate_draw_package.py <output dir>
python tools/test_validator_adversarial.py <output dir>

# replay
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
./build-geometry-replay/deracine_textured_replay <output dir> textured
./build-geometry-replay/deracine_textured_replay <output dir> untextured untextured
./build-geometry-replay/deracine_textured_replay <output dir> uv uv
```
