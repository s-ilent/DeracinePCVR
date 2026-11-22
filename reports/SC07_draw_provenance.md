# SC-07: end-to-end draw provenance

## Result

The complete chain is established and verified on the title screen:

```
UE4 deferred RHI command → PS4 RHI method → GNM packet construction
  → PM4 submission → GPU packet decoding → Vulkan pipeline and draw
```

One run produced **128 records, 128 packets constructed, 128 submitted, 128 decoded, 140 draws
recorded, 0 skipped, 0 pending, 40 expired**.

## The mechanism

The identity is a monotonic record id assigned when the RHI entry is observed. It travels in
thread-local state, so nothing is derived from an independent ordinal on the consumer side.

| Step | Where | What happens |
| --- | --- | --- |
| 1 | RHI entry `0x0CBB9C0` | Allocate record id, store in `thread_local g_pending_provenance_id` |
| 2 | `sceGnmDrawIndex` | Attach `cmdbuf`, index count, index address, topology to the record |
| 3 | After packet write | `RegisterConstructedPacket` claims the id and clears it |
| 4 | `SubmitGfx` | `NoteSubmission` assigns a generation and matches packets by buffer span |
| 5 | PM4 decode | `CapturePm4DrawIndex2At` matches by packet offset, sets the current id |
| 6 | `Rasterizer::Draw` | `NoteDrawRecorded` reports actual pipeline state |

### Why the id is cleared at step 3, not step 2

The first implementation cleared it in the GNM capture, which produced `packets=0`: the
registration that follows successful construction had nothing left to claim. The id now survives
until the packet actually exists, and is consumed there. That also enforces one packet per RHI
entry, so a second GNM call on the same thread cannot inherit a stale id.

### Only constructed packets are registered

`RegisterConstructedPacket` is called after `WriteTrailingNop<3>`, inside the branch that already
validated `size == 10`, `index_addr != 0`, `index_addr & 1 == 0` and `flags & 0x1ffffffe == 0`. A
rejected GNM call returns `-1` before that point, so it cannot produce a packet association.

## Command-buffer copying

`SubmitGfx` optionally calls `CopyCmdBuffers`. The provenance path records both the original guest
pointer and the copied span, and matches a packet against either:

```cpp
const bool inside_copied = packet.cmdbuf >= dcb_base && packet.cmdbuf < dcb_end;
const bool inside_original = copied && original_dcb != 0 && ...;
```

In this run `copied_dcb=0x0` for every record, so the copy path was not taken. The handling is
present and the configuration is explicit, not assumed.

## Generation-aware identity

A packet address is not globally unique. The identity is
`submission_id + cmdbuf + packet_offset`, and the run demonstrates why that matters:

| Buffer | Submissions |
| --- | --- |
| `0x30000F0AE0` | 9014, **9050** |

The same guest buffer is reused across two submissions and the two uses are distinguished by
generation. 14 distinct submissions were observed.

## A fully associated draw

Record 1:

| Layer | Value |
| --- | --- |
| Scene | classroom with main-menu overlay |
| RHI sequence | 1 |
| Thread | 47100, guest thread 36 |
| `cmdbuf` | `0x30000F0AE0` |
| Packet offset | `0xAE0` |
| Submission | 9014 |
| Index count | 6 |
| Index address | `0x2000042800` |
| RHI index resource | `0x10044E49E8` |
| Topology | `prim=1` (UE4 `PT_TriangleStrip`), GPU `prim=4` (`TriangleList`) |
| VS hash | `0xF9FC1744D9228EEA` |
| PS hash | `0xF9FC1F6B9DE05E7E` |
| Pipeline | `0x7C467446EADF2187` |
| Colour mask | `0xF` |
| Depth | none, `z_en=false`, `z_wr=false` |
| Outcome | recorded |

Note the topology: the RHI payload says `prim=1` and the GPU says `prim=4`. That is the
enum-domain difference SC-05 established, now visible within a single associated record.

## Coverage

| Item | Value |
| --- | --- |
| Records | 128 |
| Packets constructed | 128 |
| Submitted | 128 |
| Decoded | 128 |
| Draws recorded | 140 |
| Skipped | 0 |
| Pending | 0 |
| Expired | 40 |
| Unmatched packets | 1,000,165 |
| Distinct pipelines | 10 |
| Distinct VS | 10 |
| Distinct PS | 9 |

Draws recorded (140) exceeds records (128) because one guest draw can expand into more than one
rasterizer operation. That one-to-many relationship is preserved rather than collapsed.

