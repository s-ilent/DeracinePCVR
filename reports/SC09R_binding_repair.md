# SC-09R: shader binding correctness, host resource verification and replay package

## Result

The two critical defects in SC-09 are fixed, and one replay-ready package now exists for a real
title-screen draw.

| Item | SC-09 | SC-09R |
| --- | --- | --- |
| Bindings | 552 | 544 |
| Host-content verified | 0 | **544** |
| Guest-memory only | 552 | **0** |
| Package written | no | **yes, replay_ready=true** |

## Defect 1: Vulkan buffer identity was wrong

SC-09 walked the stages after production binding and indexed `buffer_infos[i]` with a per-stage
`i`. But `BindResources` accumulates `buffer_infos` across stages while `BindBuffers` clears
`buffer_bindings` per stage. So a pixel-stage descriptor could be reported with a vertex-stage
Vulkan buffer.

The fix records each binding **at the point production constructs it**, inside the descriptor-write
loop, where the unified binding number, the host buffer, the offset and the range all belong to the
same descriptor.

### The proof

Draw 16, which has three buffers per stage:

| Stage | Resource | Vulkan binding |
| --- | --- | --- |
| VS | 0 | 0 |
| VS | 1 | 1 |
| VS | 2 | 2 |
| PS | 0 | **5** |
| PS | 1 | 6 |
| PS | 2 | 7 |
| PS | 3 | 8 |

The pixel stage starts at 5, not 0, because the vertex stage's three buffers took 0-2 and its
images and samplers took 3-4. SC-09 would have reported PS resource 0 as binding 0 with the vertex
stage's buffer.

## Defect 2: guest-memory hashing was reported as verification

SC-09 hashed `vsharp.base_address` directly. That proves the guest bytes were readable, not that
they are the contents of the host buffer the draw consumed.

### The new status model

| Status | Meaning |
| --- | --- |
| `GuestMemoryObserved` | guest bytes sampled, host equivalence unverified |
| `HostContentVerified` | bytes correspond to the bound host resource |
| `HostContentCaptured` | verified host bytes retained for export |
| `MetadataOnly` | descriptor captured without contents |
| `Partial` | required range not fully available |
| `Unsupported` | capture cannot safely support the resource |
| `Invalid` | inconsistent or stale observation |

`Complete` is gone. It conflated "the guest range was readable" with "the host content is known".

### How host verification is established

`BufferCache::ObtainBuffer` has a stream-buffer path that copies guest memory into a host-visible
buffer:

```cpp
const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
memory->CopySparseMemory(device_addr, data, size);
stream_buffer.Commit();
```

That copy is the authoritative content the draw reads. The capture now records it at that point,
retaining a bounded prefix of the **host** bytes and hashing exactly what it retained. A later
binding of the same guest range resolves to that upload and is reported as
`HostContentVerified`.

The isolated read-const path was inspected first, as the task suggested, but it is gated on two
DSR-specific shader hashes (`kDsrSkinnedVs032fd69c`, `kDsrSkinnedVs27904a0c`) and never fires for
Déraciné. The stream-buffer path is the one this title actually uses.

### What is still not verified

- **GPU-written buffers** are `Unsupported`. Copying them from guest memory without a readback
  would read data the GPU may still be writing. No readback was added.
- **Special buffers** (GDS, flatbuf, BDA page table, fault buffer, shared memory) are
  `MetadataOnly`. They are not guest memory.
- **Ranges larger than the retained prefix** are not claimed as verified. `FindHostUpload` refuses
  a request the retained bytes cannot cover.

## Defect 3: clamped ranges

Production uses `memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize())`. SC-09 hashed the
unclamped descriptor extent. The record now carries both `requested_*` and `clamped_*`, plus a
`range_clamped` flag. In this sample no range was clamped, so the distinction is present but
unexercised.

## Defect 4: placeholder descriptor locations

SC-09 exported `sgpr_base = 0` and `dword_offset = 0` as if they were recovered. They are not
fields of `BufferResource` at all — that struct has only `sharp_idx`, an index into the shader's
flattened user-data buffer. The misleading fields are removed rather than left as zeros that imply
a measurement.

## The package

`_build/sc09r-package/` for provenance 1:

