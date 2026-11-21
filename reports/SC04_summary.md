# SC-04: scene-verified shader association

## Outcome against the acceptance criteria

| Criterion | Status |
| --- | --- |
| 1. Startup capture's scene identified with evidence | **Met.** It is the PSVR health-and-safety warning. |
| 2. Classroom capture passes scene verification | **Not met.** The classroom was not reached. |
| 3. One draw with a corroborated VS/PS/pipeline association | **Not met.** The cross-thread join fails. |
| 4. Quantified matched/unmatched/ambiguous records | **Met.** 0 matched, 156 unmatched, 0 ambiguous. |
| 5. Default-OFF, safety, clean exit, tests pass | **Met.** 12 contract tests pass. |
| 6. Committed and pushed, `main` unchanged | **Met.** |

Two of six are unmet, and both are reported as failures rather than worked around.

## What was established

### The SC-02/SC-03 captures were the PSVR health warning

Established by a render-target dump taken at 50 s, inside the capture interval, triggered through
the runner's own `lab/user/dump_render_targets` flag.

The images show the "Please adjust the PlayStation Camera and VR Headset so the headset remains in
range" panel with a green rectangle and a Continue button. Against the reviewed classroom
reference, the similarity is **-0.089** at a 0.85 threshold.

That also explains why every SC-02 and SC-03 cell reported "the classroom scene was not reached":
the gate needs 400 draws and 500 Mpx, and those runs peaked at 39 draws and 248 Mpx.

SC-02 and SC-03's technical findings stand, because they concern the capture mechanism and the
shader-state chain. Their **scope** was wrong, and this report corrects it.

### The classroom is not reachable in this configuration

Five cells with autopress. The plateau that resembles the classroom band sits at **446 draws and
469 Mpx**, below the 500 Mpx gate. Across 245 samples, zero satisfied both conditions.

A render-target dump inside that plateau produced uniform black images. The scene-signature tool
refuses to identify a uniform image, correctly: a black frame carries no scene identity.

A 200 s run with dumps at 40, 70, 100 and 130 s found only loading screens. Across 205 samples,
exactly one reached the band, at 34 s. One sample is not two consecutive windows.

So the scene is not reached later either.

### The cross-thread join fails

With both streams enabled in one cell:

| Stream | Thread | Records |
| --- | --- | --- |
| RHI command capture | `RHIThread` | 156 indexed draws |
| GPU pipeline trace | `GpuCommandProcessor` | 1162 draws |

Index-count distributions do not overlap. The RHI capture sees only 3 and 6; the GPU trace sees
3, 4, 6, 10, 48, 54, 78 and 84. Using `(index_count, instance_count, primitive_type)` as the key,
the RHI stream has 7 distinct keys and the GPU stream 11, with **zero exact matches**.

The failure is sampling, not identity. The capture stopped at 512 records and dropped 456,595
commands while the GPU trace ran to its own budget, so the windows may not overlap in time. The
capture also hooks five wrappers while the trace sees every draw, including direct and bypass
paths.

## A real fix that came out of this

`classroom-run-1` crashed with an access violation at `0x800DFA8D0` after the capture buffer
filled. The cause was the hook querying the guest mapping once per word read. Replacing that with
a single span validation per command removed the crash in the following runs.

That is a genuine defect found and fixed, independent of the scene outcome.

## Instrumentation added

One bounded, default-OFF addition:

| Item | Value |
| --- | --- |
| Variable | `SHADPS4_DERACINE_CMD_CAPTURE_AFTER` |
| Meaning | start delay in seconds, so a capture can target a later scene |
| Bound | `MaxStartDelaySeconds = 600`, refused above rather than clamped |
| Default | absent means capture from the first command |

Plus the span-validation read path, which is a correctness fix rather than a feature.

## The next renderer-replacement dependency

The task asked for the next dependency. It is **not** the pixel shader, and it is not the vertex
declaration.

It is **a capture window that covers the same draws as the GPU trace**.

Everything downstream depends on that. Without a join, a captured draw cannot be connected to its
pipeline, and without that connection the shader identity is a pointer with no hash. The
dependency chain is:

```
capture window covering the traced draws
  -> parameter join validated by overlap
    -> VS/PS hash association
      -> pipeline identity
        -> first native Vulkan draw
```

The first step is one experiment: raise the capture bound and gate it on the same frame as the
trace. If the distributions then overlap, the join is a parameter match. If they do not, the
capture is structurally missing those draws and the answer is to hook the rasterizer's draw entry
instead of the command wrappers.

## What this report does not claim

- That any draw has a VS/PS/pipeline association. It does not.
- That the classroom was reached. It was not.
- That the health warning is representative of gameplay. It is not.

## Files

| File | Purpose |
| --- | --- |
| `reports/SC04_phase1_scene_provenance.md` | the health-warning identification and its evidence |
| `reports/SC04_phase2_classroom_attempt.md` | five cells, the band mismatch, the black plateau |
| `reports/SC04_phase3_join_failure.md` | the join test and why it fails |
| `src/src/core/rhi_command_capture_contract.h` | the start-delay bound |
| `src/src/core/rhi_command_capture.cpp` | span validation, start delay, export paths |
| `src/tests/test_rhi_command_capture_contract.cpp` | 12 contract tests |

## Reproducible checks

```
build-tests/tests/shadps4_rhi_command_capture_contract_test.exe --gtest_color=no   # 12 tests

# scene identity of a dump
python tools/benchmark/scene_signature.py compare <dump>/010_*5760x3240*.png `
  --reference tools/benchmark/reference/scene-classroom-menu.json
```

`scene_signature.py` needs Pillow. Not every interpreter on this machine has it.
