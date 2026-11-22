# SC-06 Phase 4: overlapping observation windows

## Result

The observation windows now overlap **between the capture and each probe layer**, but the two
probe layers do **not** overlap each other. That is a property of the probes, not of the
rendering.

The capture is the only observer that sees both the RHI/GNM side and the GPU side of the same
work.

## What was changed

Both probes previously observed from boot with a 128-record cap. On the title screen that meant
they filled during the health warning, long before the capture's 33 s start.

Two bounded start delays were added, mirroring the capture's:

| Variable | Bound | Default |
| --- | --- | --- |
| `SHADPS4_DERACINE_RHI_PROBE_AFTER` | 600 s, refused above | observe from first entry |
| `SHADPS4_DERACINE_CMD_CAPTURE_AFTER` | 600 s, refused above | capture from first command |

The PM4 hook also needed the gate. It runs on `GpuCommandProcessor` and decodes packets submitted
from boot, so without the gate it observed a different interval from the RHI entries.

## The overlap result

Run: `_build/headless/labs/sc06-title-run-3`, all three layers gated at 33 s.

| Layer | Thread | Records | Cap |
| --- | --- | --- | --- |
| RHI entry | `RHIThread` / `TaskGraphThreadNP 0` | 128 | 128 |
| GNM draw | same as RHI entry | 128 | 128 |
| PM4 decode | `GpuCommandProcessor` | 128 | 128 |
| Command capture | `RHIThread` | 5346 indexed draws | 16384 |

### Capture covers both probe layers

| Probe layer | Counts present in the capture |
| --- | --- |
| GNM `index_count` | **128 / 128** |
| PM4 `index_count` | **128 / 128** |

So the capture's window contains the work both probes observed.

### The probe layers do not cover each other

Joining GNM to PM4 on `(index_count, index_base)`:

| Outcome | Count |
| --- | --- |
| Unique match | **0** |
| Ambiguous | 0 |
| Unmatched | **128** |

Shared addresses between the two probe sets: **0**.

## Why the probe layers miss each other

The two threads consume the command stream at different rates, so an equal record cap lands at a
different stream position on each.

The GNM records begin with `index_count=6 @ 0x2000042800`, then `11904 @ 0x200e978000`, then a run
of `48 @ 0x20000c0000`. The PM4 records begin with `11133 @ 0x200bf50000`, `513 @ 0x201d03b800`,
`4548 @ 0x201d230000`.

Those are different draws. The RHI thread is producing draws that the GPU thread has not yet
decoded, and the GPU thread is decoding draws the RHI thread produced earlier.

That is expected: the GPU thread runs behind the producer. A 128-record cap on each side samples
two different 128-record slices of the same stream.

## What this establishes

### Demonstrated

- The capture's window contains the work both probes observed, so the capture is a valid
  superset observer.
- The probe layers are offset in the stream, so a fixed record cap cannot align them.
- The RHI-to-GNM pairing is 1:1 by sequence, 128/128, with no gaps.

### Not demonstrated

- That any specific GNM record corresponds to any specific PM4 record. The offset prevents it.

## The correct join strategy

The evidence says the join must be done **within the capture**, not between the probes.

The capture already records, per command, the list, context, sequence, and the shader-state
chain. What it does not record is the GPU-side resolved draw. So the missing link is a GPU-side
observation keyed to the same submission.

Three options, in order of cost:

1. **Record the PM4 packet's command-buffer address in the capture.** The GNM call receives
   `cmdbuf` (observed: `0x30000f0ae0`, `0x3000060698`, ...). If the capture also recorded the
   `cmdbuf` pointer for each draw command, and the PM4 hook recorded the packet address, the two
   would join on a real submission identity rather than on parameters.
2. **Raise the probe caps and gate them to a narrow window.** With a short enough window the two
   slices would overlap. This is cheap but fragile, because it depends on the two threads staying
   within the cap of each other.
3. **Add a monotonic submission ordinal.** The producer increments it; the consumer reads it from
   the packet. This is the most robust and the most invasive.

Option 1 is the smallest and uses data already present at both ends.

## Why not simply raise the caps

The task says not to increase the buffer and hope. The evidence supports that: the offset is
structural, not a capacity problem. Raising both caps to 16384 would make the slices larger and
more likely to overlap, but it would not establish correspondence, and it would still leave
ambiguity wherever the same `(index_count, index_base)` pair recurs.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
```

Then compare the `index_count` sets:

```
Select-String -Path shad_log.txt -Pattern 'DERACINE_GNM_DRAW_INDEX'
Select-String -Path shad_log.txt -Pattern 'DERACINE_PM4_DRAW_INDEX2'
Select-String -Path shad_log.txt -Pattern 'command_id=1 '
```

The capture contains both probe sets; the probe sets do not contain each other.
