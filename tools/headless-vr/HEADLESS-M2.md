# Headless gameplay — M2 integration record and benchmark

Status: **integrated into `integration/deracine-unified-2026-11`; the C++ tests and the benchmark
gate are green offline.** The gameplay entry itself is **single-lab verified** (one independently
constructed lab). It is **not** yet claimed reproducible from a second independently constructed
lab; a cell produced here is single-lab evidence until that passes.

This document covers the M2 milestone only. M1 (headless front end) is in
[`README.md`](README.md) and [`HEADLESS-ONLY.md`](HEADLESS-ONLY.md). M3 (A–E renderer validation) is
not part of this integration.

## What M2 is

Déraciné's front end is Move-driven. With no OpenXR host there is no tracked hand, so the menu can
never be confirmed and gameplay is never reached. M2 adds **test-only** support that stands in for
a tracked Move and drives the menu with a committed script:

- synthetic tracked hands placed where the runtime already puts the gamepad controller
  (`SHADPS4_VR_HANDS`), and the emulated pad's buttons/trigger mapped to the Move's own bits
  (`SHADPS4_VR_MOVE_FROM_PAD`);
- a scripted-input timeline that can be timed by the guest's frame count as well as by the clock,
  with per-hand Move button/trigger overrides (`SHADPS4_INPUT_SCRIPT`, the existing mechanism);
- a test-only timed quit and a clean presenter shutdown (`SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT`)
  so an unattended run ends through the emulator's own exit path;
- a scene-gated HMD eye dump (`SHADPS4_DBG_DUMP_HMD_AT_SCENE`) so a headless run can prove it is
  looking at gameplay without knowing the frame;
- a test-only Move read trace (`SHADPS4_DBG_MOVE_TRACE`) that shows what the guest is actually told.

The measured workload is genuinely different from the menu: **gameplay ≈ 1,866–1,900 draws/frame,
620–720 Mpx/frame, 42–46 passes/frame, 30–35 fps**, versus the menu's ~465–500 draws / ~480 Mpx /
~65 passes. The existing classroom benchmark cannot be used for gameplay numbers.

## Opt-in boundaries (verified in the merged tree)

Every new behaviour is off unless its variable is set. A normal headset session sets none of them.

| variable | default | effect when unset |
|---|---|---|
| `SHADPS4_VR_HANDS` | off | the host's hand is returned; no synthetic hand is created |
| `SHADPS4_VR_MOVE_FROM_PAD` | off | the emulated pad does not touch the Move input |
| `SHADPS4_INPUT_SCRIPT` | off | no scripted input is loaded (existing mechanism) |
| `SHADPS4_DBG_AUTOPRESS` | off | no test input is injected (existing mechanism) |
| `SHADPS4_EXIT_AFTER` | off | no timed quit |
| `SHADPS4_CLEAN_EXIT` | off | the normal `quick_exit` path is unchanged |
| `SHADPS4_DBG_DUMP_HMD_AT_SCENE` | off | no scene-gated dump |
| `SHADPS4_DBG_MOVE_TRACE` | off | no read trace |

No output, timing, tracking or input path changes for a normal run. The integration adapted the
commits only where they depended on the cross-fork lineage (see *Integration notes* below).

## Reproducible procedure

Prerequisites: a Déraciné dump (an `eboot.bin`; a private shadow with its `saved/engine.ini` carries
the 300% preset), an executable built from a commit carrying M2, and a savedata slot 1 matching
[`save-slot/MANIFEST.json`](save-slot/MANIFEST.json). Python 3 is needed for the validator; the
benchmark uses only the standard library.

```bash
# one GPU-lock hold; headless only, no SteamVR and no headset
bash <worktree>/tools/headless-vr/gpu-lock.sh run --owner <you> --kind headless \
  --reason "headless gameplay cell" --ttl 1800 --require-quiet -- \
  pwsh.exe -NoProfile -File \
  <worktree>/tools/benchmark/headless-gameplay.ps1 \
  -Exe <worktree>/build/shadPS4.exe \
  -Eboot D:/Temp/Deracine/CUSA11919-qual/eboot.bin \
  -SavesFrom <save>/CUSA11919 \
  -Out <out>/headless-gameplay/r1
```

The runner creates an isolated lab from the tracked `pc-vr/` template (never the active runtime),
verifies the save against the manifest, pins the M2 environment, launches the emulator with
`-g <eboot>`, waits for the normal quit, and then runs the gate. `-DryRun` prints the inputs and
environment without launching; `-ReplayLog <file>` evaluates an existing log.