```json
{
  "schema_version": 1,
  "scene": "classroom-title",
  "provenance_id": 1,
  "pipeline": { "vs_hash": 10580915761153262368, "ps_hash": 9646496578772824588,
                "primitive_type": 4, "is_indexed": true, "num_indices": 54 },
  "resources": [
    { "id": 1, "clamped_size": 16, "status": "host_content_verified", "byte_length": 16 },
    { "id": 2, "clamped_size": 64, "status": "host_content_verified", "byte_length": 64 }
  ],
  "shader_buffers": [
    { "stage": 0, "vk_binding": 0, "resource_ref": 1 },
    { "stage": 3, "vk_binding": 3, "resource_ref": 2 }
  ],
  "capture_status": { "replay_ready": true }
}
```

Two resources, 80 bytes, both host-verified. The VS binding is 0 and the PS binding is 3 — the
corrected identity.

### Independent validation

`tools/validate_draw_package.py` is a separate Python implementation. It re-derives every content
hash from the blob rather than trusting the exporter's claim.

```
VALID: schema=1 provenance=1 resources=2 bindings=2 bytes=80 replay_ready=True
```

Tampering with one byte of the blob:

```
INVALID: resource 1 content hash does not match its bytes
```

## Accounting

| Counter | Value |
| --- | --- |
| Draws | 128 |
| Dropped | 0 |
| Bindings | 544 |
| Guest-memory observed | 0 |
| Host-content verified | 544 |
| Host-content captured | 0 |
| Metadata-only | 0 |
| Partial | 0 |
| Unsupported | 0 |
| Invalid | 0 |
| Range-clamped | 0 |
| Storage-classified | 544 |
| Read-const | 535 |
| Draws with push constants | 128 |
| Total bytes hashed | 751,984 |
| Byte budget | 67,108,864 |

## Tests

63 tests pass across five suites, including 12 new package-validator tests and 2 new status tests.

The validator tests exercise the production `ValidatePackage` and cover: wrong schema version,
missing provenance, duplicate ids, dangling references, byte-length mismatch, hash mismatch,
verified-status-without-bytes, guest-observed-for-replay, metadata-only bindings, shared resources,
and zero-byte resources claiming content.

## Limitations

- **No geometry replay.** The package contains shader buffers, not vertex or index data. The
  geometry bytes are captured by SC-08's vertex-input path, which records addresses and formats but
  not contents. A geometry replay needs those bytes retained the same way this phase retained
  shader buffers.
- **No GPU readback.** GPU-written resources remain unsupported.
- **The retained prefix is 4096 bytes per upload.** A larger resource is not claimed as verified.
- **One package per run.** `MaxPackages` is defined but the writer stops at the first success.
- **The Atlas A-024/A-025/A-026/A-027 findings were not implemented.** They describe the guest-side
  producer of the user-data map. This phase captures the consumer view, which is what a replay
  needs, and no mismatch was found between them.

## The Atlas comparison

A-024 reports that the stage-2 map is not always shader-resource metadata, with a fallback to
`[shader_state+0x1E0]+0xB0` and an active path into writer-owned scratch state. A-025 identifies
the decoder at `0x02F5D290` and the encoded value `(index < 16) ? (0x8000 | index) : (index - 16)`.
A-026 identifies the format as `GnmInputUsageSlot` and `metadata+0xC2` as immediate constant-buffer
bindings. A-027 recovers the trailer-based lookup.

None of this changes the capture. The capture reads the **resolved** descriptor at draw time, after
whatever indirection produced it, so a different map source is invisible to it. The nine
`sharp_idx` values observed (0, 4, 8, 12, 16, 20, 24, 28, 32 — all multiples of four) are DWORD
indices into the flattened buffer, consistent with the A-025 encoding.

A-025 also corrects an earlier claim: `context+0x468` and `graphics_state+0xC468` are different
base objects, so the matching suffix is not an independent cross-check. That correction is
accepted; the SC-09 report's cross-check claim was wrong.

## Next milestone: SC-10

Texture and sampler recovery. The machinery to reuse:

| Facility | Reuse |
| --- | --- |
| `BeginDrawBindings` / `RecordBinding` / `EndDrawBindings` | same per-draw observation |
| `CaptureStatus` | same seven states |
| `RetainHostBytes` / `ReadRetainedBytes` | same bounded pool |
| `DrawPackage` schema | extend with images and samplers |
| `validate_draw_package.py` | extend the checks |

The central lesson carries forward: **observe the actual host resource the production binding path
selected.** For images that means the texture cache's host image after tiling and conversion, not
raw guest memory. Hashing tiled guest bytes and calling it the effective texture would repeat the
SC-09 mistake in a harder form.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'
```

Then:

```
python tools/validate_draw_package.py <output dir>
```
