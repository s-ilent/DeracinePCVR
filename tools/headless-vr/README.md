# Headless VR tooling (no physical headset)

Tools for running Déraciné through the emulated-PSVR path with **no OpenXR host and no physical
headset**, inside the machine-wide GPU lock.

Status (2026-11-18): **M1** (headless launch/front end) and **M2** (headless gameplay entry) are
verified; **M3** (gameplay benchmark + validation) is in progress. The M2 emulator support lives
on `feature/headless-vr-scene` (see the handoff document for the commit list); the harness here is
the M1/M2/M3 entry point.

## Files

| file | what |
|---|---|
| `headless-vr-launch.ps1` | M1 entry point: isolated lab, headless env, launch, assert eye size + front-end phase, optional eye dump, stop, `result.json` |
| `headless-vr-gameplay.ps1` | M2/M3 cell runner: menu→Continue→loading→gameplay gate, measurement window, captures, A/B/A, RenderDoc and 100 %/120 Hz modes, startup-crash retries, clean-exit reporting |
| `inputs/` | scripted input examples (`menu-continue-move.txt` is the verified M2 recipe) |
| `validation/run-validation.ps1` | A–E matrix orchestrator + no-GPU dry-run/self-tests |
| `gpu-lock.sh` | machine-wide GPU lock (same slot as the old HMD slot; headless-only enforcement) |
| `HEADLESS-ONLY.md` | the user directive and rules all agents follow |

## M1 verified result

Run on 2026-11-18 on the renderer candidate `7B658961…` (source `1a53ceb`):

```
valid: True  eye: 2880x3240  hmd frames: 12  worn lines: 0
front end: draws 472.667  mpx 485.3  passes 62.967  fps 57.733
```

## M2 verified result (gameplay)

Headless menu → Continue → loading → classroom gameplay, two independently constructed labs on
`feature/headless-vr-scene` @ `a21109c`, emulator `wt-headless/build/shadPS4.exe`
(sha256 `6B260523…`):

| run | gate | draws | Mpx | passes | fps p50 | frames |
|---|---|---|---|---|---|---|
| `runs/m2-off-fixed-1` | gameplay @64 s | 1866.3 | 606.2 | 41.4 | 33.5 | 77 |
| `runs/m3-portable-2` | gameplay @63 s | 1865.8 | 550.2 | 38.4 | 31.8 | 76 |

Both report 2880x3240 per eye, RGBA8Unorm, 1 sample at hand-off. The gameplay image is the
classroom with the spectral hands and the girl at the desk (the menu UI is gone). The menu is
~465–500 draws / ~480 Mpx / ~65 passes; the loading dip is 42–55 draws. **The actual gameplay
workload is ~1866 draws, not the brief's 480–494.**

## M2 recipe (required for gameplay)

```
SHADPS4_VR=1  SHADPS4_OPENXR=0  SHADPS4_VR_DEMO=0
SHADPS4_VR_HANDS=synthetic  SHADPS4_VR_MOVE_FROM_PAD=1  SHADPS4_MOVE_PITCH=0
SHADPS4_DBG_AUTOPRESS=1  SHADPS4_DBG_AUTOPRESS_TO=45000
```

Input (`inputs/menu-continue-move.txt`): fixed head, arm's reach until 45 s, pointer hand at
`pad=0.30,-0.12,-0.5` from 46 s, then `move1=6,255` (pointer hand's trigger + Move button, 1 s)
at 60, 63 and 66 s. The menu auto-selects Continue once the star is on it; the presses are the
belt-and-braces confirm.

**Save data is part of the fixture:** the lab must contain save slot 1 (`MGS_GameData_1`, stage
`m00_00_10_10`), `MGS_SystemData` and `sce_sys/param.sfo` from a gameplay save. The harness
refreshes the lab from `_run/pc-vr-recycler-try/user/home/1000/savedata/CUSA11919` and refuses a
slot-0-only lab. Do not publish save files or game content.

## Run a gameplay cell

```bash
bash _build/team/gpu-lock.sh run --owner <you> --kind headless \
  --reason "headless M2 gameplay" --ttl 1800 --require-quiet --quiet-wait 900 -- \
  pwsh.exe -NoProfile -File M:/Development/Deracine-work/_build/headless/headless-vr-gameplay.ps1 \
  -Exe M:/Development/Deracine-work/wt-headless/build/shadPS4.exe \
  -Label m2-cell -Arm off -Gate gameplay -Seconds 40 -Warmup 10 -MaxSeconds 420 -Attempts 2 \
  -GameplayMinDraws 800 -Capture
```

The cell writes `runs/<label>/{result.json,meta.json,session.json,shad_log.txt,timings.csv,frames/*.png}`
and exits 0 only when a valid gameplay scene was measured. `-Mode aba` recycles the arms; `-Mode
rdoc` takes a scene-gated RenderDoc capture; `-Mode lowres` is the 100 %/120 Hz cell.

## Gates and signatures

| scene | draws | Mpx | passes |
|---|---|---|---|
| front end / menu | 465–500 | ~480 | ~65 |
| loading | 42–55 | — | — |
| gameplay | 1500–2300 | 550–750 | 35–55 |

`-Gate gameplay` requires the menu phase, a loading dip and a settled heavyweight phase
(`-GameplayMinDraws 800`). FPS is never used to identify the scene. The image gate compares the
gameplay capture against `references/menu-eye.png` (must differ) and `references/gameplay-eye.png`
(once installed; must match). Reference PNGs are generated evidence and are not tracked.

## Known startup crash and retry

The known `address_space.cpp Protect: Unreachable code!` crash kills a run at random before the
front end (both builds; documented earlier as pre-existing). The cell runner retries a fresh
session up to `-Attempts` (default 3). A run that merely stalls is not retried; it is reported
`valid=false` with its reason.

## What is not validated here

No physical-headset latency, comfort, optical distortion, or true compositor behaviour. The
desktop presentation path is the test-only destination; the guest still executes its normal GNM
stream, Vulkan commands, caches, depth/stencil work and allocations. Headless-only mode does not
grant authority to start, stop or reconfigure SteamVR/OpenXR services.
