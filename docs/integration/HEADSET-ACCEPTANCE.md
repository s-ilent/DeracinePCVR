# Headset acceptance checklist — Déraciné PCVR

Status: **not executed.** No OpenXR/SteamVR session, no physical headset and no in-game audio test
has been performed on the unified checkpoint. Everything below is the acceptance gate that needs a
worn headset and explicit permission; the headless cells validate the renderer and the benchmark,
not comfort, optics, tracking feel, audio, or compositor behaviour.

The acceptance build is source `9b918eb` (R6 included); record the exact executable SHA-256 and
`git rev-parse HEAD` before starting. **Do not deploy to the active runtime** and do not install the
build into `pc-vr/` without explicit authorization.

## Preconditions

- [ ] User explicitly approves a headset session and names the HMD/OpenXR runtime (SteamVR).
- [ ] No other agent holds the GPU/headset slot; take the machine-wide lock for the whole session.
- [ ] All headless switches are **unset** (`SHADPS4_VR_HANDS`, `SHADPS4_VR_MOVE_FROM_PAD`,
      `SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT`, `SHADPS4_DBG_DUMP_HMD_AT_SCENE`,
      `SHADPS4_DBG_MOVE_TRACE`, `SHADPS4_DBG_IMAGE_RECYCLER`).
- [ ] The executable under test is launched from a private lab; `pc-vr/` and saves are untouched.
- [ ] Record: executable SHA-256, source commit, GPU/driver, runtime version, headset model,
      game build (1.00 or 1.01), and the full environment.

## 1. OpenXR initialization

- [ ] SteamVR starts normally; the emulator creates an OpenXR session and reaches the front end.
- [ ] No `xrCreate*` failure, no fallback to the desktop path, no `SetHeadsetWorn` mismatch.
- [ ] Both eyes are presented; no black eye, no duplicated eye, no obvious compositor error.
- [ ] `PrepareHmdFrame` reports the expected eye picture size for the chosen resolution.
- Evidence: emulator log (session/presentation lines), OpenXR runtime log if available.

## 2. Stereo and frustum correctness

- [ ] Comfortable stereo depth; no eye swap, no inverted parallax, no vertical misalignment.
- [ ] The projection matches what the title drew: no oversized/undersized FOV, no barrel or
      pincushion mismatch at the edges.
- [ ] `SHADPS4_VR_RESOLUTION` unset uses the runtime recommendation; test `guest` as the fallback.
- [ ] A render-target dump of both eyes shows the expected layout (packed or separate, per the
      session) and the projection-math suite still passes (`shadps4_vr_projection_math_test`).
- Evidence: eye captures (left/right), `image_validation.py compare` against reviewed references
  when available; note that scene similarity alone is not colour evidence.

## 3. Head tracking

- [ ] Rotation is 1:1 with no drift over a 60 s look-around; position tracks without jumps.
- [ ] Recentre behaves; no horizon tilt; no latency spikes that break comfort.
- [ ] No tracking loss when the headset is briefly occluded; recovery is clean.
- Evidence: notes with timestamps, any `UpdateWorn` / pose log anomalies.

## 4. Move controller input and hand tracking

- [ ] Both Move controllers are discovered and assigned to the correct hands
      (`swap_hands=1` preserved; left/right match the on-screen hands).
- [ ] Pointer aim is stable; buttons (Move, trigger, cross/circle/square/triangle, options) map to
      the documented actions; no stuck or latched input.
- [ ] Menu navigation and in-game interaction work without the headless synthetic switches.
- [ ] Hand poses/orientation are plausible (no mirrored or offset hands).
- Evidence: input mapping notes, screenshots of menus that require pointing.

## 5. In-game audio and SNDX

- [ ] Music, ambience and voice play from the expected device with no dropouts or crackle.
- [ ] No SNDX/Fiona/NGS2 initialization failure; 1.00 and 1.01 both boot.
- [ ] The SNDX executable patch is not applied to 01.01 (version-gated) and the launcher reports the
      detected title/version and patch state accurately.
- [ ] Long-session audio (>= 10 min) does not degrade.
- Evidence: log lines for NGS2/Audio3d init, launcher report, device used.

## 6. Colour correctness

- [ ] Neutral whites, no red/blue cast, no double-encoded (washed-out) or double-decoded (crushed)
      image; skin tones and the sepia classroom grade look intended.
- [ ] Compare captured frames against a reviewed reference with
      `python tools\benchmark\image_validation.py compare --reference ... --candidate ... --json`;
      a `fail` with `channel_swap_suspected` blocks acceptance.
- [ ] Note explicitly that the earlier red-overlay corruption has **no established root cause**; a
      clean headset frame is evidence, not proof that the defect cannot recur.
- Evidence: reference and candidate captures, tolerance file, verdict JSON.

## 7. Presentation resolution and performance

- [ ] Presentation size is the runtime's recommendation (~3036×3036 per eye on the target setup)
      unless a resolution is explicitly selected; no unexpected up/downscale blur.
- [ ] Unique rendered FPS measured from the emulator's frame-stats series (not compositor cadence),
      at the requested configuration; record draws/Mpx/passes and GPU busy.
- [ ] 120 unique FPS at ~3036×3036 is the stated target, not a pass/fail gate for this acceptance.
- Evidence: `headless-gameplay.ps1` or the classroom runner's `run.json` + log; state the exact
  scenario and resolution profile.

## 8. Shutdown and recovery

- [ ] Clean exit through the normal window-close path: no assertion, no hang, no orphan
      `shadps4`/`vrserver`/`vrcompositor` process left behind (R6).
- [ ] With `SHADPS4_EXIT_AFTER`/`SHADPS4_CLEAN_EXIT` set **only for this test**, the process exits
      with code 0 on its own, teardown markers present, and no `GImGui` assertion.
- [ ] A second launch after a clean exit succeeds without a stale save or lock.
- [ ] Process ownership: the benchmark runner stops only its own process; another session's
      emulator is untouched (verified offline by `test_process_ownership.ps1`, 13 cases).
- Evidence: exit code, shutdown log lines, `Get-Process shadps4` empty afterwards.

## 9. Acceptance outcome

Record for each section: pass / fail / blocked, with evidence paths and the exact environment.
A failure in 1–6 blocks acceptance; 7 is recorded (the 120 FPS target is not a gate); 8 must pass.
Remaining headset-only blockers are listed in the acceptance checkpoint report.

### Known blockers before this checklist can pass

- M2/M3 headless evidence says nothing about headset optics, comfort, latency or audio.
- The classroom benchmark's private shadow needs its one-time manifest
  (`pwsh -File tools\benchmark\new-shadow-manifest.ps1 -ShadowRoot <shadow> -Verify`); the
  integration agent did not modify the private lab.
- The gameplay eye reference is not installed (candidate only, never visually reviewed).
