# Déraciné classroom benchmark and validity gate

Source-controlled replacement for the shared `_build/bench` harness. It measures one thing: the
opening **classroom with the main-menu overlay** at the pinned guest configuration, using the
emulator's own rendered-frame counter as the FPS source. It refuses to report an FPS for a run
that did not prove the scene, the configuration, the window and the protected-file state.

Scenario ID: `deracine-classroom-menu-300-msaa2-vblank60`.

## Headless gameplay benchmark (M2)

A second, dedicated benchmark lives beside the classroom one. `headless-gameplay.ps1` drives
Déraciné into **gameplay** with no physical headset and no OpenXR host (synthetic Move hands plus
the committed M2 input sequence), then validates the run against `reference/scene-gameplay.json`
with `gameplay_gate.py`. It is a separate profile because the menu (~465–500 draws) and gameplay
(~1,866–1,900 draws) are different scenes and must never share a gate. The gate, band, unique-FPS
arithmetic and the save-slot pin are unit-tested without a GPU
(`tests/test_gameplay_gate.py`). Procedure, opt-in table and single-lab status:
`../headless-vr/HEADLESS-M2.md`.

## What is measured, and what is not

| measured | source |
|---|---|
| unique rendered FPS | `SHADPS4_FRAME_STATS` windows: **total frames / total measured time** (`Σ frames / Σ(frames/fps)`), computed by the emulator draw scheduler (`FrameStats::EndFrame`) |
| window-average frame time p50/p90/p95/p99 | percentiles of the per-window mean frame time, `1000/fps` — **not** per-frame timing |
| scene identity | a render-target dump compared with the reviewed reference by normalised 64×36 grayscale correlation |
| workload | draws, passes and target Mpx per window, from the same frame-stats line |

**Not** measured or accepted as FPS:

* `xrBeginFrame` / present / compositor cadence. The old harness auto-selected a `TIMELINE
  xr_begin_end` series and reported 120 fps for a 41 fps scene because the composer presents at
  120 Hz; the validator rejects any timing source that is not the rendered-frame series.
* GPU-busy × frame-time estimates. They are not per-pass GPU timing.
* gameplay. The classroom is the front end; menu selection and gameplay are separate gates.

The measured window is 80 consecutive 0.5 s windows after a warmup of at least 10 s from the
scene anchor. Each window closes at the first frame boundary after 0.5 s, and a GPU hitch inside
a window is part of it, so the measured span is 40 s plus quantization and stalls. The run
records the emulator-measured duration (`Σ frames/fps`); the validator accepts 39.0–48.5 s for
this 40 s scenario and rejects a claimed 40 s that really lasted 0.65 s.

## Colour (RGB) validation

Scene similarity is **identity only**. `scene_signature.py` correlates a normalised 64×36
grayscale image, so it is deliberately insensitive to hue: an R/B channel swap, a serious
colour-processing regression, a black frame or a missing stereo eye can still be "the right
scene". Any claim of colour correctness therefore needs the separate colour gate,
`image_validation.py`, in addition to the scene gate.

**Neither gate replaces a headset visual check.** Both work on a render-target dump, not on what
the compositor/headset actually displayed, and neither authorises a visual-quality claim on its
own.

