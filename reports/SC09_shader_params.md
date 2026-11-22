# SC-09: shader parameters, uniform buffers and version-correct resource capture

## Result

Shader user data, buffer bindings and resource contents are captured for verified draws. One
title-screen run produced **128 draws, 552 bindings, all complete, 763,600 bytes hashed**, with
0 dropped, 0 partial, 0 unsupported and 0 invalid.

Every stage uses exactly 16 user-data registers and a 16-word push-constant block.

## The audit

### The layers, kept distinct

| Layer | Where it lives | Changes per draw |
| --- | --- | --- |
| Shader metadata | `Shader::Info::buffers`, `images`, `samplers` | no |
| Pipeline specialization | `GraphicsPipelineKey` | no |
| User-data registers | `Shader::Info::user_data` | **yes** |
| Push constants | `Shader::PushData` | **yes** |
| Buffer descriptors | `BufferResource::GetSharp()` reading user-data SGPRs | **yes** |
| Vertex-fetch descriptors | `VertexAttribute::GetSharp()` | **yes** |
| Vulkan descriptor bindings | `set_writes[].dstBinding` | no, but the bound buffer does |

The important correction to a natural assumption: **a vertex-fetch descriptor is not a uniform
buffer**. `VertexAttribute::GetSharp()` reads the same user-data SGPR space that
`BufferResource::GetSharp()` reads, but the two are different resource classes with different
consumers. SC-08 captured the former; this phase captures the latter, and the two are reported
separately.

### Binding numbers are not resource indices

`BindResources` walks stages in order and increments a single `binding.unified` counter across
buffers, images and samplers. So the Vulkan binding for a resource is its position in that walk,
not its index within `stage.buffers`.

The capture reproduces the walk rather than assuming `binding == resource_index`. In draw 1 the
pixel stage's resource 0 lands at Vulkan binding 2, because the vertex stage's two buffers took
bindings 0 and 1 first.

### Where the resolved values come from

The capture runs in `Rasterizer::Draw` after `BindResources` has built `set_writes`,
`buffer_infos` and `buffer_bindings`. Every recorded value is therefore the one the renderer
actually supplied to Vulkan, not a parallel reconstruction.

## Draw-time user data

Draw 1, vertex stage (`pgm=0x82C730EBC710047E`):

```
ud=[20280000,1c400000,7329d67f,94000204,02dfe000,00000000,00000000,00000000,
    00000092,00fff000,05000000,00000000,00030020,00000020,00000000,00000000]
push=[42800000,42800000,42800000,c2800000,0,0,0,0,0,0,0,0,0,0,0,0]
```

Draw 1, pixel stage (`pgm=0x82C730EB4A0404ED`):

```
ud=[06cf0cc0,00000020,00030000,00000020,2580e800,00100020,000000b2,20077fac,
    0e800000,00100020,00000003,20077fac,00000000,00000000,00000000,00000000]
push=[42800000,42800000,42800000,c2800000,0,0,0,0,0,0,0,0,0,0,0,0]
```

The two stages share the push block but have entirely different user data. That is the distinction
the task asked for, and it is visible in the data.

Only the registers the shader declares are emitted, so a stage using four registers does not
produce a 32-register dump. In this sample every stage declares 16.

## Buffer bindings

Draw 1 has four bindings across two stages:

| Stage | Resource | Base | Size | Stride | Records | Vk binding | Status | Hash |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| VS | 0 | `0x2002220000` | 32 | 16 | 2 | 0 | complete | `0x1CF08E1EB3AA0515` |
| VS | 1 | `0x202580E800` | 2848 | 16 | 178 | 1 | complete | `0x9204899613DB8C8E` |
| PS | 0 | `0x200E800000` | 48 | 16 | 3 | 2 | complete | `0x350D8A4FB9769A67` |
| PS | 1 | `0x202580E800` | 2848 | 16 | 178 | 3 | complete | `0x9204899613DB8C8E` |

The last two rows are the same guest buffer bound in both stages. They carry the **same content
hash**, which is the shared-resource case working correctly rather than being duplicated.

All 408 bindings are `storage=true` and `readconst=true`. That is the renderer's classification:
`IsStorage()` returns true because `uniform_buffer_limit` is 0, and `used_as_readconst` marks the
isolated-snapshot path. **None of these are uniform buffers in the Vulkan sense**, and the report
does not call them that.

## The version model

The buffer cache exposes **no per-resource generation counter**. `Buffer` has `CpuAddr`,
`Offset`, `SizeBytes` and `Handle`, plus internal watch lists, but nothing that identifies a
version of the contents.

So the honest version identity is:

