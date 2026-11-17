# Gameplay benchmark procedure (headset session)

Gameplay is the only place this title renders above 60 fps, and it cannot be reached headless: the
menu is driven by a located controller of a worn headset (`_build/PASSES-AND-PACING.md` §20). The
33–36 fps "stationary classroom" figure that motivated the recycler has therefore **never been
measured with it**, and nothing measured on the 60 Hz-capped front end says anything about gameplay.
This is the procedure for the next session with the headset on.

## What is being compared

One binary, two arms, differing only in the recycler:

| arm | variable |
|---|---|
| on | `SHADPS4_DBG_IMAGE_RECYCLER=1` |
| off | `SHADPS4_DBG_IMAGE_RECYCLER=0` |

The build is the integration candidate (`integration/cross-fork-renderer`, `wt-crossfork-int`). Its
recycler is off by default; the variable in the table is the only difference between the arms.
Run them **interleaved**: on, off, on — the second "on" cell is what tells a real difference from
run-to-run drift.

## The user's part (the whole of it)

1. Hold the HMD slot, then start a cell (command below). The script launches the emulator itself.
2. Put the headset on and play to the stationary classroom (the room with the floating menu).
3. Stand still, looking at the classroom, for the measurement window (90 s is plenty; the script
   keeps 40 s of it after discarding the start).
4. Quit the game. The script notices, analyses the log, and writes `result.json`.
5. Repeat for the other arm, then for the second "on" cell.

No input is simulated, no window is automated, and the script never starts SteamVR itself.

## Commands

```powershell
# one cell per command; the label names the arm and the repeat
pwsh -File M:\Development\Deracine-work\_build\crossfork\gameplay-cell.ps1 `
     -Exe M:\Development\Deracine-work\wt-crossfork-int\build\shadPS4.exe `
     -Arm on  -Label gp-on-1  -Minutes 8

pwsh -File M:\Development\Deracine-work\_build\crossfork\gameplay-cell.ps1 `
     -Exe M:\Development\Deracine-work\wt-crossfork-int\build\shadPS4.exe `
     -Arm off -Label gp-off-1 -Minutes 8

pwsh -File M:\Development\Deracine-work\_build\crossfork\gameplay-cell.ps1 `
     -Exe M:\Development\Deracine-work\wt-crossfork-int\build\shadPS4.exe `
     -Arm on  -Label gp-on-2  -Minutes 8
```

The script runs the emulator from a private portable directory
(`_build\crossfork\gameplay-lab`), with the benchmark's render configuration
(300 %, 2× MSAA, `SHADPS4_VR_SCALE=0.5`, vblank 60), `SHADPS4_FRAME_STATS_EVERY=1` and
`SHADPS4_DBG_OVERLAP=1`. It writes `_build\crossfork\gameplay\<label>\{session.json,shad_log.txt,result.json}`.
The live `pc-vr/` runtime, the live executable and the shared lab are not touched.

## What is recorded and how it is read

Per cell, from the emulator's own log (never from XR-present cadence):

| field | where it comes from | why |
|---|---|---|
| `fps_p50/p90/p99`, `fps_min/max` | `frame stats at …` lines, one a second | unique rendered frames; the only timing source that means what it says |
| `draws_mean`, `mpx_mean`, `passes_mean` | same lines | scene identity: the arms must agree on these before their fps may be compared |
| `gpu_busy_mean` | same lines | a CPU-side win can only show if the scene is CPU-bound; a GPU-bound scene cannot |
| `images_created_mean`, `create_ms_mean`, `destroy_ms_mean` (+ per-frame) | `RENDERSTAT` lines | the recycler's own effect: driver image creations and the wall time of the calls |
| `recycler_hits/misses_mean`, `pool_mb_mean` | same | whether the pool actually served the shapes this scene uses |
| `vram_mb_mean` | same | the emulator's own VMA total, so the pool's cost is visible |
| `reward window` | last 40 s of the longest run with `draws ≥ 400` and `Mpx ≥ 450` | the stationary classroom, not a loading screen |

**Acceptance rules for a result** (write them into the comparison, do not bend them afterwards):

1. Both arms' windows must have `draws_mean` within 5 % and `mpx_mean` within 5 % of each other.
   Otherwise the arms saw different content and no fps comparison is made.
2. A frame-rate difference counts only if it is larger than the spread between the two "on" cells.
3. `create_ms` / `destroy_ms` / `images_created` are reported **independently of fps**. They can
   show the recycler working in a cell where fps cannot move.
4. If `gpu_busy_mean` is ≥ 95 %, the scene is GPU-bound and a CPU-side saving is not expected to
   show as fps — say so rather than reporting a null.
5. A cell that never matched the classroom signature is discarded, not reported.

## What this would settle, and what it would not

- Settles: whether the recycler's 3.9 ms/frame of GPU-thread work (front-end measurement) also
  appears in gameplay, whether it changes fps or long frames there, and what it costs in device
  memory on that scene.
- Would still not settle: the pixel-contents question from the review (R3). That needs a
  render-target dump A/B in the same scene — `SHADPS4_DUMP_HMD_FRAMES`-style capture, both arms at
  the same point of the same scene, compared with `_build\bench\capture-compare.py`. Do this in the
  same session if a capture is convenient; the arms must be the same binary and the same scene.
- Does not settle the 120 fps objective: 300 % at 2880×3240 per eye is the resolution being
  measured, not the goal configuration.

## The matched front-end comparison (still pending, no headset needed)

The front-end cells that were reported earlier compared a classroom arm with a calibration arm; the
assignment calls that out and it is not yet fixed. The runner can now refuse a cell that never
reached the scene and retry the arm:

```powershell
bash _build/team/testslot.sh run --owner <you> --reason "matched cells" --ttl 3600 -- `
  pwsh.exe -NoProfile -File 'M:\Development\Deracine-work\_build\crossfork\ab.ps1' `
  -Exe 'M:\Development\Deracine-work\wt-crossfork-int\build\shadPS4.exe' `
  -Tag xf-match -Variable SHADPS4_DBG_IMAGE_RECYCLER -Arms 1,0,1 `
  -RequireQuiet -RequireScene -MaxAttempts 3 -Warmup 10 -Seconds 30 -TimeoutSeconds 220
```

That is a front-end measurement: it can show the allocation and CPU-thread savings, and it cannot
show an fps improvement, because the scene is capped at 60 Hz by the title. Add a baseline arm by
running the same command with `-Exe _build\crossfork\bin\baseline-de9c78c.exe` and `-Arms baseline`.
