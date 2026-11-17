# HEADLESS ONLY — user directive, 2026-11-18 01:07

The user has ended headset use for routine work:

> "Sounds reasonable, although I think scripted Move movement and inputs should be a second
> milestone - being able to launch without engaging the headset already opens up a lot of
> possibilities, and I want the other running agents to stop using it."

From now on:

1. **No agent starts SteamVR, OpenXR, or a headset session for ordinary emulator work.**
   No HMD slot reservation, no `SHADPS4_OPENXR=1`, no physical-headset cell.
2. **Every emulator run is headless:**
   `SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0`.
   These switches were verified on 2026-11-18: no OpenXR host is looked for, the headset is
   never reported off, both eyes render at 2880x3240, and the title advances to the classroom
   front end by itself.
3. **The GPU lock is the only serialization.** Take `_build/team/gpu-lock.sh` (or
   `testslot.sh`) with `headless` in the reason; `testslot.sh` refuses a non-headless reason
   while this marker exists (exit 8).
4. **A physical headset is an occasional final integration check only**, and only when the user
   explicitly asks for it. It is not a dependency for renderer work, scene entry, captures,
   RenderDoc, or performance cells.
5. Anyone with an OpenXR/SteamVR cell already running: finish that bounded cell, then stop;
   do not start another. The headset was not being worn anyway, so nothing was being validated
   by involving it.

## Verified headless recipe (milestone 1)

Private portable lab, game shadow `D:\Temp\Deracine\CUSA11919-qual`, current candidate
`wt-crossfork-int/build/shadPS4.exe` (sha256 `7B658961…`) or any later build:

```
SHADPS4_VR=1
SHADPS4_OPENXR=0
SHADPS4_VR_DEMO=0
SHADPS4_TITLE_EYE_WIDTH=2880
SHADPS4_TITLE_RESOLUTION=6
SHADPS4_VR_SCALE=0.5
SHADPS4_VR_DESKTOP_VIEW=stereo
SHADPS4_VR_REFRESH_RATE=120
SHADPS4_VR_FPS_CAP=120
SHADPS4_VR_FASTEST_PACE=1
SHADPS4_FRAME_STATS=1
SHADPS4_FRAME_STATS_EVERY=1
```

Launch directly (`shadps4.exe -g <eboot>`, working directory = the private lab) inside the GPU
lock. One command:

```bash
bash /mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh run \
  --owner <you> --kind headless --reason "headless M1 launch" --ttl 900 --require-quiet -- \
  pwsh.exe -NoProfile -File M:/Development/Deracine-work/_build/headless/headless-vr-launch.ps1 \
  -Exe M:/Development/Deracine-work/wt-crossfork-int/build/shadPS4.exe
```

The title's front end is Move-driven: the **existing** Move autopress
(`SHADPS4_DBG_AUTOPRESS=1` + `SHADPS4_DBG_AUTOPRESS_TO` + `SHADPS4_DBG_AUTOPRESS_MENU=1`) is
part of the recipe. Without it the game waits on the controller-check screen (~38 draws,
~236 Mpx) and never reaches the classroom menu. `headless-vr-launch.ps1` sets it for you. This
is not new support; it is the dev autopress that already exists in the tree.

Evidence of the verification: `_run/lead-headless/PROBE.md`,
`_run/lead-headless/probe1/user/log/shad_log.txt`,
`_run/lead-headless/probe1/user/rt_dump-3600/capture_01/009_2020000000_5760x3240_R8G8B8A8Unorm_s1.png`.

## Milestones

- **M1 (now):** headless launch and front-end. Any agent can boot the game to the classroom
  front end with the GPU lock and no headset. This is usable today.
- **M2 (second):** scripted Move movement and inputs, so the front end can be walked into
  gameplay (`Continue` on the saved stage `m00_00_10_10`). Test-only, env-gated support is in
  `wt-headless` (`feature/headless-vr-scene`): synthetic tracked Move hands + Move
  buttons/trigger from the emulated pad/script.
- **M3 (after M2):** A–E renderer validation on the gameplay scene (recycler A/B/A, R3 pixel
  compare, R6 clean shutdown, scene-gated RenderDoc, 100 %/120 Hz).

Do not substitute the front-end menu scene for gameplay in any renderer result.
