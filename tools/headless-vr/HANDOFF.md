# Headless gameplay — integration handoff (M3, 2026-11-18)

## Branch/commit map (history rewrite aware)

| workstream | rewritten base | commits |
|---|---|---|
| M2 emulator support | `c566db7` (rewritten `a21109c`) | `cd8fe36`, `da92b03`, `d10fe14`, `94998e0`, `832c144`, `a21109c` → `c566db7` |
| R6 ImGui repair | `c566db7` | `bc524dd` (`fix/r6-clean-shutdown`) |
| M3 harness (rewritten base) | `ae6781c` (rewritten `6417f36`) | `8ff9827`, `b9261bc`, `b0ada73` (`feature/headless-vr-harness-m3`) |
| M3 harness (original ancestry, preserved) | `1a53ceb` | `5cee4a3`, `6417f36`, `b0669aa`, `055c09d`, `27fd76f` |

No force-push and no old ancestry over the rewritten GitHub history. Portable patches:
`_build/handoff/patches/harness/000{1,2,3}-*.patch` and
`_build/handoff/patches/r6/0001-*.patch`.

## What the unified branch already has

`integration/deracine-unified-2026-11` (rewritten `cf10f9a5`; worktree `wt-unified`, detached
`88686f5` at the time of writing) contains its own M2 (`99085ad`, merged in `647b87b`) and a
tracked benchmark (`tools/benchmark/headless-gameplay.ps1`, `gameplay_gate.py`, tests,
`HEADLESS-M2.md`). Keep it; do not replace it.

## Missing M3 functionality to add

1. **R6 fix** `bc524dd` — `imgui_core.cpp` saves settings before the context is destroyed and
   makes the `at_quick_exit` callback context-safe/idempotent; new
   `test_imgui_exit_guard.cpp` (4 tests). Apply the patch; the unified build otherwise aborts
   with `GImGui != NULL` on the clean-exit path.
2. **M3 runner and inputs** `8ff9827` — `headless-vr-gameplay.ps1`, `inputs/menu-continue-move.txt`,
   `validation/run-validation.ps1`, `HANDOFF.md`, `M3-REPORT.md`. The unified benchmark is
   single-cell; this adds the menu→loading→gameplay gate, captures, A/B/A, RenderDoc, clean-exit
   and the A–E report machinery.
3. **Resolution profile and portability** `b0ada73` — 100 % profile (960×1080/eye, anchor by
   draws ≥ 800, Mpx 55–90 / passes 30–55 calibrated from a verified 100 % capture), `-Root/
   -SourceLab/-SavedataSource/-Shadow/-Eboot` parameters instead of machine paths.
4. **Save-slot manifest** `tools/headless-vr/save-slot/MANIFEST.json` — slot 1 stage
   `m00_00_10_10`, hashes; the runner refuses a slot-0-only lab.
5. **Two-lab evidence** for the M3 report: `runs/m2-off-fixed-1`, `runs/m3-portable-2`; use these
   to upgrade the unified `HEADLESS-M2.md` status from single-lab.

## Workload calibration warning

The unified build reaches the same classroom gameplay (1866 draws, eye 2880×3240, ~35.7 fps) but
measured **~860 Mpx / 55.7 passes**, versus the candidate's 550–650 / 38–46. The unified
benchmark's band (550–750 Mpx, 35–55 passes) rejects that live cell. Re-derive the unified band
from a captured unified gameplay frame before comparing any A/B numbers; do not reuse the
candidate band.

## Exact identities and evidence

- R6-validated build: `wt-r6/build/shadPS4.exe`, sha256
  `D231FA329473C015FE3C1A943B16E409A2B8DD20171B61842D272E1634CEAC63` (base `c566db7` +
  `bc524dd`); clean cell `runs/r6-clean-1-try2`, exit 0, no assertion.
- M2 validation build: `wt-headless/build/shadPS4.exe`, `6B260523…` (base `a21109c`).
- Candidate: `1a53ceb` / exe `7B658961…`; game CUSA11919 01.01; save fixture hashes in
  `tools/headless-vr/save-slot/MANIFEST.json`.
- M3 report: `_build/headless/M3-FINAL-REPORT.md` and `tools/headless-vr/M3-REPORT.md`.
- Raw saves, game data, `.rdc` captures, large images and logs are outside Git.

## Rules for the integration agent

- Do not modify the live `pc-vr` runtime, this worktree, or SteamVR/OpenXR services.
- Merge/cherry-pick the patches above; keep M2 defaults off (`SHADPS4_VR_HANDS`,
  `SHADPS4_VR_MOVE_FROM_PAD`, `SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT`,
  `SHADPS4_DBG_DUMP_HMD_AT_SCENE`, `SHADPS4_DBG_MOVE_TRACE` do nothing when unset). The
  recycler stays opt-in/off by default.
- Re-run the clean-exit C cell and one gameplay cell on the integrated build; record the actual
  workload band. Distinguish the two documented Windows baseline test failures from new ones.
- No physical-headset claim can be derived from these cells (latency, comfort, optics,
  reprojection, compositor, thermals).
