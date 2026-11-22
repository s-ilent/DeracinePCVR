# SC-09G: geometry resource capture and first independent Vulkan replay

## Result

A real Déraciné title-screen draw has been captured with complete, version-correct geometry and
rendered by a standalone Vulkan program that has no dependency on shadPS4.

| Item | Value |
| --- | --- |
| Draw | provenance 54 |
| Vertices | 925 |
| Indices | 3183 (16-bit) |
| Primitives | 1061 |
| Vertex stride | 12 bytes |
| Captured bytes | 23,466 |
| Replay device | NVIDIA GeForce RTX 3090 Ti |
| Replay result | `drawIndexed(3183, 1, 0, 0, 0)` submitted and completed |

## 1. Host-upload identity

SC-09R matched uploads by guest address and size, choosing the newest. That could confuse two
uploads of the same guest range into different host allocations.

The identity is now `UploadKey`: a monotonic sequence plus the host buffer, host offset, size and
guest range. The guest range is a consistency check, not the identity.

`FindHostUploadForBinding` requires the **host buffer and offset** the production descriptor was
built from to match, so a guest range re-uploaded into a different allocation does not resolve to
the earlier one.

### Thread safety

The ring was published by an atomic index with ordinary slot writes, so a reader could observe a
partially published record or race a slot replacement. It is now published under a mutex, held only
for the copy in and out of a slot, never across GPU execution.

## 2. Geometry capture

Recorded at the production binding points, not reconstructed afterwards:

- `BufferCache::BindVertexBuffers` records each binding with the host buffer, host offset, stride,
  input rate, divisor and the exact upload it resolved to.
- `BufferCache::BindIndexBuffer` records the host buffer, offset, index width, count, first index,
  base vertex, instance count and primitive topology.

Both retain the host bytes at capture time, so the package holds the version the draw consumed.

## 3. The package

Schema version 2. The single `replay_ready` flag is replaced by explicit dimensions:

```json
"readiness": {
  "shader_buffers_ready": true,
  "geometry_ready": true,
  "textures_ready": false,
  "pipeline_state_ready": false,
  "diagnostic_geometry_replay_ready": true,
  "full_draw_replay_ready": false,
  "limitation": "diagnostic geometry replay only: no textures, no original shaders, no material state"
}
```

A version 1 package is rejected rather than reinterpreted, because its `replay_ready` meant
shader-buffer completeness only.

## 4. Independent validation

`tools/validate_draw_package.py` re-derives every content hash from the blob and re-checks the
geometry arithmetic: index width, index count against captured bytes, and whether the highest
referenced index fits the captured vertex range.

```
VALID: schema=2 provenance=54 resources=6 shader_bindings=4 vertex_bindings=1 indexed=True
       bytes=23466 geometry_ready=True diagnostic_ready=True
```

Tampering with one byte of the blob:

```
INVALID: resource 1 content hash does not match its bytes
```

## 5. The standalone replay

`tools/geometry_replay/` is a separate CMake project. Its dependency list is:

```
vulkan-1.dll, MSVCP140D.dll, KERNEL32.dll, VCRUNTIME140D.dll, VCRUNTIME140_1D.dll, ucrtbased.dll
```

No shadPS4 DLL, no emulator, no guest address space. It creates its own instance, device, queue,
offscreen render target, pipeline and command buffer, uploads the captured bytes, and executes the
captured draw.

### What it does

1. Reads and validates the package, re-deriving every hash.
2. Decodes the indices from the captured bytes.
3. Picks the two vertex components with the largest extent as the diagnostic axes.
4. Fits them to the viewport and uploads.
5. Creates a 512x512 R8G8B8A8 offscreen target and a diagnostic pipeline.
6. Records `drawIndexed(3183, 1, 0, 0, 0)` and submits it.
7. Reads the target back and writes `replay.ppm`.

### What it does not do

- No textures, no original shaders, no material state.
- No claim of pixel equivalence with the game.
- The transform is diagnostic, not the game's.

## 6. Honest limits of the demonstration

**The selected draw is 2D screen-space geometry, not a 3D scene mesh.** Its third component spans
only `-0.087` to `191.37` while the first spans `-397` to `344`, and the rendered result is a flat
band. That is a real draw from the title screen, correctly captured and correctly replayed, but it
is a UI element rather than a scene mesh.

The larger 3D draws exist in the sample — one run showed a draw with 25,387 vertices, 48,744
indices and four attributes across two strides — but the selection picks the largest **complete**
draw, and in the final run that was the 925-vertex element. Provenance IDs shift between runs, so
the specific draw is not stable.

This is a genuine limitation of the demonstration, not of the capture. The machinery handles the
larger draw; the run simply did not select it.

## 7. What was fixed along the way

- **Retained prefix raised from 4 KB to 256 KB.** A scene mesh's index buffer exceeds 4 KB, so the
  smaller prefix made large draws unresolvable and they were skipped.
- **Upload ring reduced from 512 to 64 slots** to keep the memory bound at 16 MB with the larger
  prefix.
- **Draw selection requires complete geometry.** A draw with an unresolved vertex binding cannot
  produce a valid package, so it is not a candidate. The first attempt selected such a draw and the
  validator correctly refused it.

## 8. Portable components for SC-10 and the Native Renderer Foundation

| Component | Portability |
| --- | --- |
| `UploadKey` identity model | backend-neutral |
| `Readiness` dimensions | backend-neutral |
| Package schema v2 | backend-neutral |
| `validate_draw_package.py` | independent of any renderer |
| `tools/geometry_replay` | direct Vulkan, no abstraction library |

The package is deliberately backend-neutral: it describes resources, ranges, formats and draw
arguments, not Vulkan objects. An NRI implementation can consume the same geometry without
modification.

No abstraction library was chosen. That decision belongs to the Native Renderer Foundation track.

## 9. SC-10 handoff

SC-10 adds images and samplers through the same identity and versioning model. The lesson carries
forward: observe the host resource the production binding path selected, not the guest descriptor.
For images that means the texture cache's host image after tiling and conversion.

The `Readiness.textures_ready` dimension already exists and is false, so SC-10 has a place to
record its result without changing the schema's shape.

## 10. Atlas A-033

A-033 reports that two shader-resource constructors load a dirty-group participation mask into
`resource+0x200` and deserialize five DWORD-array lists at `+0x208`, `+0x218`, `+0x228`, `+0x238`
and `+0x248`, of which only three have identified consumers.

This does not change the capture. The capture reads the **resolved** descriptor at draw time, after
whatever indirection produced it. A future binding-provenance capture should record the effective
mask, the selected packed list entry, the guest object and the emitted descriptor, and preserve the
two unclassified lists rather than assuming they are unnecessary.

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

# validate
python tools/validate_draw_package.py <output dir>

# replay
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
./build-geometry-replay/deracine_geometry_replay <output dir>
```