Motivation: on 2026-11-17 a captured gameplay frame showed large, soft red regions over otherwise
correct geometry (the red-overlay screenshot in `_run\pc-vr-recycler-try\evidence\`). The **root
cause is not established**: the recycler hypothesis in
`docs/assignment01/REVISED-CONCLUSIONS.md` is unproven, and that document's addendum later found
that the diagnostic PNG dump itself can write R and B swapped. A dump-level RGB gate cannot tell a
renderer defect from a dump defect by itself, which is why it reports a channel swap explicitly
instead of folding everything into one score.

`image_validation.py` compares a candidate with the reviewed reference and reports:

* per-channel R/G/B (and A when both images carry it) mean absolute error, max, p95/p99, and the
  fraction of pixels whose worst colour channel differs by more than `pixel_diff_threshold`;
* per-region MAE on a configurable grid (default 4×4), with the worst region and its coordinates;
* black-frame detection (near-zero fraction per channel, whole image and per stereo half), a
  uniform-image check, and stereo side-by-side missing/duplicated eye detection;
* R/B channel-swap detection: the direct MAE is compared with the MAE after swapping the
  reference's R and B; `channel_swap_suspected` is reported when the swapped comparison is
  materially better;
* an optional provenance check: the record's reference sha256 (and, when present, its candidate
  sha256 and capture dimensions) is verified against the files; a mismatch fails the verdict and is
  never silently ignored.

It never imports `scene_signature.py`; every result carries `scene_identity_untouched: true`.
Verdicts are `pass`, `fail` and `inconclusive`; black, uniform or unreadable data is
`inconclusive`, never a silent pass. Defaults are documented in
`reference/image-tolerances.json` (`max_mae` 2.0, `max_channel_mae` 3.0, `max_p99` 12,
`max_bad_fraction` 0.01, `max_region_mae` 6.0, plus the black/stereo keys); unknown tolerance keys
are rejected instead of silently falling back to a default.

A provenance record is a JSON object such as
`{"reference": {"sha256": ...}, "source": {"commit": ...}, "executable": {"sha256": ...},
"capture": {"dimensions": [...], "conditions": "..."}}`. `reference.sha256` is required; the other
fields are recorded in the result for the report, and any recorded value that can be checked
against the images (candidate sha256, reference/capture dimensions) fails the verdict when it
disagrees. Provenance is never silently dropped.

```powershell
# colour gate for one candidate frame (exit 0 pass / 2 fail / 3 inconclusive or usage)
& $PY $WT\tools\benchmark\image_validation.py compare `
  --reference $REF --candidate $CAND `
  --tolerances $WT\tools\benchmark\reference\image-tolerances.json `
  --provenance $PROV --json
```

`compare()` is the same function the harness can import (`from image_validation import compare`);
the CLI is a thin wrapper. The image gate and the scene gate are independent: a run must pass both,
and a scene-signature score is not colour evidence.

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
  gameplay_gate.py      headless gameplay gate: frame-stats parse, scene gate/band, unique FPS,
                        save-manifest verification (standard library only)
  image_validation.py   RGB/colour gate: per-channel and per-region statistics, black/missing-eye/
                        channel-swap detection, optional provenance check (Pillow/numpy; never
                        computes the scene signature)
  headless-gameplay.ps1 one headless gameplay cell: lab, launch, window, normal quit, run.json
  schema/run.schema.json
  reference/            reviewed classroom reference signature + preview, scene-gameplay.json, and
                        image-tolerances.json (documented colour-gate defaults)
  fixtures/             negative-control images (SIE splash)
  tests/                unittest suites for the validator, the gameplay gate, the colour gate and
                        the path guard
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

* Frame-time percentiles are percentiles of 0.5 s window means (`1000/fps`), not of individual frame durations; p95/p99 describe windows. A per-frame timing capture is not implemented (future work).
* The classroom front end is not gameplay; nothing here validates quests, Move input or the
  120 FPS target, which need a worn headset and a real session.
* The scene reference is one reviewed frame; a large lighting change in the scene would require
  re-reviewing the reference, not lowering the threshold.
* The validator checks the screenshot against the reference and every in-window sample against a
  90 %-of-window entry gate plus a 250 draws / 250 Mpx floor; it does not recognise objects, and
  the overlay presence comes from matching a reference that contains it.
* The run records the emulator's frame-stats series and the raw log, but the validator hashes the
  log rather than re-parsing it. The runner counts frame-stats lines it could not read whole in
  `timing.incomplete_stats_lines`; a partial line inside the measured window would shorten the
  series without being separately detected.
* RenderDoc capture is a separate step: `runner.ps1 -RenderDocCaptureDir <dir> -RdocCapture <s>`
  launches the app through `renderdoccmd capture`, finds the capture in the app's own
  `user\captures\CUSA11919_frameNNN.rdc` location, copies it into the run, extracts its final
  backbuffer image, and records both in `run.json`; the validator checks the capture hash and the
  extracted image against the classroom reference. The injected attempt on this machine
  device-lost before it could capture.
