# SC-05 Investigation B: the primitive-type audit and the corrected join

## Result

SC-04's join analysis was **wrong in two ways**, and correcting it changes the conclusion from
"zero matches" to "three matches, all ambiguous".

The root cause of the failure is not the capture window. It is that the two streams observe
**different draw populations**, and the one key they share is too common to identify a draw.

## Correction 1: the index-count distributions do overlap

SC-04 said they "do not overlap". That was wrong. They share the values 3 and 6.

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

The correct statement is that the RHI stream is a **subset** of the GPU stream's value range, not
disjoint from it.

## Correction 2: the primitive types were compared across incompatible enums

SC-04 compared the UE4 payload's `PrimitiveType` directly against the AMD register value. Those
are different enumerations and must not be compared numerically.

### The UE4 enum

From the pinned UE4.18.3 `RHIDefinitions.h`:

```cpp
enum EPrimitiveType
{
    PT_TriangleList,      // 0
    PT_TriangleStrip,     // 1
    PT_LineList,          // 2
    PT_QuadList,          // 3
    PT_PointList,         // 4
    PT_1_ControlPointPatchList,  // 5
    ...
};
```

So `PT_TriangleList = 0`, which is what the capture recorded.

### The AMD enum

From `src/src/video_core/amdgpu/regs_vertex.h`:

```cpp
enum class PrimitiveType : u32 {
    None = 0,
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriangleList = 4,
    TriangleFan = 5,
    TriangleStrip = 6,
    ...
};
```

So AMD `TriangleList = 4`, which is what the GPU trace recorded as `prim=4`.

### The mapping

| UE4 | Value | AMD | Value |
| --- | --- | --- | --- |
| `PT_TriangleList` | 0 | `TriangleList` | 4 |
| `PT_TriangleStrip` | 1 | `TriangleStrip` | 6 |
| `PT_LineList` | 2 | `LineList` | 2 |
| `PT_QuadList` | 3 | `QuadList` | 19 |
| `PT_PointList` | 4 | `PointList` | 1 |

The two enums agree only on `LineList = 2`, by coincidence. Comparing them numerically is
meaningless.

### Why the values differ at all

`regs.primitive_type` is a hardware context register that the guest writes with AMD values
directly. It is not derived from the UE4 enum by the emulator. The UE4 backend translates
`PT_*` to the AMD value before writing the register, and that translation happens inside the
guest's own PS4 RHI code, which is exactly the code this investigation is trying to observe.

## The corrected join

Normalizing both streams to a semantic primitive name, and converting `NumPrimitives` to an index
count only for triangle lists:

### RHI distinct keys

| Key | Count |
| --- | --- |
| `TriangleList\|6\|32` | 66 |
| `TriangleList\|6\|8` | 36 |
| `TriangleList\|6\|16` | 16 |
| `TriangleList\|6\|64` | 16 |
| `TriangleList\|6\|128` | 15 |
| `TriangleList\|6\|2` | 4 |
| `TriangleList\|3\|1` | 3 |

### GPU distinct keys

| Key | Count |
| --- | --- |
| `TriangleStrip\|4\|1` | 243 |
| `TriangleList\|48\|1` | 124 |
| `TriangleList\|3\|1` | 123 |
| `TriangleList\|6\|1` | 122 |
| `TriangleList\|78\|1` | 122 |
| `TriangleList\|84\|1` | 122 |
| `TriangleList\|180\|1` | 62 |
| `RectList\|3\|1` | 61 |
| `TriangleList\|54\|1` | 61 |
| `TriangleStrip\|10\|1` | 61 |
| `TriangleStrip\|4\|32` | 61 |

### Matches

Exactly **one** key is shared: `TriangleList|3|1`.

| Stream | Records with that key |
| --- | --- |
| RHI | 3 |
| GPU | 123 |

So normalization produces **3 candidate matches against 123 candidates each**. That is not a
match. It is the most common key in the GPU stream.

**Verdict: 3 ambiguous, 0 unambiguous, 153 RHI draws unmatched.**

## What this establishes

The failure has two independent causes, and SC-04 named only the weaker one.

### Cause 1: the populations differ (demonstrated)

The RHI capture sees instance counts 2, 8, 16, 32, 64, 128. The GPU trace sees instance count 1
almost exclusively, plus one `TriangleStrip|4|32`.

That is not a sampling-window artifact. It is a **different set of draws**. The capture is
observing instanced draws that the GPU trace does not report as instanced, and the GPU trace is
reporting triangle strips and rect lists that the capture never sees.

### Cause 2: the shared key is not discriminating (demonstrated)

Even where the keys agree, 123 GPU draws share it. A parameter-only join cannot identify a draw
when the key is that common.

### Cause 3: the capture window (plausible, not demonstrated)

SC-04 attributed the failure primarily to non-overlapping windows. That remains plausible, since
the capture dropped 456,595 commands, but it is **not** the demonstrated cause. The population
mismatch above would produce zero unambiguous matches even with perfect window alignment.

## What this means for the next experiment

Raising the capture bound would not fix the join. The evidence says the two streams describe
different draws, and the only shared key is ambiguous.

The next step must add a discriminator, not more volume. Candidates, in order of cost:

1. **The index-buffer address.** The RHI payload carries `IndexBuffer` at `+0x10`. The GPU trace
   does not currently log it, but `regs.index_base_address` is available at the draw. Adding it
   to the trace line would give a per-draw identity that is far more selective than a count.
2. **A monotonic draw ordinal.** If both streams increment a counter per draw on their own side,
   the ordinals can be compared once the populations are known to align.
3. **The PM4 packet address.** The A3 work already correlates GNM calls to PM4 packets by index
   count and index base. That is the same discriminator as option 1, reached from the other side.

Option 1 is the smallest: one extra field in an existing diagnostic line, no new hook.

## Reproducible checks

```
# the two enums
grep -A6 'enum EPrimitiveType' RHIDefinitions.h          # UE4: PT_TriangleList = 0
grep -A8 'enum class PrimitiveType' src/src/video_core/amdgpu/regs_vertex.h  # AMD: TriangleList = 4

# the join
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DBG_TRACE_HMD_FRAMES='40'
```

Then normalize both streams to a semantic primitive name before comparing. Comparing the raw
enum values is the error SC-04 made.
