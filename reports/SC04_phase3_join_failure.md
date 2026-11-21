# SC-04 Phase 3: the cross-thread join fails

## Result

The RHI capture and the GPU pipeline trace **cannot be joined** with the available keys. The two
streams observe different draw populations, and no parameter-based key produces a match.

This is reported as a failure rather than papered over with a nearest-match.

## What was measured

Cell: `_out/SC04_scene/join-run-1`, headless, no save, no input, `-Autopress 0`, with both the
SC-02 capture and the existing `SHADPS4_DBG_TRACE_HMD_FRAMES` pipeline diagnostic enabled.

| Stream | Thread | Records |
| --- | --- | --- |
| RHI command capture | `RHIThread` | 156 indexed draws |
| GPU pipeline trace | `GpuCommandProcessor` | 1162 draws |

## The key test

The candidate join key is the draw's own parameters: index count, instance count, primitive type.
Both sides expose them.

### Index-count distributions do not overlap

| RHI `NumPrimitives * 3` | Count |
| --- | --- |
| 3 | 3 |
| 6 | 153 |

| GPU `n` | Count |
| --- | --- |
| 3 | 184 |
| 4 | 304 |
| 6 | 122 |
| 10 | 61 |
| 48 | 124 |
| 54 | 61 |
| 78 | 122 |
| 84 | 122 |

The RHI capture sees only 3 and 6. The GPU trace sees eight distinct values, most of them far
larger. The two are not sampling the same draws.

### Exact key matches: zero

Using `(index_count, instance_count, primitive_type)` as the key:

| Stream | Draws | Distinct keys |
| --- | --- | --- |
| RHI | 156 | 7 |
| GPU | 1162 | 11 |
| **Exact matches** | | **0** |

Zero. Not a low match rate, not an ambiguous match: no key from one stream appears in the other.

## Why the join fails

Three independent reasons, in order of confidence.

### 1. The capture is capped and the trace is not

The RHI capture stopped at 512 records and dropped 456,595 commands. The GPU trace ran to its own
2500-entry budget. So the RHI stream is a small early window and the GPU stream is a later,
larger window. They may not overlap in time at all.

That alone would explain zero matches without any deeper problem.

### 2. The capture hooks five wrappers, the trace sees every draw

The capture only observes commands that pass through the five hooked `Execute` wrappers. The GPU
trace sees every draw the rasterizer issues, including direct and bypass paths. So the GPU stream
is a superset that includes draws the capture structurally cannot see.

### 3. The index counts differ by construction

The RHI payload's `NumPrimitives` is the UE4-side primitive count. The GPU trace's `n` is the
resolved index count at draw time. For a triangle list these should agree at a factor of 3, and
they do for the 3 and 6 cases. But the GPU trace also shows 48, 54, 78 and 84, which the capture
never recorded. Those are draws the capture did not observe.

## What this means

The task asked for a defensible cross-thread join, and to reject ambiguous associations rather
than selecting the nearest plausible match. There is nothing to reject here: there are no
candidate matches at all.

So the honest conclusion is that **the join is not available from these two streams as
configured**, and the reason is sampling, not identity.

## The smallest next experiment

The failure is caused by the capture window, not by a missing identity. Two bounded changes would
test that:

1. **Raise the capture bound** so the RHI stream covers the same interval as the GPU trace. 512
   records is far too few; the run dropped 456,595 commands.
2. **Gate the capture on the same frame** as the trace, using the existing
   `SHADPS4_DBG_TRACE_HMD_FRAMES` frame number, so both streams start at the same point.

If the distributions then overlap, the join is a parameter match and can be validated. If they
still do not, the capture is structurally missing the draws the trace sees, and the answer is to
hook the rasterizer's draw entry rather than the command wrappers.

That is one experiment with one variable changed, not a new investigation.

## What was not done

- No nearest-match pairing. The task forbids it and there is nothing to pair.
- No new instrumentation. The existing pipeline diagnostic was sufficient to establish the
  failure.
- No claim that any specific draw has a VS/PS/pipeline association. That claim is not supported.

## Reproducible checks

```
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DBG_TRACE_HMD_FRAMES='40'
$env:SHADPS4_EXIT_AFTER='70'; $env:SHADPS4_CLEAN_EXIT='1'
pwsh -File tools/benchmark/runner.ps1 -Exe <build>\shadPS4.exe -Out <fresh path> `
  -ShadowRoot <private shadow> -Lab <fresh lab> -Autopress 0 -TimeoutSeconds 110
```

Then compare the `command_id=1` payload index counts against the `TRACE draw` `n=` values. They
do not overlap.
