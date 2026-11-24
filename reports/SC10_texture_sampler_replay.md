# SC-10: effective texture and sampler capture, and standalone textured replay

## Result

A real Déraciné draw has been captured with its effective texture, its contents and its sampler
state, and rendered by a standalone Vulkan program that samples the recovered texture.

| Item | Value |
| --- | --- |
| Draw | provenance 25 |
| Vertex streams | 4 |
| Vertices | 807 |
| Indices | 1101 |
| Triangles | 367 |
| Texture | 64x64 pixels, `BC3_SRGB_BLOCK`, 4096 bytes |
| Sampler | bilinear mag/min, nearest mip, LOD 0 to 15.996 |
| UV attribute | binding 3, components 0 and 1, range [0,1] |
| Replay device | NVIDIA GeForce RTX 3090 Ti |
| Textured render | 19,477 distinct colours |
| Untextured render | 2 distinct colours |
| UV-only render | 3,673 distinct colours |

## 1. What was captured, and where

The capture records the effective image binding at the point the production renderer builds the
descriptor, in the descriptor-write loop of `BindTextures`. That is the only place where the unified
Vulkan binding number, the texture-cache image, its view and the layout the descriptor was built
from all belong to the same descriptor.

The sampler is recorded in the sampler loop, from the state the production `Sampler` constructor
produced. `Sampler` now exposes `AnisotropyEnabled()` and `MaxAnisotropy()` so the observer reads the
real state rather than re-deriving it and risking a different answer.

## 2. The image snapshot

`TextureCache::SnapshotImageForCapture` copies the image through `Image::Download`, the same
synchronized path the renderer uses to download an image. That path ends the current rendering,
barriers the image to transfer-source, copies, and barriers the buffer back.

The readback is deferred through `Scheduler::DeferPriorityOperation`, which runs only after the GPU
has reached the current tick. That is what makes the bytes the image's contents rather than whatever
the buffer held before.

The snapshot is bounded: at most 24 draws per run, at most 8 MiB per image. It is taken only for a
draw the provenance chain matched and that actually binds a sampled image.

## 3. Block-compressed images

The first texture the capture found was `BC3_SRGB_BLOCK`. A compressed image is a legal copy target,
so it is not rejected. The copy extent is expressed in blocks and the row pitch is the block-row
pitch, which is what the capture records.

The replay expands the block count back to the pixel extent, because `vkCmdCopyBufferToImage` takes
its extent in texels even for a compressed format. Passing the block count copied a quarter of the
image and left the rest undefined, which rendered black until it was fixed.

## 4. Texture coordinates

The UV attribute is found by evidence, not by assumption. The search accepts two- and
four-component streams in 32-bit float, 16-bit float and 16-bit normalized formats, and scores each
component pair on whether both axes sit inside a small margin around [0,1] and are not degenerate.

The selected draw's UVs are on binding 3, a `R16G16B16A16_SFLOAT` stream, components 0 and 1, with
range `u [-0.001, 0.993]` and `v [0.788, 1.004]`. The evidence string travels in the package.

An earlier attempt restricted the search to two-component 32-bit float and found nothing, because
this draw packs its UVs into a four-component half-float stream. That is recorded here because it is
the kind of assumption the task warned against.

## 5. The package

Schema 2, extended with `textures`, `samplers` and `texture_coordinate`. Texture payload goes in its
own `textures.bin` sidecar, so a geometry-only consumer never has to skip past it.

The readiness dimensions are now:

```json
"readiness": {
  "shader_buffers_ready": true,
  "geometry_ready": true,
  "texture_binding_ready": true,
  "texture_contents_ready": false,
  "sampler_ready": true,
  "pipeline_state_ready": false,
  "diagnostic_geometry_replay_ready": true,
  "diagnostic_textured_replay_ready": true,
  "original_material_ready": false,
  "limitation": "diagnostic textured replay from the captured texture only: another binding on this draw was not captured"
}
```

`texture_contents_ready` is false because this draw binds two textures and only one was captured.
`diagnostic_textured_replay_ready` is true because one usable texture is enough for a diagnostic.
The two claims are deliberately distinct: a diagnostic can sample what was captured, but the draw's
original material cannot be reproduced from an incomplete set.

## 6. Independent validation

`tools/validate_draw_package.py` now checks texture payload hashes, subresource bounds and row
lengths, duplicate texture and sampler ids, sampler reference resolution, sampler numeric ranges,
and that each readiness claim is justified by the data.

```
VALID: schema=2 provenance=25 resources=10 shader_bindings=6 vertex_bindings=4 indexed=True
       bytes=56638 three_d=True triangles=367 textures=2 samplers=11 textured_ready=True
```

### Adversarial tests

`tools/test_validator_adversarial.py` corrupts a real package 34 ways. All 34 are rejected. The
texture-specific cases cover a corrupted texture byte, a hash mismatch, an out-of-range subresource,
an empty subresource, a duplicate texture id, a missing sampler reference, inverted LOD bounds,
anisotropy below one, an out-of-range address mode, and three readiness overclaims.

```
34/34 adversarial cases rejected
```

## 7. The standalone replay

`tools/geometry_replay/deracine_textured_replay.cpp` is a separate executable in the same independent
CMake project. Its dependencies are the Vulkan loader and the C runtime.

It creates its own instance, device, queue, 640x640 colour target, D32 depth target, render pass,
pipeline, descriptor set and sampler. It uploads the captured texture bytes into a device-local
image, creates the view at the captured format, builds the sampler from the captured state, binds it
through a combined image sampler, and draws the captured geometry with the captured UVs.

Three modes exist so the result can be shown to differ rather than asserted to:

| Mode | Distinct colours |
| --- | --- |
| textured | 19,477 |
| untextured | 2 |
| uv | 3,673 |

## 8. Honest limits

**The mesh is a particle or foliage element, not a solid object.** It renders as a scattered field of
small triangles. That is the draw's actual geometry, not a capture error.

**Only one of the draw's two textures was captured.** The second binding is a 5760x3240 image at
8.3 MB, over the per-image bound. `texture_contents_ready` is false for that reason.

**The texture is a mask.** Its alpha channel is mostly zero, so the diagnostic forces alpha opaque
to show the colour. That is a diagnostic choice, recorded in the shader source.

**No original shader, no lighting, no material state.** The fragment shader is a single
`texture()` call. `original_material_ready` is false.

**No Vulkan validation layers were enabled.** I did not enable them, so I am not claiming
validation-layer success. The draw completed without a device loss.

**The scene identity is not established.** The draw is a verified textured mesh of unknown scene
identity.

## 9. What SC-10 does not do

No texture detiling, no material reconstruction, no GCN shader translation, no UE4 material
interpretation, no OpenXR, no NRI. The package stays backend-neutral: it describes formats,
dimensions, subresources, sampler state and binding numbers, not Vulkan handles.

## 10. Handoff

The portable pieces are the `UploadKey` identity model, the `ContentStatus` vocabulary, the
`Readiness` dimensions, the package schema, the validator, and the replay contract.

The lesson that carried through three phases: observe the resource the production path actually
selected, and check the relation that actually holds. SC-09G's equality test on upload offsets
looked right and silently rejected every multi-stream draw. SC-10's copy extent in blocks looked
right and rendered a quarter of the image.

## Reproducible checks

```
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
