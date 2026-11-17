# Headless VR tooling (no physical headset)

Tools for running Déraciné through the emulated-PSVR path with **no OpenXR host and no physical
headset**, inside the machine-wide GPU lock.

Milestones (user decision, 2026-11-18):

1. **M1 — headless launch / front end (available now).** Boot the game to the classroom front
   end with the three switches, both eyes at 2880x3240, no headset. This is what the other
   agents use for routine work.
2. **M2 — scripted Move movement and inputs (second).** Aim/click the Move pointer so the front
   end can be walked into gameplay (`Continue`, saved stage `m00_00_10_10`). Test-only support
   lives on `feature/headless-vr-scene` (`wt-headless`).
3. **M3 — renderer validation on gameplay** (recycler A/B/A, R3 pixel compare, R6 clean
   shutdown, scene-gated RenderDoc, 100 %/120 Hz).

## Files

| file | what |
|---|---|
| `headless-vr-launch.ps1` | M1 entry point: isolated lab, headless env, launch, watch the log, assert the eye size and the front-end heavy phase, capture one eye PNG, stop, write `result.json` |
| `gpu-lock.sh` | machine-wide GPU lock (same lock directory as the old HMD slot; headless-friendly; `--require-quiet` process audit; release on failure) |
| `HEADLESS-ONLY.md` | the user directive and the rules all agents follow |
| `HEADLESS-M2.md` | M2 gameplay: integration notes, opt-in table, benchmark procedure, single-lab status |
| `input/gameplay-entry.txt` | the committed M2 scripted Move sequence that reaches gameplay |
| `save-slot/` | `MANIFEST.json` + `provision.ps1` for the deterministic slot-1 save (hashes pinned) |

## M1 verified result

Run on 2026-11-18, renderer candidate `integration/cross-fork-renderer` (binary sha256
`7B658961…`, source `1a53ceb`) in a private lab with game shadow
`D:\Temp\Deracine\CUSA11919-qual`:

```
valid: True  eye: 2880x3240  hmd frames: 12  worn lines: 0
front end: draws 472.667  mpx 485.3  passes 62.967  fps 57.733
```

A repeat with the same build after the known startup crash (the launcher retried automatically):
`valid: True`, 471.333 draws / 491.667 Mpx / 63.6 passes / 56.067 fps. Both are the classroom
menu scene, **not gameplay**; the result says `gameplay=false` and must not be quoted as a
gameplay number.

Evidence: `_build/headless/runs/m1-candidate-r3/` and `.../m1-candidate-r5-r3/`
(`result.json`, `shad_log.txt`, `env.txt`). Full left/right eye captures are taken scene-gated on
the M2/M3 build; M1's proof is the log + result.json.

## Run it

```bash
bash _build/team/gpu-lock.sh run --owner <you> --kind headless \
  --reason "headless M1 launch" --ttl 900 --require-quiet -- \
  pwsh.exe -NoProfile -File M:/Development/Deracine-work/_build/headless/headless-vr-launch.ps1 \
  -Exe M:/Development/Deracine-work/wt-crossfork-int/build/shadPS4.exe
```

`headless-vr-launch.ps1` takes `-Exe` (mandatory), `-Label`, `-Lab`, `-Eboot`, `-Seconds`,
`-DumpFrame`, `-DumpWidth`, `-Attempts`. It creates/reuses a private lab under
`_build/headless/labs/`, copies the save with progress if present, sets `Log.sync=true`, launches
the emulator directly (never `pc-vr/launch.ps1`), and stops it after the front end is reached.

## Headless switches

```
SHADPS4_VR=1  SHADPS4_OPENXR=0  SHADPS4_VR_DEMO=0
```

The title's front end is Move-driven: the **existing** dev autopress
(`SHADPS4_DBG_AUTOPRESS=1`, `SHADPS4_DBG_AUTOPRESS_TO`, `SHADPS4_DBG_AUTOPRESS_MENU=1`) advances
the controller-check screen. Without it the game waits at ~38 draws / ~236 Mpx and never reaches
the menu. The launcher sets it.

## Known startup crash and retry

The known `address_space.cpp Protect: Unreachable code!` crash kills a run at random before the
front end (both builds; documented earlier as pre-existing). The launcher retries a fresh session
up to `-Attempts` (default 3) when the emulator dies before the front end. A run that merely
stalls is not retried; it is reported `valid=false`.

## What is not validated here

No physical-headset latency, comfort, optical distortion, or true compositor behaviour. The
desktop presentation path is the test-only destination; the guest still executes its normal GNM
stream, Vulkan commands, caches, depth/stencil work and allocations.