| Component | Source |
| --- | --- |
| Draw ID | SC-07R provenance |
| Guest address range | `vsharp.base_address`, `vsharp.GetSize()` |
| Host buffer identity | `vk_buffer_handle` |
| Host offset and range | `vk_offset`, `vk_range` |
| Buffer-cache resource | `buffer_id` |
| Content hash | FNV-1a over the captured bytes |
| Capture point | draw time, after descriptor construction |
| Completeness | `CaptureStatus` |

**No generation counter was invented.** Claiming one would assert a guarantee the cache does not
make. The content hash is the substitute, and it is sufficient to detect that two captures of the
same range differ.

### What is not captured, and why

- **GPU-written buffers** are marked `Unsupported`. Copying them from guest memory without a
  readback would be reading data the GPU may still be writing. This phase adds no readback.
- **Special buffers** (GDS, flatbuf, BDA page table, fault buffer, shared memory) are marked
  `MetadataOnly`. They are not guest memory and have no meaningful guest range.
- **Null or zero-size descriptors** are marked `MetadataOnly`.

In this sample none of those cases occurred: all 408 bindings were ordinary guest buffers and all
were captured completely.

## The Atlas A-022 comparison

The independent Binary Atlas reports two deferred RHI resource commands:

| Item | Atlas value |
| --- | --- |
| Execute methods | `0x0CACDA0`, `0x0CACF80` |
| Command vtables | `0x03EDFCA0`, `0x03EDFCC0` |
| User-data DWORDs | four, at `resource+0x50` |
| Context caches | `+0x568/+0x928`, `+0x608/+0x92C` |
| Graphics-context vtable | `0x03EDFF18`, shader recorder `0x0CB5130` at `+0xC8` |

### They are a different vtable family from our deferred commands

Our five hooked wrappers are at `0x13A68B0`–`0x13A6BB0` with vtables
`0x03F26A08`–`0x03F26A88`. The Atlas commands are at `0x0CACDA0`/`0x0CACF80` with vtables
`0x03EDFCA0`/`0x03EDFCC0`.

Those are not the same objects. Our wrappers are the deferred command-list entries; the Atlas
commands are RHI context methods that write shader user data directly. Both feed the same
`Shader::Info::user_data` that this phase captures, but through different paths.

### What this means for the capture

The capture reads the **effective** user data at draw time, so it is correct regardless of which
path wrote it. The Atlas commands are a *producer* of that state; our snapshot is the *consumer*
view. Adding hooks on them would tell us which UE4 call set a register, not what the register
value was — and we already have the value.

So no hooks were added. The Atlas finding is recorded as the likely origin of the user-data
writes, and it is the right starting point for the UE4-provenance question in section 9, not for
the effective capture.

The Atlas also notes that `Execute` releases resource references, so a snapshot must happen
synchronously. That is consistent with our design: we capture at draw time from the resolved
descriptor, not by retaining a resource pointer.

## The Atlas A-023 comparison

A-023 reports the shader-map provenance and a deferred upload-command family.

### The shader map

Two binding paths (`0x0CB132A`, `0x0CC372B`) assign `state+0xC468 = shader_resource+0xB0`. The
generic user-data binder then reads `map+0xC2+2*slot`, which is `shader_resource+0x172+2*slot` for
stage zero. That is **16 slots of u16**, each holding a register index, with `0xFFFF` for unmapped
and bit 15 selecting direct PM4 emission versus shadow-register update.

Our capture records `sharp_idx`, which is the index into the shader's flattened user-data buffer
where the descriptor begins. Across 552 bindings it takes exactly nine values:

| `sharp_idx` | Slot equivalent | Bindings |
| --- | --- | --- |
| 0 | 0 | 18 |
| 4 | 1 | 134 |
| 8 | 2 | 119 |
| 12 | 3 | 26 |
| 16 | 4 | 117 |
| 20 | 5 | 117 |
| 24 | 6 | 11 |
| 28 | 7 | 8 |
| 32 | 8 | 2 |

Two observations follow, and they are consistent with A-023 rather than contradicting it:

1. **`sharp_idx` is always a multiple of 4**, so it is a DWORD index into the flattened buffer,
   not a slot number. Dividing by four gives the slot-equivalent index, and all nine values fall
   inside the 16 slots the Atlas map provides.
2. **Nine of sixteen slots are in use** in this sample. The map is not fully populated, which is
   what a `0xFFFF`-for-unmapped design implies.

So the Atlas map is the **slot-to-register indirection**, and our `sharp_idx` is the **resolved
register offset** after that indirection. They are two views of the same binding, at different
stages. Our capture already holds the post-indirection value, which is the one a replay needs.

