# SC-08: vertex declarations, geometry resources and effective vertex inputs

## Result

Effective vertex and index input state is now captured for verified draws. One title-screen run
produced **128 snapshots, 282 attributes, 0 dropped**, spanning 123 16-bit-index draws and 5
32-bit-index draws, with 110 instanced draws.

The index-offset question is **settled**: the address delta is `index_offset × index_size`, proven
on three non-zero-offset draws.

## The audit

### What is immutable per shader

`FetchShaderData` is parsed from the vertex shader and cached on the pipeline. Its attributes are
fixed for a given shader: `semantic`, `dest_vgpr`, `num_elements`, `sgpr_base`, `dword_offset`,
`instance_data`, `inst_offset`, `data_format`, `num_format`.

### What is draw-time

`GetSharp(vs_info)` reads the buffer descriptor from user-data SGPRs, which the guest rewrites per
draw. So the **resolved address, stride and record count change between draws sharing a pipeline**.
That is why the snapshot is taken at the draw, not at pipeline creation.

### What belongs to the pipeline key

The pipeline key carries stage hashes, primitive type, sample counts, colour write masks and the
fixed-function state. It does **not** carry vertex buffer addresses.

### The translation the renderer performs

`GetVertexInputs()` assigns each fetched attribute its own Vulkan binding, using the semantic
index, and supplies an attribute offset of **zero**. The fetch instruction offset is folded into
the resolved buffer address by `GetSharp()`:

```cpp
buffer.base_address += inst_offset;
```

So an interleaved UE4 layout is reproduced as N single-attribute bindings whose base addresses
differ by the element offsets. The original stream grouping is **not** reconstructed by the
renderer, and the snapshot records that faithfully rather than inventing a grouping.

### Instance rates

`GetStepRate()` returns `InstanceIdType`. `None` maps to per-vertex; every other value maps to
per-instance with a divisor from `step_rate_0`/`step_rate_1`. Those step rates live on
`RuntimeInfo::vs_info`, not on `Shader::Info`, so the pipeline now stores them and exposes
`GetStepRate0()`/`GetStepRate1()` for the observer.

### Shaders without a conventional fetch shader

`GetVertexInputs()` returns early when `fetch_shader` is absent or has no attributes. The snapshot
records `attributes=0` for such draws. In this sample all 128 draws had attributes, so no
vertex-pulling path was observed — but the code path exists and is reported rather than assumed
away.

## Index format and offset semantics

### The format is a hardware register, not an address delta

`BindIndexBuffer` reads `regs.index_buffer_type.index_type`:

| `IndexType` | Value | Element size | Draws |
| --- | --- | --- | --- |
| `Index16` | 0 | 2 bytes | 123 |
| `Index32` | 1 | 4 bytes | 5 |

The SC-07R report inferred 4 bytes per index from `336 / 84`. **That inference was wrong.** The
two draws in that example were not adjacent in the buffer, so the delta had nothing to do with the
index count.

### The offset relationship, proven

`BindIndexBuffer` computes:

```cpp
const VAddr index_address = regs.index_base_address.Address<VAddr>() + index_offset * index_size;
```

Three draws in this sample have a non-zero `index_offset`, and all three match exactly:

| `index_offset` | `index_size` | Expected delta | Actual delta | Match |
| --- | --- | --- | --- | --- |
| 16 | 4 | 64 | 64 | yes |
| 136 | 4 | 544 | 544 | yes |
| 56 | 4 | 224 | 224 | yes |

So `StartIndex` is **not** folded into the GNM base address. It is applied at bind time, in
elements, scaled by the element width.

### What remains unresolved

- **Base vertex semantics.** `GetDrawOffsets()` returns `regs.index_offset` as the vertex offset
  when non-zero, otherwise the fetch shader's `vertex_offset_sgpr`. The two are conflated in the
  existing code and this sample does not separate them.
- **Index endian conversion.** `index_swap_mode` is 0 for every sampled draw, so no swap path was
  exercised.
- **Primitive restart.** Not observed in this sample.

## Effective vertex inputs

### A simple draw (provenance 1)

| Field | Value |
| --- | --- |
| VS / PS | `0x92D6F6D31DDF4320` / `0x85DF3D8770C6720C` |
| Pipeline | `0x79C0A8BC787C9D09` |
| Topology | `prim=4` (AMD `TriangleList`) |
| Indices | 54, `index_type=1` (32-bit), base `0x20088E9800` |
| Attributes | 4, all stride 40 |

