# Headless gameplay handoff (integration-ready)

Status 2026-11-18: M1 verified, M2 verified in two independently constructed labs, M3 harness
committed; the A–E matrix and integration are the remaining steps.

## Branches and commits

**Harness/tooling** — `feature/headless-vr-harness`, based on `1a53ceb`:

| commit | what |
|---|---|
| `5cee4a3` | M1 launcher + GPU lock + HEADLESS-ONLY directive + README |
| `6417f36` | M1 verified result recorded |
| (this commit) | M2/M3 gameplay cell runner, inputs, A–E validation runner |

**Emulator support** — `feature/headless-vr-scene`, based on `1a53ceb`, head `a21109c`:

| commit | what |
|---|---|
| `cd8fe36` | synthetic Move hands (`SHADPS4_VR_HANDS=synthetic|1`) + Move input from the pad (`SHADPS4_VR_MOVE_FROM_PAD=1`) |
| `da92b03` | frame-relative script steps (`frame=`, `frames=`), guest frame counter, `move0=`/`move1=` overrides + 6 gtests |
| `d10fe14` | scene-gated HMD eye dump (`SHADPS4_DBG_DUMP_HMD_AT_SCENE`) |
| `94998e0` | `SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT=1` with bounded guest-pause safety and an explicit R6-blocked log |
| `832c144` | mirrored hand-1 aim (scripted yaw/roll mirrored, pitch shared) |
| `a21109c` | `SHADPS4_DBG_MOVE_TRACE` read trace |

Build: `wt-headless/build/shadPS4.exe`, 73,130,496 B, sha256
`6B2605231E6733B2A047752BF04DA13B4004ABD5E1033C326D0F0438A62AC379`, Release/Ninja/clang-cl 23.1.5.

## Dependencies

- Base renderer candidate: `integration/cross-fork-renderer` @ `1a53ceb` (recycler off by
  default; `1dd4eb4` is docs-only on top of it).
- A build with the six commits above (`wt-headless/build/shadPS4.exe`); the harness's gameplay
  mode refuses without the new envs (it records the missing ones as the failure reason).
- Game: Déraciné CUSA11919, eboot `D:\Temp\Deracine\CUSA11919-qual\eboot.bin` (300 %/2x engine
  config in the same shadow).
- Save fixture: save slot 1 `MGS_GameData_1` (stage `m00_00_10_10`), `MGS_SystemData`,
  `sce_sys/param.sfo` from `_run/pc-vr-recycler-try/user/home/1000/savedata/CUSA11919`. The
  harness copies/verifies this; do not commit user saves or game content.
- Windows x86_64, RTX 3090 Ti; the GPU lock (`tools/headless-vr/gpu-lock.sh`) serializes runs.
- No OpenXR runtime, no headset, no SteamVR interaction.

## Verified evidence

- M1: `_build/headless/runs/m1-candidate-r3`, `m1-candidate-r5-r3` — 2880x3240/eye, 0 worn lines,
  front end 472.7 draws/485.3 Mpx/63 passes.
- M2: `_build/headless/probe-scene/evidence/m2-confirm-1` (menu → Continue → loading → gameplay)
  and `m2-minimal-1`; gameplay ~1866 draws / 620–720 Mpx / 42–46 passes / 30–35 fps.
- M3 portability (fixed harness, recycler off): `_build/headless/runs/m2-off-fixed-1` (1866.3
  draws/606.2 Mpx/41.4 passes, fps p50 33.5) and `m3-portable-2` (1865.8/550.2/38.4, p50 31.8),
  each in a freshly constructed lab, with gameplay captures.
- Gameplay reference image (not tracked): `_build/headless/references/gameplay-eye.png`, sha256
  `110DAC1F…`, from `m3-portable-2/frames/gameplay-postwindow__011_…png`.

## Reproduction

M1 (front end):

```
bash tools/headless-vr/gpu-lock.sh run --owner <you> --kind headless \
  --reason "headless M1" --ttl 900 --require-quiet -- \
  pwsh.exe -NoProfile -File <root>/_build/headless/headless-vr-launch.ps1 \
  -Exe <root>/wt-crossfork-int/build/shadPS4.exe
```

M2/M3 (gameplay):

```
bash tools/headless-vr/gpu-lock.sh run --owner <you> --kind headless \
  --reason "headless gameplay" --ttl 1800 --require-quiet --quiet-wait 900 -- \
  pwsh.exe -NoProfile -File <root>/_build/headless/headless-vr-gameplay.ps1 \
  -Exe <root>/wt-headless/build/shadPS4.exe -Label my-cell -Arm off -Gate gameplay \
  -Seconds 40 -Warmup 10 -MaxSeconds 420 -Attempts 2 -GameplayMinDraws 800 -Capture
```

A–E matrix: `pwsh -File tools/headless-vr/validation/run-validation.ps1 -PrintPlan` then the
smoke pair and the A/B/C/D/E cells (see `results/COMMANDS.md`). Modes: `-Mode cell|aba|rdoc|lowres`.

## Scene definition (do not substitute the front end)

| scene | draws | Mpx | passes |
|---|---|---|---|
| menu/front end | 465–500 | ~480 | ~65 |
| loading | 42–55 | — | — |
| gameplay | 1500–2300 | 550–750 | 35–55 |

The gameplay eye image shows the classroom with the spectral hands and the girl at a desk; the
menu UI is gone. The brief's 480–494 does not describe this scene. Measure unique rendered
frames from `frame stats` windows, never present/OpenXR cadence.

## Defaults and integration rules

- All headless support is env-gated and **default off** in ordinary VR gameplay:
  synthetic hands/Move-from-pad only when explicitly set; `SHADPS4_EXIT_AFTER`/`CLEAN_EXIT` and
  the scene dump only when set. No renderer path changes; the recycler remains off by default.
- The harness's gameplay mode sets the M2 envs explicitly for its own cells; it does not change
  the game's settings or the active `pc-vr` runtime.
- Do not merge/deploy into the active runtime from this workstream. Integration order for the
  unified branch (`integration/deracine-unified-2026-11` @ `e3c1694`, which predates M2):
  1. merge/cherry-pick `feature/headless-vr-scene` (`cd8fe36..a21109c`), then
  2. take `feature/headless-vr-harness` for the tooling, then
  3. re-run M1/M2/M3 on the integrated build with the same save fixture and gates.
- Tests: `wt-headless/build-tests` target `shadps4_scripted_timeline_test` (6 tests) passes; a
  full `-Tests` tree hits only the two documented Windows baseline build failures. Run a fresh
  ctest on the integrated build and distinguish those failures.
- Known startup crash `address_space.cpp Protect: Unreachable code!` is pre-existing; the runner
  retries up to `-Attempts`.

## Limitations

- No physical-headset latency, comfort, optics or compositor validation. Desktop presentation is
  the test-only destination; the guest still executes its normal GNM/Vulkan/cache/depth work.
- The gameplay scene is the save's checkpoint; a different save/profile changes the view and the
  draw count. Keep the fixture and record its hashes in `meta.json`.
- Recycler correctness (red regions, R3) is under the validation workstream; the recycler stays
  off until the off/on gameplay pair and pixel comparisons pass.