The `state+0xC468` offset matches our existing `ContextShaderStateOffset = 0x468` constant, which
SC-02 recovered independently. That is a useful cross-check on both findings.

### The deferred upload family

Six Execute methods (`0x0CAAE70`, `0x0CAB100`, `0x0CAB140`, `0x0CAB1C0`, `0x0CAB180`, `0x0CAB200`)
write through context buffer pointers `+0xAE0` to `+0xB08`, sharing copy routine `0x0CAAEB0`.
Producer `0x0CC6BA0` snapshots up to 16 bytes into the command-list allocator and enqueues vtable
`0x03EDF7E0`.

This is a **different mechanism** from anything this phase captures. It is a deferred upload of up
to 16 bytes into a context buffer, not a shader descriptor binding. It plausibly carries small
uniform payloads, but nothing in our capture identifies which context buffer a given draw read.

No hooks were added, for the same reason as A-022: the effective values are already captured, and
this family is a producer path. It becomes relevant when the UE4-provenance question is pursued,
because it is a candidate for how small constant blocks reach the GPU.

The Atlas notes that the shader-map resource and the separate upload-owner metadata are **not
proven to be the same object**. Our data does not resolve that either: we observe the resolved
descriptors, not the objects that produced them.

## UE4 shader-parameter provenance

Not recovered. The strongest candidates are now:

| Candidate | Address | Role |
| --- | --- | --- |
| Shader-map binding | `0x0CB132A`, `0x0CC372B` | writes `state+0xC468` |
| Generic user-data binder | A-022 `0x0CACDA0`, `0x0CACF80` | writes four DWORDs at `resource+0x50` |
| Deferred upload | `0x0CAAE70` family | up to 16 bytes into context buffers |
| Shader recorder | `0x0CB5130` at vtable `0x03EDFF18 + 0xC8` | pipeline state |

None was pursued in this phase, because the effective GPU-side state is complete and the task
prioritised it. The mapping
`UE4 parameter → PS4 RHI object → GPU descriptor → Vulkan binding` remains open at the first
arrow.

## Coverage

| Item | Value |
| --- | --- |
| Draws | 128 |
| Dropped | 0 |
| Bindings | 552 |
| Complete | 552 |
| Metadata-only | 0 |
| Partial | 0 |
| Unsupported | 0 |
| Invalid | 0 |
| Storage-classified | 552 |
| Read-const | 543 |
| Draws with push constants | 128 |
| Total bytes hashed | 763,600 |
| Byte budget | 67,108,864 |

## Limitations

- **No GPU readback.** GPU-written buffers are marked unsupported rather than captured. This is
  the principal gap for a replay-ready package.
- **No geometry replay.** The optional experiment was not attempted. The captured bytes are
  hashed but not exported as a package, because the schema work below is the prerequisite.
- **No UE4 provenance.** The first arrow of the mapping is open.
- **The content hash is not a generation counter.** Two captures with the same hash are strong
  evidence of identical contents, but a hash collision is theoretically possible and the cache
  offers no ordering guarantee.
- **`buffer_cpu_addr` and `buffer_bytes` are zero** for every binding, because the buffer-cache
  `Buffer` object was not reachable from the capture point. The guest range is recorded instead,
  which is what the descriptor actually addresses.
- **`sharp_idx` is a DWORD index, not a slot number.** It is always a multiple of four. Dividing
  by four gives the slot-equivalent index, which is what the Atlas A-023 map addresses.

## Next milestone: SC-10

Texture and sampler recovery. The reusable pieces from this phase:

| Facility | Reuse |
| --- | --- |
| `ShaderParamCapture::DrawSnapshot` | extend with image and sampler arrays |
| `CaptureStatus` | same five states apply to textures |
| `HashCapturedBytes` | same content-identity approach |
| `ReserveCaptureBytes` | same global byte budget |
| Draw-time capture point | same `Rasterizer::Draw` site |

SC-10 needs, per image: descriptor words, texture-cache resource identity, format, dimensions, mip
and array selection, swizzle, sampling state, layout and synchronization, and the Vulkan
image/view/sampler bindings. The version problem is harder there, because images are tiled and
detiled, so the content hash must be taken after the cache has produced the host image.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'
```

Then:

```
Select-String -Path shad_log.txt -Pattern 'DERACINE_SHADER_PARAM_SUMMARY'
Select-String -Path shad_log.txt -Pattern 'DERACINE_SHADER_PARAM_DRAW'
Select-String -Path shad_log.txt -Pattern 'DERACINE_SHADER_STAGE'
Select-String -Path shad_log.txt -Pattern 'DERACINE_SHADER_BINDING'
```
