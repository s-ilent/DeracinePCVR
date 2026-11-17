# Déraciné classroom benchmark and validity gate

Source-controlled replacement for the shared `_build/bench` harness. It measures one thing: the
opening **classroom with the main-menu overlay** at the pinned guest configuration, using the
emulator's own rendered-frame counter as the FPS source. It refuses to report an FPS for a run
that did not prove the scene, the configuration, the window and the protected-file state.

Scenario ID: `deracine-classroom-menu-300-msaa2-vblank60`.

## What is measured, and what is not

| measured | source |
|---|---|
| unique rendered FPS | `SHADPS4_FRAME_STATS` windows: **total frames / total measured time** (`Σ frames / Σ(frames/fps)`), computed by the emulator draw scheduler (`FrameStats::EndFrame`) |
| frame time p50/p90/p95/p99 | the mean frame time of each 0.5 s window, `1000/fps` |
| scene identity | a render-target dump compared with the reviewed reference by normalised 64×36 grayscale correlation |
| workload | draws, passes and target Mpx per window, from the same frame-stats line |

**Not** measured or accepted as FPS:

* `xrBeginFrame` / present / compositor cadence. The old harness auto-selected a `TIMELINE
  xr_begin_end` series and reported 120 fps for a 41 fps scene because the composer presents at
  120 Hz; the validator rejects any timing source that is not the rendered-frame series.
* GPU-busy × frame-time estimates. They are not per-pass GPU timing.
* gameplay. The classroom is the front end; menu selection and gameplay are separate gates.

The measured window is 80 consecutive 0.5 s windows after a warmup of at least 10 s from the
scene anchor. The emulator closes a stats window at the first frame boundary after 0.5 s, so the
80 windows cover `40 + 80/fps` seconds; the run records the emulator-measured duration
(`Σ frames/fps`) and the validator requires it within 2 s of the scenario's 40 s. The first and
last sample timestamps and the runner-observed duration are recorded as well, so a claimed 40 s
window that really lasted 0.65 s is rejected.

## Files

```
tools/benchmark/
  runner.ps1            one pinned session: preflight, launch, scene gate, screenshot, warmup,
                        window, stop, postflight, run.json, self-validation
  preflight.ps1         hashes protected files/worktrees before a session
  postflight.ps1        recomputes them; fails on unexpected change
  validator.py          independent validity gate (exit 0 valid / 1 invalid / 2 usage)
  report.py             valid-only report; separate observed / estimated / informational
  compare.py            A/B with workload equality, content-drift and noise-floor gates
  scene_signature.py    signature, preview and size helpers for scene screenshots
  path_guard.py         allowlist/denylist guard for automated file operations
  json_schema_lite.py   schema-structure check for run.json
  schema/run.schema.json
  reference/            reviewed classroom reference signature + preview
  fixtures/             negative-control images (SIE splash)
  tests/                unittest suites for the validator and the path guard
  README.md
```

## Commands

All commands are noninteractive. `$PY` must be a Python with `Pillow` and `numpy`
(the validator and scene signature use them); no other dependency is needed.

```powershell
$WT = 'M:\Development\Deracine-work\wt-autonomous-guardrails'
$PY = 'C:\Users\Arukas\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'

# one session: preflight -> launch -> scene screenshot -> warmup -> 40 s window -> postflight
pwsh -NoProfile -File $WT\tools\benchmark\runner.ps1 `
  -Exe $WT\build\shadps4.exe -Out $WT\_out\runs\classroom-r1 -Capture -Python $PY

# the validity gate on its own (never trust run.json's own block)
& $PY $WT\tools\benchmark\validator.py run $WT\_out\runs\classroom-r1

# valid-run report
& $PY $WT\tools\benchmark\report.py $WT\_out\runs\classroom-r1

# A/B: both runs must be valid, the same workload, and content within 10 %
& $PY $WT\tools\benchmark\compare.py $WT\_out\runs\a $WT\_out\runs\b --noise-floor-pct 10.0

