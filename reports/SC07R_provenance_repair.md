# SC-07R: draw provenance correctness repair

## Result

The defects identified in review were real, and all are fixed. The chain is now exact:

```
UE4 deferred command ID → RHI invocation → constructed packet
  → submission ID + buffer + offset → decoded packet → pipeline → Vulkan draw
```

One title-screen run: **128 records, 128 with a deferred-command identity, 128 constructed,
128 submitted, 128 decoded, 128 recorded, 0 expired, 0 pending, 0 ambiguous, 0 inconsistent.**

## The defects and their fixes

### 1. Submission identity was not used

The old matcher searched for the first packet with matching index count and address, and only
conditionally compared the offset. Submission and buffer identity were never compared.

The matcher is now a pure function, `MatchDecodedPacket`, in
`rhi_entry_probe_contract.h`. It resolves on `submission_id + source_buffer + packet_offset`
only. Index count and address are consistency checks applied **after** identity is established,
never a fallback.

`NoteSubmission` now returns the submission id, and `SubmitGfx` passes it into
`ProcessGraphics(dcb, ccb, provenance_submission, provenance_source_buffer)`, so the identity
travels with the queued graphics task rather than through a timestamp or an ordinal.

The indirect path at `liverpool.cpp:1016` uses the default arguments (zeros), so indirect and
nested packets stay unassociated by construction.

### 2. Offset zero was treated as unknown

The old code used `record.packet_offset != 0` as a proxy for "offset known", so a packet at
offset zero fell through to parameter matching.

`PacketKey` now carries `packet_offset` with no sentinel meaning. Zero is a valid identity, and
`ProvenanceMatcher.OffsetZeroIsAValidIdentity` covers it.

### 3. Records stayed eligible after decoding

`PacketKey.consumed` is set when a decode claims the packet, and the matcher reports
`AlreadyConsumed` rather than matching again.

### 4. Ambiguity was resolved by taking the first candidate

The matcher counts all identity matches and returns `Ambiguous` when more than one survives. It
also distinguishes `Inconsistent` (identity matched, fields disagreed) from `NoCandidate`.

### 5. The deferred-command link did not exist

This was the largest gap. Two bugs had to be fixed:

**The vtable was read from the wrong place.** `frame->rdi` is the command object, not its vtable.
The vtable is the first word of that object and must be read from guest memory. Reading the
register directly made `ClassifyCommandVtable` return non-draw for every command, so the scope was
never published.

**The scope was published too late.** It was set at the end of the capture body, after the
start-delay and budget checks. It is now published at wrapper entry, before any of those, because
the identity of the executing command does not depend on whether this run is recording it.

The scope is claimed once at the RHI entry and cleared there, so one invocation cannot inherit
another's identity. A stack-depth guard refuses a claim at or above the wrapper's entry pointer,
which is what makes a wrapper that returns without issuing the RHI call leave an unusable scope
rather than a stale one.

Result: `with_command=128 without_command=0`. Direct RHI calls that bypass the wrapper would
report `without_command`, and that distinction is explicit rather than invented.

### 6. Draw outcomes were reported before the draw

`NoteDrawRecorded` ran immediately after the pipeline check. A resolved pipeline is not a recorded
draw: the paths between can still filter or shrink the operation.

The report now runs after `cmdbuf.drawIndexed`/`cmdbuf.draw`, and the identity is cleared there so
a later unrelated draw cannot inherit it. `NoteDrawSkipped` covers the pipeline-unavailable path.

### 7. Parameter semantics were conflated

`record.primitive_type` held the GNM `type` argument and was described as UE4 topology. Those are
now separate fields:

| Field | Meaning |
| --- | --- |
| `gnm_type` | GNM draw `type` argument (observed `1`) |
| `gnm_flags` | GNM draw `flags` argument (observed `0x0`) |
| `ue4_primitive_type` | UE4 `EPrimitiveType` from the command payload |
| GPU `prim` | AMD hardware topology, reported at draw time |

The GNM `type` argument's meaning is **not** established. It is recorded as an opaque value and
not interpreted.

### 8. Accounting conflated events with unique records

The summary now reports both, separately:

| Counter | Value |
| --- | --- |
| Records | 128 |
| Packets registered | 128 |
| Constructed | 128 |
| Submitted | 128 |
| Decoded records | 128 |
| Recorded records | 128 |
| Skipped records | 0 |
| Failed records | 0 |
| Rejected records | 0 |
| Pending | 0 |
| Expired | 0 |
| With deferred command | 128 |
| Without deferred command | 0 |
| Child draws | 128 |
| Decoded events | 128 |
| Recorded events | 128 |
| Unmatched packets | 1,015,414 |
| Ambiguous | 0 |
| Inconsistent | 0 |
| No submission identity | 0 |

Every record has an explainable terminal state. `child_draws=128` over 128 records means each
guest packet produced exactly one Vulkan draw.

## SC-07's 140-versus-128 was duplicate correlation

SC-07 reported 140 recorded draws against 128 decoded packets and attributed the difference to
expansion. That was wrong.