Verdict (exit 0 valid / 2 invalid / 1 execution error) comes from
[`gameplay_gate.py`](../benchmark/gameplay_gate.py) against
[`scene-gameplay.json`](../benchmark/reference/scene-gameplay.json):

- gate: at least 2 consecutive windows with **draws ≥ 800** and **Mpx ≥ 450**;
- band: draws 1500–2300, Mpx 550–750, passes 35–55, with ≥ 90 % of the measured window inside it;
- at least 5 window samples;
- **unique rendered FPS = total frames / total measured time** (`Σ frames / Σ(frames/fps)`), never
  a mean of per-window rates, and never OpenXR compositor cadence;
- the save files are verified against the manifest on every run.

The menu cannot pass the 800-draw gate, so a run that never leaves the menu is reported
`phase=menu`, not as gameplay. The unit suite pins the positive control and each negative case:

```powershell
python -m unittest discover -s tools\benchmark\tests -p "test_gameplay_gate.py" -v   # 14 tests
```

An optional gameplay eye reference is deliberately **not** installed: a candidate exists
(`_build/headless/validation/evidence/gameplay-reference-candidate/`) but was never visually
reviewed. Scene identity therefore rests on the numeric gate+band, which the menu cannot satisfy.

### Reproducibility notes

The M2 path above is self-contained in this repository: the lab template is the tracked `pc-vr/`,
the input sequence and the save manifest/provisioner are committed, and the only external inputs are
a game dump and a save you supply. It reads nothing from `_build/`; the `_build/...` references in
this document are provenance/evidence locations, not dependencies.

The M1 launcher (`headless-vr-launch.ps1`) predates that and still defaults `-TemplateLab`,
`-SavesFrom` and its run directory to the original machine's paths. It is parameterised, so use it
without that lab by overriding all three:

```powershell
pwsh -File tools\headless-vr\headless-vr-launch.ps1 -Exe <build>\shadPS4.exe `
  -Eboot <game>\eboot.bin -TemplateLab <repo>\pc-vr -SavesFrom <your save>\CUSA11919 `
  -Lab <dir>\lab
```

The dedicated gameplay benchmark (`headless-gameplay.ps1`) does this by default and is the
recommended entry point.

## Evidence already recorded (single lab)

- Gameplay entry: `_build/headless/probe-scene/evidence/m2-confirm-1` (`run.cmd`, `input-script.txt`,
  `shad_log.txt`, eye dumps) — menu → Continue → loading → gameplay in ~49–65 s, from the slot-1 save.
- Workload and bands: `_build/headless/validation/results/RESULTS.md` §2 (M2-confirmed).
- Save provenance and hashes: `_build/headless/validation/results/SAVE.json` and
  [`save-slot/MANIFEST.json`](save-slot/MANIFEST.json).
- The committed input sequence is that run's script: [`input/gameplay-entry.txt`](input/gameplay-entry.txt).

These are **inputs to** the benchmark, not a benchmark result: a fresh cell still has to be run
inside the GPU lock, and this integration did not launch the emulator.

## Integration notes (why the commits are not verbatim)

The M2 branch was authored on the live runtime lineage (`de9c78c` + cross-fork), so its commits were
cherry-picked onto the unified base, not merged. Two dependencies were re-expressed:

1. `d10fe14` (scene-gated HMD dump) used the lineage's `Tally::PeekWindow()` and the scene-gated
   RenderDoc from `3d375a4`. The RenderDoc scene gate is **not** imported (this tree keeps the
   time-based trigger); `PeekWindow()` is added locally as a non-consuming read, and the dump
   request/presenter code is the original.
2. `94998e0`'s diff moved a `core/vr/frame_timeline.h` include that exists only on the lineage;
   the 3-way merge kept the moved include, which was removed. Nothing in the file used it.

No recycler code, no LUT work and no `633d212` diagnostics arrived with M2. The C++ suite on the
integrated branch: **776 tests, 772 passed, 4 failed** — the same four inherited failures as the
unified branch (gcn `_NOT_BUILT`, usbd `_NOT_BUILT`, `ReadConstSnapshotDiag.IsolationDisabledAfterTestB`,
`BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`); the six new
`ScriptedTimeline.*` tests pass.

## Limits

- Single lab only; the second independently constructed lab has not been run. Do not quote a cell
  from this infrastructure as generally reproducible yet.
- No physical headset: no latency, comfort, distortion or compositor claims.
- Synthetic hands and scripted input are test-only; they must never become a production default.
- The private game shadow's `saved/engine.ini` carries the 300 % preset; the benchmark records the
  eboot hash and the environment but cannot pin that file from this repository.
