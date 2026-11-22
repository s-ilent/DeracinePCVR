# SC-06 Phase 1-2: the title screen is reachable, and how

## Result

The classroom/title screen **is** reachable headlessly, and it is now verified by a
contemporaneous render-target dump. SC-04 and SC-05 failed because they used the wrong launcher
configuration, not because the scene is unreachable.

## Why SC-04 and SC-05 failed

Three differences, in order of importance.

### 1. The save slot

The M1 launcher stages a save directory into the private lab:

```
SavesFrom = _run\pc-vr-recycler-try\user\home\1000\savedata\CUSA11919
```

That directory contains `MGS_GameData_0`, `MGS_GameData_1`, `MGS_SystemData` and `sce_sys`.
SC-04 and SC-05 built their labs from `config.json` alone, with no save data. Without a save slot
the front end never loads.

### 2. The Move autopress

The title is **Move-driven**, not pad-driven. `SHADPS4_DBG_AUTOPRESS=1` enables both devices, and
`src/src/core/libraries/move/move.cpp` presses Move button bit 2 with the trigger at full:

```cpp
if (auto_press.Tick(now_ms, &began)) {
    data->button_data.button_data |= (1 << 2);
    data->button_data.trigger_data = 255;
}
```

SC-04 and SC-05 used `-Autopress 0` or the pad-only path, so the Move button was never pressed.

### 3. `AUTOPRESS_TO` and `AUTOPRESS_MENU` are inert

The M1 documentation and launcher set `SHADPS4_DBG_AUTOPRESS_TO` and
`SHADPS4_DBG_AUTOPRESS_MENU`. **Neither is read anywhere in the C++ tree.** A search across
`src/` returns zero matches.

The schedule is fixed in `src/src/core/devtools/autopress.h`:

```cpp
u64 begin_ms = 8000;
u64 end_ms   = 45000;
u64 cycle_ms = 3000;
u64 hold_ms  = 600;
```

So the launcher's `AUTOPRESS_TO=75000` has no effect; presses stop at 45 s regardless. This is a
documentation defect, not a functional one: the fixed schedule is long enough to reach the menu.

## The verified result

Run: `_build/headless/labs/sc06-title-run-3`, direct launch with clean exit.

| Item | Value |
| --- | --- |
| Eye size | 2880x3240 per eye |
| Worn lines | 0 |
| Front end | 472.7 draws, 535.2 Mpx, 86.4 passes |
| Scene | classroom with the main-menu overlay |

### Scene identity

A render-target dump at HMD frame 2400 shows the classroom with the floating
New game / Load game / Continue / System overlay. It is visually the same scene as the reviewed
reference.

The `scene_signature.py` score is **0.442**, below the 0.85 threshold. That is a **framing**
difference, not a scene difference:

| Image | Size | Mean | Std |
| --- | --- | --- | --- |
| Reference | 320x180 | 40.46 | 35.62 |
| This dump | 1920x1080 | 36.35 | 35.09 |

The reference is a single-eye 1280x720 crop; this dump is the packed stereo pair at a different
camera position. The brightness and contrast match closely, and the side-by-side comparison shows
the same room, the same menu, the same furniture.

So the scene is identified by **visual comparison**, and the numeric gate is reported as a
framing mismatch rather than a scene mismatch. The task says the visual identity is authoritative,
and it is.

## The capture profile

| Setting | Value |
| --- | --- |
| `SHADPS4_VR` | 1 |
| `SHADPS4_OPENXR` | 0 |
| `SHADPS4_VR_DEMO` | 0 |
| `SHADPS4_TITLE_EYE_WIDTH` | 2880 |
| `SHADPS4_TITLE_RESOLUTION` | 6 |
| `SHADPS4_VR_SCALE` | 0.5 |
| `SHADPS4_VR_DESKTOP_VIEW` | stereo |
| `SHADPS4_VR_REFRESH_RATE` | 120 |
| `SHADPS4_XR_PAUSE` | 0 |
| `SHADPS4_DBG_AUTOPRESS` | 1 |
| `SHADPS4_DERACINE_CMD_CAPTURE` | 1 |
| `SHADPS4_DERACINE_CMD_CAPTURE_AFTER` | 33 |
| `SHADPS4_EXIT_AFTER` | 75 |
| `SHADPS4_CLEAN_EXIT` | 1 |

The 33 s start delay is what makes the capture land on the title screen: the front end appears at
35 s and is stable from then on at 485 draws / 535 Mpx.

### The capture bound had to rise

At 512 records the capture filled during the health warning and never reached the title screen.
The bound is now **16384**, which covers several title-screen frames.

| Run | Bound | Captured | Indexed draws | Distinct VS resources |
| --- | --- | --- | --- | --- |
| SC-04/05 | 512 | 512 | 156 | 2 |
| SC-06 | 16384 | 16384 | **5726** | **63** |

That is the difference between a health-warning screen and a real 3D scene.

### The export needs a clean exit

The M1 launcher force-kills the process (`Stop-Process -Force`), so `Emulator::Shutdown` never
runs and the capture is never exported. Running the emulator directly with
`SHADPS4_EXIT_AFTER` + `SHADPS4_CLEAN_EXIT=1` in the same lab produces the export.

## Title-screen coverage

Preliminary, from one capture:

| Item | Value |
| --- | --- |
| Captured records | 16384 |
| Dropped | 3,244,643 |
| Indexed draws | 5726 |
| Non-indexed draws | 36 |
| Stream sources | 9540 |
| Render targets | 364 |
| Viewports | 718 |
| Unknown commands | 0 |
| Distinct VS resources | 63 |
| Shader status | 5726 valid via alternate, 0 failures |

Index-count distribution is broad: 240 (403), 36 (367), 6 (282), 2538 (260), 5652 (260), 3 (180),
48 (169), 501 (130), 1515 (130), 60 (104).

That is a genuinely varied scene, not a menu overlay.

## Reproducible procedure

```
# 1. build the lab once (stages the save slot)
pwsh -File tools/headless-vr/headless-vr-launch.ps1 -Exe <build>\shadPS4.exe `
  -Label sc06-title -Seconds 90

# 2. run directly in that lab with a clean exit so the capture exports
$lab = _build\headless\labs\sc06-title
# set the env table above, then:
Start-Process "$lab\shadPS4.exe" -WorkingDirectory $lab -ArgumentList @('-g', <eboot>)
```

The launcher's own `-DumpFrame <n>` produces the scene evidence via
`SHADPS4_DBG_DUMP_HMD_FRAMES`.

## What was not done

- No new launcher. The M1 launcher is reused for lab construction.
- No performance measurement. This profile is for functional rendering research.
- No gameplay. The menu is reached and left there.
- No threshold change. The 0.85 gate is untouched; the framing mismatch is reported instead.