The old matcher did not consume records, so a packet could be claimed more than once. With
consumption enforced, the count is exactly 128 records and 128 child draws. There is no expansion
in this scene.

## The index-offset question

SC-06 claimed `StartIndex` is not folded into the GNM address. SC-07 correctly flagged that as
unproven. This run provides the comparison SC-07 lacked: **the same resource with different
addresses**.

Resource `0x100155ACA8`:

| Record | Index count | Index address |
| --- | --- | --- |
| 4 | 84 | `0x2006CE7000` |
| 5 | 546 | `0x2006CE7150` |

The delta is `0x150` = 336 bytes = **168 16-bit indices**, which is exactly `2 × 84`, the previous
draw's index count.

Resource `0x10015511A8` shows the same pattern: delta `0x1638` = 5688 bytes = 2844 indices, an
even number consistent with 16-bit elements.

So the index data is packed sequentially at 2 bytes per index, and the address advances by the
previous draw's count. That is consistent with `StartIndex` being applied as an offset into the
buffer rather than folded into the base address, but it does **not** prove it: no draw in this
sample has a non-zero `StartIndex` on a shared resource. The question stays open and is deferred
to vertex-input recovery, as directed.

## Coverage

| Item | Value |
| --- | --- |
| Records | 128 |
| Distinct VS | 17 |
| Distinct PS | 19 |
| Distinct pipelines | 19 |
| Distinct submissions | 5 |
| Identical geometry, distinct ids | `240@0x20089FEC00` × 12, `2538@0x2015F74000` × 10 |

A representative fully associated record:

| Layer | Value |
| --- | --- |
| Deferred command | `command_id=32503`, `command_type=1` (indexed draw), list `0x100BAA8C00` |
| RHI invocation | sequence 1, thread 51060, guest thread 36 |
| Index resource | `0x1017AF5028` |
| Index count / address | 54 / `0x200E8A4800` |
| GNM | `type=1`, `flags=0x0`, `cmdbuf=0x30000B0DE8` |
| Submission | 9063, offset `0xDE8`, no copy |
| State | 4 (`Recorded`), 1 child draw |

## Tests

27 tests pass, including 13 adversarial matcher tests that call the production
`MatchDecodedPacket`:

| Test | Result |
| --- | --- |
| Identical parameters in different submissions | correct submission selected |
| Identical offsets in different buffers | correct buffer selected |
| Same buffer reused across generations | old generation cannot claim |
| Valid packet at offset zero | exact match succeeds |
| Missing submission identity | no match |
| Unsubmitted packet | not eligible |
| No parameter fallback | identity wins over parameters |
| Duplicate candidates | rejected as ambiguous |
| Consumed record | cannot match again |
| Inconsistent packet fields | rejected |
| Copied buffer | original identity survives |
| Out-of-order decoding | resolves by identity |
| Empty table | no candidate |

## Limitations

- **Command-buffer copying is not live-verified.** `copied_dcb=0x0` for every record, so the copy
  path was not taken. The handling is unit-tested (`CopiedBufferKeepsTheOriginalIdentity`) but the
  live run does not exercise it.
- **Indirect and nested PM4 paths are unsupported.** They pass zero submission identity and are
  counted as unmatched. The title screen's use of them is not quantified.
- **The GNM `type` argument's meaning is not established.** It is recorded, not interpreted.
- **`StartIndex` semantics remain open.** The evidence is consistent with an offset but does not
  prove it.
- **Unmatched packets are high** (1,015,414) because the GPU thread decodes the whole stream while
  the record set is bounded at 256. They are counted, never guessed.

## The reusable contract for vertex-input work

| Function | Purpose |
| --- | --- |
| `CurrentDrawProvenanceId()` | record id for the draw being processed |
| `NoteDrawRecorded(...)` | report actual pipeline state after the draw is emitted |
| `NoteDrawSkipped(id, reason)` | report a decoded packet that produced no draw |
| `ClearCurrentDrawProvenance()` | release the identity after reporting |
| `ExportProvenance()` | post-guest summary and per-record lines |

A vertex-input investigation attaches at `CurrentDrawProvenanceId()` and reads the pipeline's
fetch shader and vertex inputs there.

### Next milestone: vertex declarations

Reuse points that already exist:

| Facility | Provides |
| --- | --- |
| `GraphicsPipeline::GetFetchShader()` | `Shader::Gcn::FetchShaderData` |
| `GraphicsPipeline::GetVertexInputs()` | attributes, bindings, divisors, guest buffers |
| `Shader::Gcn::VertexAttribute` | semantic index, VGPR, component count, offset, format |

The gap is the **vertex declaration itself**. The capture hooks `SetStreamSource` but does not
decode its payload into an attribute layout, and a bound stream does not identify its layout. The
declaration must come from the fetch shader, which is already resolved at draw time.

The smallest next step is to record, per verified draw, the fetch shader's attribute list and the
guest vertex-buffer descriptors, then correlate them with the UE4 stream state.

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
