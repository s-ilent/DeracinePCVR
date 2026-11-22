# SC-09H: first recognizable 3D scene mesh replay

## Result

A real Déraciné classroom draw has been captured with five vertex streams and rendered by a
standalone Vulkan program from three camera angles. The geometry is verifiably three-dimensional.

| Item | Value |
| --- | --- |
| Draw | provenance 43 |
| Vertex streams | 5 |
| Vertices | 593 |
| Indices | 3036 (16-bit) |
| Triangles | 1012 |
| Degenerate triangles | 0 |
| Unique referenced vertices | 593 |
| Nonfinite vertices | 0 |
| Captured bytes | 50,040 |
| Position bounds | (-197.6, -255.6, -12.5) to (329.0, 176.4, 128.2) |
| Replay device | NVIDIA GeForce RTX 3090 Ti |
| Views rendered | 3, with depth and perspective |

## 1. What SC-09G got wrong

SC-09G selected the largest draw with complete geometry and got a 925-vertex screen-space element.
Two defects caused that.

**Selection was by size.** A large UI quad outranked a small scene mesh.

**Multi-stream draws were silently rejected.** 69 draws with three or more streams were skipped as
"incomplete". The cause was a real bug: `FindHostUploadForBinding` required the binding's host
offset to *equal* the upload's offset, but the production path uploads merged ranges, so a binding
usually sits somewhere inside an upload. Every multi-stream draw failed that test.

## 2. The upload containment fix

The relation is containment, not equality. A binding resolves to an upload when:

- the host allocation matches,
- the binding's host offset falls inside the upload,
- the same delta holds on the guest side, which is what proves the two describe the same bytes,
- the retained prefix covers the binding's slice.

`CopyUploadBytes` and `RetainUploadBytes` now index from the upload's own start, and the retained
hash is computed over the **slice** rather than the whole upload. That last part mattered: the
validator refused the first multi-stream package with "resource content hash does not match its
bytes", which was correct, because the package carried the upload's hash against the slice's bytes.

## 3. Candidate inventory

`tools/inventory_draws.py` classifies every sampled draw by index count, vertex count, attribute
count, stream count, strides, formats, instance count, primitive topology and a screen-space
heuristic.

The sample contains 128 draws. The largest is 48,744 indices across 25,387 vertices and four
streams, but it is not automatically the best target: its index buffer is 97 KB and its vertex
buffers are 300 KB, which exceeds the retained prefix.

## 4. Stable selection

Selection is by signature, not by provenance id, because ids shift between runs. A candidate must
be a complete indexed draw whose every vertex binding resolved to host content. It is scored on
three-dimensional structure rather than raw size: each attribute with three or more components
contributes a large weight, so a small scene mesh outranks a large screen-space element.

`SHADPS4_DERACINE_DRAW_PACKAGE_SELECT=<vs_hash>` overrides the choice for reproducibility.

## 5. Attribute interpretation

The capture now records, per binding, the attribute the production vertex-input construction
describes: semantic, Vulkan format, byte offset, component count, stride, input rate and divisor.
These travel with the binding because shadPS4 binds one attribute per binding.

The selected draw's layout:

| Binding | Semantic | Format | Components | Stride | Bytes |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | `R32G32B32_SFLOAT` | 3 | 12 | 7116 |
| 1 | 1 | `R8G8B8A8_UNORM` | 4 | 16 | 9488 |
| 2 | 2 | `R8G8B8A8_UNORM` | 4 | 16 | 9488 |
| 3 | 3 | `R8G8B8A8_UNORM` | 4 | 4 | 2372 |
| 4 | 4 | `R16G16B16A16_SFLOAT` | 4 | 16 | 9488 |

Binding 0 is the only 32-bit float stream and the only one with real extent on all three axes.
Bindings 1, 2 and 4 are normalized values in `[0,1]`, which is the shape of a normal or a tangent.
Binding 3 has stride 4 with four components, so it is a packed colour rather than geometry.

## 6. Position attribution

The position candidate is chosen by evidence, not assumption: the 32-bit float attribute with the
largest **second-axis** span. Ranking by the second-largest span rather than the largest prevents a
flat quad from winning on one wide axis alone.

The evidence string is recorded in the package so a reader can disagree with the choice. The
confidence is moderate: the format and the extents are consistent with object-space position, but
the game's own transform is not captured, so this is an attribution rather than a recovered fact.

## 7. The package

Schema 2, extended with the attribute description, a `geometry` block and the existing readiness
dimensions.

