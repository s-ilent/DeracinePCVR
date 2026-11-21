# SC-03: are the captured draws depth-only?

## Result

**No.** The captured draws are colour-writing, and they have a real fragment shader.

The hypothesis was reasonable and worth testing, but the evidence refutes it on three
independent counts.

## What was measured

One cell with the existing `SHADPS4_DBG_TRACE_HMD_FRAMES` pipeline diagnostic enabled alongside
the SC-02 capture. That diagnostic already logs the fragment-shader hash, the colour-target mask,
the depth address, and the depth-test/write state per draw, so no new instrumentation was needed.

Cell: `_out/SC02_draw_shader/trace-run-1`, headless, no save, no input, clean exit.

| Item | Count |
| --- | --- |
| Pipeline traces | 1161 |
| Captured command records | 512 |
| Captured indexed draws | 156 |

## Evidence 1: the captured draws have a colour target

Walking the captured records in sequence order and tracking the most recent `SetRenderTargets`:

| Tracked colour target | Draws |
| --- | --- |
| `0x100C09BD80` | 155 |
| `0x10100A1C80` | 1 |
| **null** | **0** |

| Tracked depth target | Draws |
| --- | --- |
| **null** | **156** |

So every captured draw has a non-null colour target and **no** depth target. That is the
opposite of the depth-only hypothesis.

The representative draw's `rt_count=0` and `depth=0x0` in the SC-02 record are not evidence of
a null target. The draw command does not repeat render-target state, so a consumer must track
the last `SetRenderTargets` per list. SC-02's report said this; this cell confirms it.

## Evidence 2: the pipeline has a real fragment shader

Across the 1161 traced draws:

| Fragment shader | Draws |
| --- | --- |
| `0x0` (none) | 183 |
| `0x1EE50CD7240559BB` | 122 |
| `0x3D246647A653BEA8` | 62 |
| `0x4496C84956245785` | 61 |
| `0x5C28AAD32E4358DD` | 61 |
| `0x6F502A0396D11FBD` | 62 |
| `0x85DF3D8770C6720C` | 61 |
| `0x8804619047EC9209` | 61 |
| `0x8804619047EC920A` | 182 |
| `0xB096888E3AAA51E7` | 123 |
| `0xBA40C4D165F2A1EA` | 122 |
| `0xE40E595E30E352AB` | 61 |

Eleven distinct fragment shaders. Only 183 of 1161 draws have none.

## Evidence 3: the depth-only draws are a different set

The genuinely depth-only draws are identifiable and distinct:

```
TRACE draw idx=true n=84 inst=1 prim=4 pipeline=true cb0=0x2007000000 1024x1024 s1
  cb_mode=0 mask=0x0 db=0x2030000000 z_en=true z_wr=true zfunc=6 samples=2/2
  vs=0x687E608ADFD3B0B5 ps=0x0 ... zfmt=3
```

Those have `mask=0x0`, a non-null depth buffer, `z_en=true`, `z_wr=true`, and `ps=0x0`. There
are 61 of them, plus 122 more with the same shape. They are a real category, and the capture did
not select them.

## Classification of the traced draws

| Fragment shader | Colour target | Depth target | z_en | z_wr | Draws |
| --- | --- | --- | --- | --- | --- |
| yes | yes | yes | false | false | 610 |
| yes | yes | no | false | false | 246 |
| no | no | yes | true | true | 122 |
| yes | no | yes | false | false | 61 |
| yes | no | yes | true | true | 61 |
| no | no | yes | false | false | 61 |

So the scene contains both categories, and the capture's 156 draws fall in the colour-writing
rows.

## What this means for the pixel shader

The null `sceGnmSetPsShader350` call is **not** explained by these draws being depth-only.

That leaves the original SC-02 question open, but narrowed. The draws write colour and the
pipeline has a fragment shader, so a pixel shader *is* bound at draw time. The null setter call
must therefore be either:

1. A different binding path that this capture does not observe, or
2. A call whose register writes precede it and carry the real program address.

The audit's caution applies directly: a non-zero address before the null call does not prove the
shader remains bound, and a null call does not prove no shader is bound. The pipeline trace is
the authority, and it says a fragment shader is present.

## Correction to the SC-02 handoff

The SC-02 handoff said the next dependency was the pixel-shader identity, and implied the null
setter was the blocker. That framing was incomplete. The pipeline already knows the fragment
shader hash. What is missing is the **association** between a captured draw and that hash, not
the hash itself.

That is a smaller problem than it looked.

## The revised next step

Map the captured vertex-shader resource to its shader-recompiler hash, then do the same for the
fragment shader.

The pipeline trace gives `vs` and `ps` hashes per draw. The capture gives the vertex-shader
resource pointer per draw. If those two can be joined, the association is complete without
recovering any new binding path.

The join is plausible because both are per-draw and both are stable across consecutive draws.

### The join is not free

The two mechanisms run on **different threads**:

| Mechanism | Thread |
| --- | --- |
| Command capture | `RHIThread`, native id 31724 |
| Pipeline trace | `GpuCommandProcessor` |

So the records cannot be paired by position or by thread. A join needs a shared key, and the
obvious candidate is the draw's own parameters: `NumPrimitives`, `StartIndex`, `NumInstances`,
and the index-buffer address. Those are visible on both sides.

That is a real constraint, not a detail. The next experiment must emit a join key explicitly
rather than assuming the two streams line up.

### The bounded experiment

Emit, in one record, the pipeline's `vs`/`ps` hashes and the captured vertex-shader resource
pointer, keyed by the draw parameters. Then check whether the pairing is one-to-one across a
sample.

If it is, the association is complete. If it is not, the two streams are observing different
draw populations and the capture surface needs widening before any join is meaningful.

## What was not done

- No new instrumentation was added. The existing `TRACE draw` diagnostic was sufficient.
- No renderer change.
- The capture itself is unchanged from SC-02.

## Reproducible checks

```
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DBG_TRACE_HMD_FRAMES='40'
$env:SHADPS4_EXIT_AFTER='80'; $env:SHADPS4_CLEAN_EXIT='1'
pwsh -File tools/benchmark/runner.ps1 -Exe <sc02 build>\shadPS4.exe -Out <fresh _out path> `
  -ShadowRoot <private game shadow> -Lab <fresh lab> -Autopress 0 -TimeoutSeconds 120
```

Then classify with:

```
Select-String -Path shad_log.txt -Pattern 'TRACE draw'
```

The fields are `mask=` (colour target mask), `db=` (depth address), `z_en=`, `z_wr=`, `vs=`,
`ps=`.