| Semantic | VGPR | Elements | Base | Stride | Data fmt | Num fmt | Vk format |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 4 | 4 | `0x200223C000` | 40 | 14 | 7 | 109 |
| 1 | 8 | 2 | `0x200223C010` | 40 | 11 | 7 | 103 |
| 2 | 12 | 2 | `0x200223C018` | 40 | 11 | 7 | 103 |
| 3 | 16 | 4 | `0x200223C020` | 40 | 10 | 0 | 37 |

The bases differ by 0, 16, 24, 32 within a 40-byte stride. That is a single interleaved stream
reproduced as four bindings.

### A multi-stream draw (provenance 25)

7 attributes across **three** distinct strides:

| Stride | Attributes | Base addresses |
| --- | --- | --- |
| 28 | 4 | `0x2006A60008`, `0x2006A60000`, `0x2006A60004`, `0x2006A60014` |
| 8 | 2 | `0x20069F8000`, `0x20069F8004` |
| 0 | 1 | `0x2000041000` (4 records) |

The stride-0 attribute with 4 records is a constant/uniform-style fetch, not a per-vertex stream.
That is a distinct case the renderer handles without special-casing, and the snapshot records it
as-is.

## Coverage

| Item | Value |
| --- | --- |
| Snapshots | 128 |
| Dropped | 0 |
| With attributes | 128 |
| Without attributes | 0 |
| Indexed | 128 |
| 16-bit indices | 123 |
| 32-bit indices | 5 |
| Instanced | 110 |
| Total attributes | 282 |
| Max attributes per draw | 7 |

## The UE4 declaration: not recovered

The original UE4 vertex declaration object was **not** located. What is established:

- `SetStreamSource` at `0x13A6930` is hooked and its payload is captured, but the payload fields
  have not been decoded into stream index, resource, offset and stride.
- A bound stream does not identify its attribute layout; the layout comes from the fetch shader.
- The renderer never reconstructs the UE4 stream grouping, so the effective snapshot cannot
  recover it by observation alone.

The strongest candidate path for a future investigation is the shader-state binding that produces
the fetch shader, since that is where the attribute list is fixed. This phase did not pursue it,
because the effective declaration is sufficient for geometry replay and the task prioritised it.

**Effective geometry recovery is complete for the sampled draws. UE4 semantic recovery is not.**

## What this enables

For each sampled draw the following is now known well enough to reconstruct the vertex fetch:

- Every attribute's semantic, destination VGPR, component count and format.
- The resolved buffer address, stride and record count.
- The Vulkan binding, format and input rate the renderer used.
- The index format, base address, offset and element size.
- The draw arguments actually passed to Vulkan.

## Limitations

- **Geometry bytes are not captured.** The snapshot records addresses and formats, not the vertex
  data itself. Capturing bytes safely requires observing the buffer-cache version the renderer
  consumed, which this phase did not implement. Saving bytes read later from guest memory would
  risk stale data, so it was not done.
- **No geometry replay.** The optional replay experiment was not attempted, because the byte
  capture it depends on is absent.
- **Base vertex semantics remain conflated** with the index offset in `GetDrawOffsets()`.
- **No vertex-pulling path observed**, so that case is untested.

## Next milestone: SC-09

Shader constants, user data and uniform buffers.

The reusable interface is `CurrentDrawProvenanceId()` plus the snapshot record. SC-09 attaches at
the same point and reads the user-data registers and buffer descriptors the shader declares.

The geometry package becomes one component of a future complete draw package:

| Phase | Deliverable |
| --- | --- |
| SC-08 | effective vertex/index inputs and geometry resources |
| SC-09 | shader constants, user data and uniform buffers |
| SC-10 | texture and sampler bindings |
| SC-11 | fixed-function pipeline and attachment state |
| SC-12 | independent, material-correct replay of one captured draw |

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'
```

Then:

```
Select-String -Path shad_log.txt -Pattern 'DERACINE_VERTEX_INPUT_SUMMARY'
Select-String -Path shad_log.txt -Pattern 'DERACINE_VERTEX_INPUT_DRAW'
Select-String -Path shad_log.txt -Pattern 'DERACINE_VERTEX_ATTRIBUTE'
```