Unmatched packets are high because the GPU thread decodes the whole stream while the record set is
bounded at 256. They are counted, not logged, and never assigned a guessed identity.

## Negative controls

| Control | Result |
| --- | --- |
| 1. Identical geometry stays distinguishable | **Pass.** `6@0x2000042800` appears 80 times with distinct ids 1, 14, 27, 40, 53-128 |
| 2. Buffer reuse causes no false association | **Pass.** `0x30000F0AE0` used in submissions 9014 and 9050, kept separate |
| 3. Concurrency does not swap identities | **Pass.** Records span guest threads 7 and 36 and multiple native threads, each with its own id |
| 4. Failed GNM calls produce no packet | **Pass.** Registration is inside the validated branch |
| 5. Unassociated packets get no guessed id | **Pass.** 1,000,165 counted as unmatched, `provenance_id=0` |
| 6. Copy path handled or rejected | **Handled.** Both pointers matched; not taken in this run |
| 7. Pending/dropped/unmatched accounted | **Pass.** 40 expired, 0 pending, 1,000,165 unmatched, all reported |

## The index-offset question

SC-06 claimed `StartIndex` is not folded into the GNM address. This run does not settle it, and
the report should not have stated it as established.

What is observed: record 1 has `index_count=6` and `index_addr=0x2000042800`. The earlier
health-warning run had `index_count=3` with `index_addr=0x200004280C`. Those are **different
resources in different runs**, which is exactly the comparison the task warns against.

The `0xC` difference is consistent with six 16-bit indices, but consistency is not proof. Settling
it needs multiple draws on the **same** resource with different start indices, plus static
instruction-level evidence from `0x0CBB9C0`. That is deferred, and it does not invalidate the
association: the record carries both the RHI resource and the resolved GNM address, so the
translation is captured either way.

## Instrumentation

| Property | Status |
| --- | --- |
| Default OFF | `SHADPS4_DERACINE_DRAW_PROVENANCE` unset produces no records |
| Bounded | 256 records, 256 packets, fixed arrays |
| Thread-safe | `std::mutex` around record and packet tables |
| Non-mutating | reads guest state only; no packet contents changed |
| No per-draw disk I/O | all logging is post-guest or matched-only |
| Clean-exit export | `ExportProvenance` in `Emulator::Shutdown` |
| Reversible | no guest detours added; reuses the existing entry observer |

One correction was needed during development: logging every PM4 decode exhausted the 50 MB log
budget before the summary was written. Only matched packets are logged now; unmatched ones are
counted.

## The reusable observation contract

For subsequent research, the available interface is:

| Function | Purpose |
| --- | --- |
| `CurrentDrawProvenanceId()` | the record id for the draw being processed |
| `NoteDrawRecorded(...)` | report actual pipeline state at draw time |
| `NoteDrawSkipped(id, reason)` | report a decoded packet that produced no draw |
| `ExportProvenance()` | post-guest summary and per-record lines |

A new investigation attaches to `CurrentDrawProvenanceId()` and reads whatever additional state it
needs, without touching the correlation machinery.

## Next milestone: vertex declarations

The smallest remaining field set for a native indexed Vulkan draw:

| Field | Where it should come from |
| --- | --- |
| Vertex declaration identity | UE4 `SetStreamSource` / declaration command, not yet hooked |
| Stream-to-attribute mapping | the fetch shader, `GraphicsPipeline::GetFetchShader()` |
| Attribute formats and offsets | `GetVertexInputs()` already resolves these |
| Vertex-buffer resource and GPU address | same translation problem as the index buffer |
| Stride and instancing | `regs.vertex_buffers[]` plus the fetch shader |
| Index format and offset | `regs.index_buffer` type; the offset question above |
| Shader input interface | `key.stage_hashes` plus the SPIR-V input list |

`GetVertexInputs()` and `GetFetchShader()` already exist and are the natural reuse points. The
capture currently hooks `SetStreamSource` but does not decode its payload into a declaration.

## What this does not claim

- That every PM4 packet is covered. Only direct DCB `DrawIndex2` packets are supported; indirect
  and nested graphics tasks are not.
- That the index-offset semantics are settled.
- That gameplay uses the same paths.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'
```

Then:

```
Select-String -Path shad_log.txt -Pattern 'DERACINE_DRAW_PROVENANCE_SUMMARY'
Select-String -Path shad_log.txt -Pattern 'DERACINE_DRAW_PROVENANCE_RECORD'
Select-String -Path shad_log.txt -Pattern 'DERACINE_DRAW_PROVENANCE provenance_id'
```
