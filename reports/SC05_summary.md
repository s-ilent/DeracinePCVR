# SC-05: title-screen progression and shader-join audit

## Outcome

Both SC-04 conclusions were **partly wrong**, and correcting them changes the next step.

| SC-04 claim | Verdict |
| --- | --- |
| The classroom is unreachable with automated input | **Confirmed, with a better explanation** |
| The join fails because the capture windows are disjoint | **Wrong as the primary cause** |

## Investigation A: the title screen

The classroom was not reached. The PSVR health warning was not dismissed by any input tried.

### The input schedule

`SHADPS4_DBG_AUTOPRESS` presses **Cross only**, held 600 ms every 3 s from 8 s to 45 s. Twelve
presses. That is the entire automated input.

### The 446-draw plateau is a black screen

The plateau begins at the 36 s press and ends at the 42 s press. A dump inside it is black: the
brightest sRGB display target has mean **9.11**, against the classroom reference's **40.46** and
the health panel's **178.85**.

The 624-draw / 502.7-Mpx cell is also black.

So the plateau is a black transition with a classroom-like draw count. SC-04 correctly refused to
call it the classroom, but for the wrong reason: it blamed the Mpx gate, when the image was
simply black.

### Three controlled cells

Using the repository's scripted-input system (`SHADPS4_INPUT_SCRIPT`) with individual timed
presses:

| Cell | Input | Result |
| --- | --- | --- |
| 1 | one Cross at 25 s | warning persisted to 60 s |
| 2 | Cross, Options, Circle, Start, Cross | warning persisted to 68 s |
| 3 | two 2 s Cross holds | warning persisted to 85 s |

No cell changed the draw count. Draws stayed at 37-39 throughout.

### Why it does not clear

The warning says "Please adjust the PlayStation Camera and VR Headset so the headset remains in
range." That is a **positioning** instruction, not a confirmation prompt.

Supporting evidence: it renders at 24 passes against the classroom's ~80, the Move controllers
are registered, and the headset is reported worn. So the blocker is most likely **tracking data**,
not input.

The established path to the classroom is a staged save slot, which is the headless gameplay
harness rather than `runner.ps1`.

## Investigation B: the join audit

SC-04's join analysis was wrong in two ways.

### Correction 1: the index counts do overlap

SC-04 said they do not. They share 3 and 6. The RHI stream is a subset of the GPU stream's range.

### Correction 2: the primitive types were compared across incompatible enums

| UE4 (`RHIDefinitions.h`) | Value | AMD (`regs_vertex.h`) | Value |
| --- | --- | --- | --- |
| `PT_TriangleList` | 0 | `TriangleList` | 4 |
| `PT_TriangleStrip` | 1 | `TriangleStrip` | 6 |
| `PT_LineList` | 2 | `LineList` | 2 |
| `PT_QuadList` | 3 | `QuadList` | 19 |
| `PT_PointList` | 4 | `PointList` | 1 |

They agree only on `LineList = 2`, by coincidence. `regs.primitive_type` is a hardware context
register the guest writes with AMD values directly, so the translation happens inside the guest's
own PS4 RHI code.

### The corrected join

Normalizing both streams to a semantic primitive name, and converting `NumPrimitives` to an index
count only for triangle lists:

| Stream | Records | Distinct keys |
| --- | --- | --- |
| RHI | 156 | 7 |
| GPU | 1162 | 11 |
| Shared keys | | **1** (`TriangleList\|3\|1`) |

| Stream | Records with the shared key |
| --- | --- |
| RHI | 3 |
| GPU | 123 |

**Verdict: 3 ambiguous, 0 unambiguous, 153 unmatched.**

## Root-cause assessment

Three causes, separated by evidence strength.

### Demonstrated: the populations differ

The RHI capture sees instance counts 2, 8, 16, 32, 64, 128. The GPU trace sees instance count 1
almost exclusively, plus one `TriangleStrip|4|32`.

That is not a window artifact. It is a different set of draws. The capture observes instanced
draws the trace does not report as instanced, and the trace reports triangle strips and rect
lists the capture never sees.

### Demonstrated: the shared key does not discriminate

123 GPU draws share `TriangleList|3|1`. A parameter-only join cannot identify a draw when the key
is that common.

### Plausible, not demonstrated: the capture window

SC-04 attributed the failure primarily to disjoint windows. That remains plausible, since the
capture dropped 456,595 commands, but the population mismatch above would produce zero
unambiguous matches even with perfect alignment.

## The next experiment

**Add the index-buffer address to the GPU trace line.**

The RHI payload already carries `IndexBuffer` at `+0x10`. The GPU trace does not log it, but
`regs.index_base_address` is available at the draw. That is one extra field in an existing
diagnostic line, no new hook.

Why this and not more capture volume: raising the bound would not fix the join, because the
populations differ and the shared key is ambiguous. The evidence says the missing information is
a per-draw identity, not more records.

If the index-buffer address still produces ambiguity, the next discriminator is a monotonic draw
ordinal on both sides, compared once the populations are known to align.

## Acceptance criteria

| Criterion | Status |
| --- | --- |
| 1. Startup capture's scene identified with evidence | **Met.** PSVR health warning. |
| 2. Classroom capture passes scene verification | **Not met.** Not reachable by input. |
| 3. One draw with a corroborated VS/PS/pipeline association | **Not met.** Join is ambiguous. |
| 4. Quantified matched/unmatched/ambiguous | **Met.** 3 ambiguous, 0 unambiguous, 153 unmatched. |
| 5. Default-OFF, safety, clean exit, tests pass | **Met.** Unchanged from SC-04. |
| 6. Committed and pushed, `main` unchanged | **Met.** |

Three of six unmet or partly met. The two failures are reported as failures.

## What was not done

- No new instrumentation. The audit used existing diagnostics and the existing scripted-input
  system.
- No threshold weakening. The classroom similarity threshold and Mpx gate are untouched.
- No renderer, VR, or OpenXR change.
- No claim that any draw has a pipeline association.

## Files

| File | Purpose |
| --- | --- |
| `reports/SC05_investigation_a_title_progression.md` | input timeline, controlled cells, blocker |
| `reports/SC05_investigation_b_primitive_audit.md` | enum mapping, corrected join, ambiguity |
| `reports/SC05_summary.md` | this document |

## Reproducible checks

```
# the autopress schedule
grep -A6 'struct AutoPressSchedule' src/src/core/devtools/autopress.h

# the two primitive enums
grep -A6 'enum EPrimitiveType' RHIDefinitions.h
grep -A8 'enum class PrimitiveType' src/src/video_core/amdgpu/regs_vertex.h

# a controlled press
echo "25 0.3 cross" > input.txt
$env:SHADPS4_INPUT_SCRIPT=<abs path>\input.txt
```
