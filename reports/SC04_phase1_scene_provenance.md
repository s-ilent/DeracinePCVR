# SC-04 Phase 1: scene provenance of the SC-02/SC-03 captures

## Result

The SC-02 and SC-03 captures were **not** the classroom. They were the **PSVR health-and-safety
warning screen**: the "Please adjust the PlayStation Camera and VR Headset so the headset remains
in range" panel with a green rectangle and a Continue button.

That is established by direct visual evidence from a render-target dump taken during the capture
interval, not by inference from draw counts or timing.

## Evidence

Cell: `_out/SC04_scene/startup-run-1`, headless, no save data, no input, `-Autopress 0`,
`SHADPS4_EXIT_AFTER=70`, `SHADPS4_CLEAN_EXIT=1`.

A render-target dump was triggered at 50 s by writing the runner's own
`lab/user/dump_render_targets` flag, so the dump falls inside the capture interval rather than
after it.

### The dump is contemporaneous with the capture

| Time | Draws | Mpx | Passes |
| --- | --- | --- | --- |
| 48 s | 38 | 240.0 | 24.1 |
| 49 s | 39 | 248.1 | 24.7 |
| **50 s (dump)** | **39** | **247.3** | **24.9** |
| 51 s | 38 | 239.4 | 24.0 |
| 52 s | 39 | 247.6 | 24.8 |

The capture summary for the same run reports `draw_indexed=156 draw=3 stream_source=313
render_targets=14 viewport=26`, and the SC-02/SC-03 runs reported the identical distribution.
So the dump and the command records describe the same interval.

### The images

| File | SHA-256 | Content |
| --- | --- | --- |
| `008_2020000000_5760x3240_R8G8B8A8Unorm_s1.png` | `05EB10596A0BB514EE7FF5063A1820DBB01F1DECC51A7ABF2C2C35164437BFD4` | health warning, stereo pair |
| `020_202d000000_1920x1200_R8G8B8A8Srgb_s1.png` | `038E3031996E4C73FC9B0F97C9C90EC22697D41EE6CA033AD2746E77219D37F7` | same panel, single eye |

Both show the same content: the camera/headset adjustment warning, a green rectangle, and a
Continue button. This is the PSVR system health screen, which Déraciné presents before the title.

### It is not the classroom

Compared against the reviewed classroom reference with the repository's own
`tools/benchmark/scene_signature.py`:

| Image | Similarity | Threshold | Pass |
| --- | --- | --- | --- |
| `008_...` | -0.089301 | 0.85 | false |
| `009_...` | -0.089307 | 0.85 | false |
| `010_...` | -0.089309 | 0.85 | false |

A negative correlation is not a near miss. The classroom reference is the opening classroom with
the floating New game / Load game / Continue / System overlay; this is a different screen
entirely.

## Why the earlier runs could not have been the classroom

The classroom gate requires 400 draws and 500 Mpx. The SC-02 and SC-03 runs peaked at 39 draws
and 248 Mpx across all 147 frame-stat samples. They never approached the gate, which is why the
runner reported `INVALID: the classroom scene was not reached before the timeout` in every cell.

That message was accurate. The captures were valid, but they were of the wrong scene.

## What this means for SC-02 and SC-03

Their technical findings stand, because they are about the capture mechanism and the shader-state
chain, not about the scene:

- The two-slot shader-state layout and the alternate-slot resolution are real.
- The 156 draws with valid shader associations are real.
- The colour-writing classification is real.

What changes is the **scope**: those 156 draws are the health-warning screen's draws, not
classroom draws. Any statement about "the game's rendering" based on them was overreaching.

## Scene identity, recorded

| Field | Value |
| --- | --- |
| Scene | PSVR health-and-safety warning ("adjust camera and headset") |
| Evidence | render-target dump at 50 s, inside the capture interval |
| Screenshot SHA-256 | `05EB10596A0BB514EE7FF5063A1820DBB01F1DECC51A7ABF2C2C35164437BFD4` |
| Classroom similarity | -0.089 (threshold 0.85) |
| Input | none (`-Autopress 0`) |
| Save | none |
| Render settings | `SHADPS4_VR=1`, `SHADPS4_OPENXR=0`, `SHADPS4_VR_DEMO=0` |
| Executable | `wt-sc04-scene/build/shadPS4.exe` |
| Capture interval | 0-70 s, dump at 50 s |
| Draws at dump | 39, 247.3 Mpx, 24.9 passes |

## Consequence for the remaining phases

Phase 2 must reach the actual classroom, which needs the classroom gate to pass. That requires
automated input, which the existing `runner.ps1 -Capture` path provides through its autopress
schedule.

The health-warning screen is a legitimate baseline, and it is now identified. It is not a
substitute for the classroom.

## Reproducible checks

```
# trigger a dump inside the capture interval
[IO.File]::WriteAllText("<run>/lab/user/dump_render_targets", "1`r`n")

# compare against the reviewed classroom reference
python tools/benchmark/scene_signature.py compare <dump>/008_*.png `
  --reference tools/benchmark/reference/scene-classroom-menu.json

# preview for visual identification
python tools/benchmark/scene_signature.py preview <dump>/020_*.png --out preview.png
```

`scene_signature.py` needs Pillow. Not every interpreter on this machine has it; check with
`python -c "import PIL"` before relying on the comparison.