```json
"geometry": {
  "position_binding": 0,
  "position_components": 3,
  "position_evidence": "largest second-axis span among 32-bit float attributes",
  "bounds_min": [-197.594, -255.557, -12.5376],
  "bounds_max": [329.012, 176.393, 128.182],
  "is_three_dimensional": true,
  "triangle_count": 1012,
  "degenerate_triangles": 0,
  "unique_referenced_vertices": 593,
  "nonfinite_vertices": 0
}
```

## 8. Parser correctness

The replay's string-search reader is gone. It could confuse similarly named fields in different
objects, which is not acceptable for a multi-stream package. It now uses `nlohmann/json` from the
tree's own externals, and a missing required field is a hard error rather than a silent zero.

## 9. Independent validation

`tools/validate_draw_package.py` re-derives every hash and re-checks the geometry contract:

- format is known, and its component count matches the declared one,
- the attribute fits inside its stride,
- the resource range is a multiple of the stride,
- the index width is 2 or 4 and the count fits the captured bytes,
- the highest index fits the captured vertex range,
- the bounds are finite, ordered and nondegenerate when a 3D claim is made,
- the declared triangle, degenerate and unique-vertex counts match the captured indices.

```
VALID: schema=2 provenance=43 resources=11 shader_bindings=5 vertex_bindings=5 indexed=True
       bytes=50040 three_d=True triangles=1012 diagnostic_ready=True
```

### Adversarial tests

`tools/test_validator_adversarial.py` corrupts a real package 22 different ways and asserts
rejection. All 22 are rejected:

```
22/22 adversarial cases rejected
```

The cases cover a corrupted blob byte, wrong schema, missing provenance, duplicate ids, dangling
references, unknown format, wrong component count, attribute exceeding stride, zero stride, bad
index width, oversized index count, a degenerate 3D claim, inverted bounds, wrong triangle count,
wrong degenerate count, wrong unique count, missing position binding, component mismatch,
diagnostic readiness without geometry, full-draw readiness without textures, an unverified vertex
resource, and a truncated blob.

## 10. The 3D replay

`tools/geometry_replay/` is a separate CMake project. Its dependency list is the Vulkan loader and
the C runtime. No shadPS4 DLL, no emulator, no guest address space.

It creates its own instance, device, queue, 640x640 colour target, D32 depth target, render pass,
pipeline and command buffers. It uses a perspective projection, a look-at camera, depth testing and
a push-constant MVP. It renders three views and writes each as a PPM.

```
package: provenance=43 resources=11 vertex_bindings=5 indexed=yes
geometry: three_dimensional=yes triangles=1012 degenerate=0 unique_vertices=593
geometry: position_binding=0 evidence="largest second-axis span among 32-bit float attributes"
geometry: bounds=(-197.594,-255.557,-12.5376)-(329.012,176.393,128.182)
replay: vertices=593 indices=3036 stride=12 scale=0.00379791
device: NVIDIA GeForce RTX 3090 Ti
replay: views=3 primitives=1012 depth=yes perspective=yes
```

The three views show the same object from the front, from an oblique angle and from an elevated
rear position. The silhouette changes consistently with the camera, which is what distinguishes a
three-dimensional object from a flat projection.

## 11. Honest limits

**The object is a flattened relief, not a free-standing object.** Its z extent is 140 units against
an x extent of 527, so it is roughly 27% as deep as it is wide. That is consistent with a wall
panel, a blackboard or a framed picture rather than a chair or a birdcage.

**The scene identity is not established.** I captured the classroom backdrop for comparison, and the
mesh's proportions are consistent with a wall-mounted element, but I did not isolate the draw in the
research environment to confirm which object it is. Per the task's own instruction, this is reported
as a verified 3D mesh of **unknown scene identity** rather than named.

**Only the position stream is rendered.** The package carries all five streams, but the diagnostic
renderer consumes position alone. Bindings 1, 2 and 4 are normalized values consistent with normals
or tangents; they are captured and described but not yet used for shading.

**The transform is diagnostic.** The object is centred and scaled to fit the viewport. That
transformation is reported in the replay output as `scale=0.00379791` and is not the game's.

**No Vulkan validation layers were enabled.** I did not enable them, so I am not claiming
validation-layer success. The draws completed without a device loss, which is a weaker statement.

## 12. What remains for SC-10

Textures and samplers, through the same identity and versioning model. The `textures_ready`
dimension already exists and is false.

The lesson from this phase carries forward: observe the host resource the production binding path
selected, and check the relation that actually holds. SC-09G's equality test looked correct and
silently rejected every multi-stream draw. Containment was the real relation.

## Reproducible checks

```
# capture
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'

# inventory and validate
python tools/inventory_draws.py <log>
python tools/validate_draw_package.py <output dir>
python tools/test_validator_adversarial.py <output dir>

# replay
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
./build-geometry-replay/deracine_scene_replay <output dir>
```
