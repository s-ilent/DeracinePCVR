# SC-05 Investigation A: title-screen progression

## Result

The classroom/title screen was **not** reached, and the PSVR health warning was **not** dismissed
by any input tried. The warning persists for the entire run regardless of what is pressed.

The evidence points to a specific blocker: the warning is a **camera/headset positioning screen**
that waits for tracking data, not for a button.

## The input schedule, reconstructed

`SHADPS4_DBG_AUTOPRESS` is implemented in `src/src/core/devtools/autopress.h`:

```cpp
struct AutoPressSchedule {
    u64 begin_ms = 8000;
    u64 end_ms   = 45000;
    u64 cycle_ms = 3000;
    u64 hold_ms  = 600;
};
```

And `src/src/core/libraries/pad/pad.cpp` injects exactly one button:

```cpp
if (auto_press.Tick(now_ms, &began)) {
    pData[i].buttons |= OrbisPadButtonDataOffset::Cross;
}
```

So autopress is **Cross only**, held 600 ms every 3 s from 8 s to 45 s. Twelve presses.

### The actual timeline from `classroom-run-3`

| Press | Draws at that second | Mpx |
| --- | --- | --- |
| 9013 ms | 25 | 194.8 |
| 12000 ms | 26 | 191.7 |
| 15013 ms | 26 | 194.1 |
| 18001 ms | 26 | 189.2 |
| 21004 ms | 39 | 247.6 |
| 24003 ms | 28 | 184.1 |
| 27016 ms | 32 | 208.3 |
| 30017 ms | 32 | 208.3 |
| 33014 ms | 31 | 201.6 |
| 36015 ms | **445** | 437.1 |
| 39015 ms | **449** | 441.4 |
| 42014 ms | 28 | 189.8 |

The 445-draw plateau begins at the 36 s press and ends at the 42 s press. That is a real
transition, and it is the one SC-04 mistook for the classroom.

## The 446-draw plateau is a black screen

A render-target dump taken at 38 s, inside the plateau, was analysed image by image.

| Image | Grayscale std | Content |
| --- | --- | --- |
| `008_..._5760x3240_R8G8B8A8Unorm_s1` | 0.267 | black |
| `009_..._5760x3240_R8G8B8A8Unorm_s1` | 0.317 | black |
| `010_..._5760x3240_R8G8B8A8Unorm_s1` | 0.251 | black |
| `011_..._1280x720_R8G8B8A8Srgb_s1` | 13.85 | font atlas only |
| `016_..._1024x1024_R8G8B8A8Unorm_s1` | 57.91 | noise, not a scene |
| `020_..._1920x1200_R8G8B8A8Srgb_s1` | 0.000 | uniform black |

The brightest sRGB display target in the whole plateau dump has mean **9.11**. The classroom
reference has mean **40.46**. The health warning's panel has mean **178.85**.

So the plateau is a black transition screen with a classroom-like draw count. It is not the
classroom.

## The 624-draw / 502.7-Mpx cell is also black

`classroom-off-run-1` reached 624 draws and 502.7 Mpx, passing the workload gate. Its dumps:

| Image | Mean |
| --- | --- |
| `022_..._1280x720_R8G8B8A8Srgb_s1_alpha` | 9.11 |
| `023_..._1920x1080_R8G8B8A8Srgb_s1` | 0.35 |

Also black. The screenshot similarity was 0.4283, below the 0.85 threshold, so the scene gate
correctly failed.

## The controlled experiment

The task asked for individual, explicitly timed presses rather than repeated autopress. The
repository has a scripted-input system for exactly this (`SHADPS4_INPUT_SCRIPT`), with per-line
timing.

### Cell 1: one Cross press at 25 s

```
25 0.3 cross
```

Result: the warning persisted through 60 s. Draws stayed at 37-39, Mpx at 231-249. No transition.

### Cell 2: each plausible dismiss button once

```
22 0.3 cross
28 0.3 options
34 0.3 circle
40 0.3 start
46 0.3 cross
```

Result: the warning persisted through 68 s. Draws stayed at 37-39. Dumps at 32, 44, 56 and 68 s
all showed the same bright panel (mean 178.85).

### Cell 3: long Cross holds

```
20 2.0 cross
40 2.0 cross
```

Result: the warning persisted through 85 s. Draws stayed at 37-39. All four dumps identical.

## Why the warning does not clear

The warning text is:

> Please adjust the PlayStation Camera and VR Headset so the headset remains in range.
> Assuming proper posture before making adjustments optimizes the interactive experience.

That is a **positioning** instruction, not a confirmation prompt. It is asking the player to move
the headset into the camera's view. A button press is not the expected input.

Supporting evidence:

- The screen renders at **24 passes**, against the classroom's ~80. It is a lightweight overlay,
  not a scene.
- The Move controllers are registered (`sceMoveOpen` for both hands, `sceVrTrackerRegisterDevice`
  for both), so the game has the devices.
- The headset is reported worn (`SHADPS4_XR_PAUSE=0` forces it), so the worn gate is not the
  blocker.
- No button changes the draw count at all. A dismissible prompt would show some response.

So the blocker is most likely the **tracking data**: the game wants the headset and controllers
positioned in view of the camera before it proceeds, and a headless run has no real tracking.

## What this means

The classroom is not reachable by pressing buttons. It needs either:

1. Synthetic tracking data that satisfies the positioning check, or
2. A save file that skips the front end, which is the approach the memory notes describe for
   headless gameplay.

Option 2 is the established path: the headless gameplay harness stages a save slot and reaches
the menu. That is a different harness from `runner.ps1`, and it is outside this branch's scope.

## Scene identification, recorded

| Field | Value |
| --- | --- |
| Scene | PSVR health-and-safety positioning warning |
| Evidence | render-target dump at 50 s, inside the capture interval |
| Screenshot SHA-256 | `05EB10596A0BB514EE7FF5063A1820DBB01F1DECC51A7ABF2C2C35164437BFD4` |
| Classroom similarity | -0.089 (threshold 0.85) |
| Passes | 24 (classroom ~80) |
| Input tried | Cross, Options, Circle, Start, 2 s holds |
| Result | No dismissal, no draw-count change |

## Reproducible checks

```
# one press, then nothing
echo "25 0.3 cross" > input.txt
$env:SHADPS4_INPUT_SCRIPT=<abs path>\input.txt

# dump inside the interval
[IO.File]::WriteAllText("<run>/lab/user/dump_render_targets", "1`r`n")

# identify
python tools/benchmark/scene_signature.py compare <dump>/010_*5760x3240*.png `
  --reference tools/benchmark/reference/scene-classroom-menu.json
```

The scripted-input format is documented in `src/src/input/scripted_input.h`.