# the gate's own tests: 10 prior-failure negatives plus positive controls
& $PY -m unittest discover -s $WT\tools\benchmark\tests -v
```

The runner calls `validator.py` at the end and exits with its status: a completed but invalid
session is a non-zero result, not a benchmark number.

## The pinned session

| item | value |
|---|---|
| worktree | the assigned private worktree, never `DeracinePCVR` |
| lab runtime | `<worktree>\_lab\pc-vr` (private copy; the runner swaps in the executable under test) |
| game shadow | `D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17` (private eboot copy, private `gameproject/saved`, read-only junctions to shared content) |
| guest | `r.ScreenPercentage=300`, `r.MSAACount=2`, 2880×3240 per eye, 5760×3240 packed stereo |
| presentation | `SHADPS4_VR_SCALE=0.5`; actual swapchain read back from the session log |
| vblank | `-Vblank 60`, written to `config.json` and read back (`vblank_actual_hz`) |
| automation | `SHADPS4_DBG_AUTOPRESS=1` (or `pad`/`move`); logged explicitly in `run.json` and by each burst |
| timing | `SHADPS4_FRAME_STATS=1`, `SHADPS4_FRAME_STATS_EVERY=0.5`; each line carries the window's
|          | frame count, so the measured window duration is the emulator's own `Σ frames/fps`, not
|          | the runner's polling interval |
| scene gate | two consecutive windows with `draws >= 400` and target `Mpx >= 500`, **and** a screenshot matching the reference at similarity ≥ 0.85 |
| warmup / window | 10 s / 40 s (80 × 0.5 s windows), warmup counted after the last scene dump |
| screenshot | render-target dumps at `-CaptureOffsets` (default 12 s and 25 s after the anchor, bounded
|            | retry until the image matches the reference), `SHADPS4_DUMP_WIDTH=1280` |

The runner records the requested **and actual** values it can observe (vblank read back from the
config, presentation size read from the log), plus the guest config written into the shadow.

## run.json

`schema/run.schema.json` is the structure. `run_id`, `created_utc`, `scenario`, `source`
(worktree, HEAD, dirty manifest), `executable` (inspected + lab-copied hashes), `host`
(GPU/driver/XR runtime/headset), `env`, `timing` (source, raw samples, window boundaries,
percentiles, unique FPS), `scene` (classification, anchor stats, screenshot hash), `cleanup`
(allowed paths only), `provenance` (preflight manifest, protected-file result) and `artifacts`.

`validator.py` recomputes every number from `per_sample` and from the files on disk. It rejects,
among others:

1. compositor/present cadence used as FPS;
2. a measured window that is not 80 windows / about 40 s, or rows that do not carry the frame
   counts the duration is derived from;
3. a light front end or SIE splash screenshotted while carrying high FPS numbers;
4. 500+ draws with a missing or wrong screenshot (draw count alone is never scene proof);
5. an in-window sample that drops below the scene gate, or missing autopress, vblank, guest
   resolution, GPU/driver or executable hash;
6. mixed 60/120 Hz cells (compare.py refuses the comparison);
7. an empty/traversal/out-of-root cleanup destination (validator) or an unsafe file operation
   (path guard);
8. truncated, overlapping, nonmonotonic or inconsistent samples, including an `INVALID.txt`
   marker from a failed attempt;
9. metadata naming one executable while a different file was run;
10. content drift between A and B (compare.py refuses the comparison);
11. an actual presentation/guest size that differs from the requested one.

Invalid runs get `valid:false`, a reason list, no headline FPS and no place in a summary table.
The raw log and the invalid run directory are preserved.

## Path guard: preventive where it runs, detection otherwise

`path_guard.py` is called by the runner before its copies, deletes and the session-state writes
(lab executable and settings, engine.ini, run directory artifacts); the guard config itself is
written before the first destructive operation. It refuses empty paths, relative paths, `..`
components, paths outside the allow roots, paths under a deny root, and reparse points that
resolve outside the allowed root. It does not sandbox arbitrary processes: this machine runs
everything under one Windows token, so a tool that ignores the guard can still write anywhere the
user can. `preflight.ps1`/`postflight.ps1` are the barrier that covers that gap: they hash the
protected files before and after and the run is invalid if anything changed. Both are detection,
not OS enforcement; no ACL or permission was changed.

## Reference image

`reference/scene-classroom-menu.json` is the signature of the recovered benchmark frame
`fixA2/captures/capture_02/009_...png`: the classroom with the floating **New game / Load game /
Continue / System** overlay, side-by-side stereo, 1280×720 downscale of the 5760×3240 eye
picture (SHA-256 `8b64aba9…`). `reference/scene-classroom-menu.png` is the 320×180 preview that
was visually reviewed for this purpose. The measured separation is wide: same-scene frames score
0.93–1.00, an SIE splash scores −0.05.

Replacing the reference is a reviewed change: extract the signature with
`scene_signature.py extract`, save the preview, state the source and hash in the note, and keep
the old reference in history.

## Known limitations

* Frame times are 0.5 s window means, not individual frames: p95/p99 describe windows.
* The classroom front end is not gameplay; nothing here validates quests, Move input or the
  120 FPS target, which need a worn headset and a real session.
* The scene reference is one reviewed frame; a large lighting change in the scene would require
  re-reviewing the reference, not lowering the threshold.
* RenderDoc capture is a separate step (`runner.ps1 -RdocCapture <s>` plus `renderdoccmd`); the
  `.rdc` is not produced by the runner itself.
