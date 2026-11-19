# Native frame mode, measured: where the 60-frame ceiling binds, and where it does not

What this is: the measurement of Déraciné's `frame_mode` — the group of Unreal settings that
`pc-vr/render-config.ps1` writes into the game's own saved configuration, offered as `frame_mode=60|120`
in `pc-vr/settings.txt`. `docs/psvr-morpheus-ue4-settings.md` owns what those settings are and where
they come from; this document owns only what they were measured to do.

**The feature removes a ceiling. It can therefore only be seen in a scene that is fast enough to reach
that ceiling.** `frame_mode=120` makes the title ask its headset path for one vblank wait per frame
instead of two, so where the renderer has room it doubles the frame rate, and where the renderer does
not it changes nothing. An earlier version of this document measured only the second case and called
the effect undetected; that was the wrong operating point for the question, and this document is
rebuilt around the three rows below.

## 1. The cap, measured in three scenes

Three arms of the M2 gameplay scenario, differing **only** in the shadow's `engine.ini` (§4): `a60` and
`c60` in 60 mode, `b120` in 120 mode. Each run passes through the front end, the title backdrop and
then gameplay, so all three scenes are present in the same three logs. FPS is the emulator's own
per-window rendered-frame series; `n` is the number of 0.5 s windows in that scene for that arm.

| scene (same three runs) | draws / Mpx (median) | 60 mode | 120 mode |
|---|---|---|---|
| calibration / front end | 32 / 208 | **59.9** (n=81), **60.0** (n=82) | **119.9** (n=77) |
| menu / title backdrop | 477–482 / 535–575 | 51.2 (n=19), 59.9 (n=31) | **61.2** (n=19) |
| gameplay at 300 % render | 1,865–1,866 / 872–914 | 34.7 (n=235), 36.7 (n=224) | 37.5 (n=240) |

(`a60` first, `c60` second, in every cell. p90 in the calibration band: 60.4 / 60.2 in 60 mode against
120.3 in 120 mode.)

- **The calibration band is the decisive one.** It is trivially cheap — about 30 draws and 200 Mpx per
  frame, the harness's own calibration signature — so neither arm is renderer-limited. The 60-mode arms
  sit at **exactly 60** (59.9, 60.0; p90 60.4 and 60.2) and the 120-mode arm at **exactly 120** (119.9;
  p90 120.3), across ~80 windows each. That is a ceiling being removed, not a renderer getting faster.
- **This reproduces, headless and with a large n, the positive result this feature was found with**:
  120.0 unique frames a second measured in a session on the same class of screen, with vblank waits per
  frame falling from 2.03 to 1.19. Current `main` does not instrument vblank waits (`0` such lines in
  these logs), so the rate itself is the evidence here; it is the same doubling.
- **The menu / title backdrop is the near-ceiling row**: it has GPU headroom, and only the 120-mode arm
  crosses 60 (61.2 against 59.9 and 51.2). The two 60-mode arms disagree with each other in this band
  and `n` is small (19–31 windows), so treat this row as consistent with the calibration row rather
  than as independent evidence.
- **Gameplay at 300 % is the below-ceiling row**: nothing is due from the feature there, and nothing was
  seen — 34.7 / 36.7 against 37.5, inside the 7.2 % run-to-run spread that two runs of the same 60-mode
  configuration showed (§5). §4–§7 record that comparison in full, because "the mode does nothing at
  300 %" is a true and useful statement about an overloaded renderer; it is simply not a statement
  about the feature.

The framing of which rows to expect is the reviewer's, and it is the reason this document exists in
this shape:

| scene performance without a 60 FPS cap | 60 mode | 120 mode |
|---|---|---|
| can sustain 120 FPS | limited to ~60 | can reach ~120 |
| can sustain 90 FPS | limited to ~60 | can reach ~90 |
| can sustain 55 FPS | ~55 | ~55 |
| can sustain 35 FPS | ~35 | ~35 |

*Illustrative ceilings, not measured results.* The measured rows above are the first, the third and the
fourth of that table; the second is not covered by these runs.

**Attribution.** `frame_mode=120` is a **group of five settings**, not `PSVRFrameSequence` alone: it also
sets `bSmoothFrameRate=False`, the 22..250 smoothed range, `t.MaxFPS=0` and `r.FinishCurrentFrame=0`.
Nothing here isolates any one of them, so the ceiling above is removed by **the configuration as a
whole**. (The one key measured in isolation anywhere is `r.FinishCurrentFrame`, historically and outside
this harness: 48.7 against 44.0 frames a second in the cold state.)

## 2. What this changes about the earlier negative reading

An earlier version of this document reported the 300 % gameplay comparison as "no effect detectable",
full stop. The measurement was correct; the scope was not. In 60-mode terms the feature is worth
exactly the frames the scene cannot otherwise reach above 60, and a scene running at 35 fps cannot
reach any. The corrected claims are:

- **The frame-mode group doubles the rendered rate in a scene with headroom** (59.9/60.0 → 119.9), which
  is what it exists to do.
- **It does not make an overloaded scene faster**: at 300 % render, the 60-mode and 120-mode cells are
  the same within this harness's noise, and that says nothing for or against the feature.
- **Whether it helps a *gameplay* scene therefore depends on that scene's cost**, i.e. on the render
  scale — which is the trade `render_percent` already expresses (§4.3 of the settings document). It
  needs a scene that can pass 60 at the chosen scale; the M2 gameplay cell at 300 % is not one.

## 3. Why no run at 100 % render is included

The M2 scenario's validity gate is `min_draws 800` **and `min_mpx 450`**, calibrated at 300 % render
(§4). At `render_percent=100` the same scene writes about a ninth of the pixels — roughly 100 Mpx — so
the cell fails its own gate and would not be a valid cell of that scenario. Re-deriving the band is a
deliberate act the ledger reserves for live cells, and mixing a re-derived band into this document's
comparisons is exactly what §3 of the ledger forbids. A gameplay-band run at 100 % is therefore
available to anyone who wants it **as a differently-scened measurement with its own band**, not as a
re-run of this scenario. The calibration row above carries the cap question on its own.

## 4. What was held fixed

| | |
|---|---|
| scenario | `deracine-headless-gameplay-300-msaa2-vblank120`, profile `tools/benchmark/reference/scene-gameplay.json` |
| gate / advisory band | `min_draws 800`, `min_mpx 450`, `consecutive_samples 2`; the profile's advisory band is draws 1500–2300, Mpx 550–750, passes 35–55 |
| runner | `tools/benchmark/headless-gameplay.ps1`, sha256 `9DC86093D1818848BF72C655E72C2C1828E444A38D2BE49264E6E96A70D60FB5` |
| executable | `_out/gpu01-depthpass-2026-11-19/bin/shadPS4.exe`, sha256 `8991510F22CB8D4EC376DB4CA3458C713E4B5E90780FD994A780812B144E635A` |
| game | private shadow `_out/morpheus-ab-2026-11-19/shadow`, `eboot.bin` sha256 `8DA8E46AC5FDC48E4E9B59E459920A266039993DC13A2D92740F8803E46FDF75` |
| save | `_build/unified/accept-lab2-save-sfo`, slot 1, stage `m00_00_10_10` |
| guest settings under test | `render_percent=300`, `guest_msaa=2` — identical in every arm |
| presentation | the runner's own defaults, identical in every arm |
| diagnostics | `SHADPS4_GPU_TIMING=1`, `SHADPS4_DBG_OVERLAP=1`, `SHADPS4_DBG_DEPTH_PASS=0` — the M2 cells' set, identical in every arm |

**Provenance trap, recorded as the ledger requires:** the runner's `run.json` `env` block does **not**
record `SHADPS4_GPU_TIMING`, `SHADPS4_DBG_OVERLAP` or `SHADPS4_DBG_DEPTH_PASS`. They are recorded per arm
in `_out/morpheus-ab2-2026-11-19/<arm>.mode.json`, not inferred.

The only thing that differs between arms is the shadow's `gameproject\saved\config\ps4\engine.ini`,
written by `pc-vr/render-config.ps1`. The 60-mode arms' file and the 120-mode arm's file hash to
`78C0D8AC…` and `1E895554…` — the same values as the earlier pair, so the control produced
byte-identical configurations across both batches.

## 5. The below-ceiling comparison in full (gameplay at 300 %)

The three arms, over their in-band gameplay windows. Frame times are 0.5 s window averages (`1000/fps`),
percentiles by the validator's own definition; GPU executing and command-thread times are the medians of
the `gpu timeline` line printed with each window.

| arm | mode | engine.ini | unique fps | p50 ms | p90 ms | p99 ms | GPU exec ms | cmd thread ms | GPU busy | draws (med) | Mpx (med) | n |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `a60` | 60 | `78C0D8AC…` | 34.06 | 28.82 | 33.60 | 36.23 | 13.81 | 24.31 | 51.4 % | 1,865.0 | 914.4 | 233 |
| `b120` | 120 | `1E895554…` | 37.17 | 26.67 | 28.26 | 32.94 | 11.94 | 23.85 | 47.2 % | 1,866.0 | 871.9 | 240 |
| `c60` | 60 | `78C0D8AC…` | 36.50 | 27.25 | 28.49 | 30.84 | 11.92 | 23.85 | 46.5 % | 1,866.0 | 871.9 | 224 |

Each arm's `unique fps` is recomputed from the log's own frame-stat series (total frames ÷ total time)
and agrees with the runner's verdict to within 0.05 fps.

**Local noise.** The two 60-mode arms are the same configuration, so their difference is this workload's
run-to-run spread: **34.06 against 36.50 fps = 7.18 %** (their windows are also not at the same pixel
load — 914.4 against 871.9 Mpx median).

| comparison | 60-mode | 120-mode | change | vs local noise |
|---|---|---|---|---|
| full windows | 35.28 (mean of 2) | 37.17 (1) | +5.4 % | 0.75 × |
| shared Mpx range 882.0–957.2 | 34.57 | 34.78 | +0.6 % | 0.08 × |
| pooled median ±5 % (828.3–915.5) | 35.69 | 37.40 | +4.8 % | 0.67 × |
| pooled median ±10 % (784.7–959.1) | 35.41 | 37.21 | +5.1 % | 0.71 × |

Samples surviving each cut, per arm:

| cut | a60 | b120 | c60 |
|---|---|---|---|
| shared Mpx range | 225 of 233 | **30 of 240** | **26 of 224** |
| pooled ±5 % | 155 | 229 | 216 |
| pooled ±10 % | 225 | 239 | 222 |

**Best-matched pair.** `c60` and `b120` came out matched as closely as this harness produces them —
identical median Mpx (871.9), draws (1,866), GPU executing (11.92 ms) and command thread (23.85 ms) —
with fps 36.50 against 37.17, i.e. **+1.8 %**.

**Read that as the below-ceiling row it is.** A scene whose frame takes 27 ms cannot be held back by a
two-vblank wait it never reaches, so this band was never able to show the feature either way. The first
pair's +11 % (and this document's earlier framing of it) came from a matched cut that retained only 36
of the 120-mode arm's 207 samples; the cuts here swing from +0.6 % to +4.8 % on the same data depending
on the band, which is the same fragility.

## 6. Failures and limits

- **One arm-run was lost** before gameplay to the pre-existing `address_space.cpp:568 Protect:
  Unreachable code!` crash (exit `0x80000003`, 21.9 s in, 26 draws; the ledger records the same crash
  costing other arm-runs). It was the 60-mode arm, so it is not attributable to the mode. The attempt is
  kept as `_out/morpheus-ab2-2026-11-19/a60-attempt1-crash/`; the retry is `a60`.
- **No headset.** Unique rendered frames as the emulator reports them; nothing here about latency,
  comfort, the compositor, or whether a session behaves differently. The calibration row measures the
  same ceiling the session measurement did, which is why it is the strongest headless statement
  available — not a substitute for a session.
- The diagnostics cost frame time (command thread 23.9–24.3 ms against 26.7–28.8 ms frames), uniformly
  across arms. None of these numbers is a gameplay frame rate to quote as such.
- Percentiles are of 0.5 s window-average frame times, not individual frames, as
  `tools/benchmark/validator.py` defines them; the runner used here writes no such block, so they are
  computed from the log with the validator's own percentile definition.
- The scene identity of each band is by its draws/Mpx signature, not by an image check.

## 7. Reproducing it

```powershell
# the slot is machine-wide and shared with other workstreams
bash tools/headless-vr/gpu-lock.sh acquire --owner <you> --kind headless --ttl 3600 --require-quiet

# one arm per invocation: sets the mode through the control, records the effective configuration,
# runs the M2 gameplay cell
pwsh -NoProfile -File _out\morpheus-ab2-2026-11-19\run-arm.ps1 -Arm a60  -Mode 60
pwsh -NoProfile -File _out\morpheus-ab2-2026-11-19\run-arm.ps1 -Arm b120 -Mode 120
pwsh -NoProfile -File _out\morpheus-ab2-2026-11-19\run-arm.ps1 -Arm c60  -Mode 60

pwsh -NoProfile -File _out\morpheus-ab2-2026-11-19\analyze-ab2.ps1

bash tools/headless-vr/gpu-lock.sh release --token <token>
```

`run-arm.ps1` is the whole fixed environment in one place. It refuses any mode but 60 or 120, and it
records the engine.ini hash before and after. To get §1's three scene rows, group each log's frame-stat
series by its draws/Mpx signature (the bands in §1) — `analyze-ab2.ps1` reports the gameplay window;
the calibration and backdrop rows are the same series before the anchor.

## 8. Files

| what | where |
|---|---|
| this batch | `_out/morpheus-ab2-2026-11-19/` — `run-arm.ps1`, `analyze-ab2.ps1`, `a60/`, `b120/`, `c60/` (each with `run.json` and `shad_log.txt`), `<arm>.mode.json` (mode, control output, engine.ini hash and text, hashes of binary/runner/eboot, diagnostics), `a60-attempt1-crash/` |
| the earlier pair | `_out/morpheus-ab-2026-11-19/` — `AB.json`, `analyze-ab.ps1`, `arm60/`, `arm120/`, `shadow/` |
| the benchmark campaign's 120-configuration cells | `_out/gpu01-depthpass-2026-11-19/arm0-b1/`, `arm0-b2/` |
| what the settings are | `docs/psvr-morpheus-ue4-settings.md` |
| the scenario's own record | `docs/performance/target-ledger.md` §4.5–4.6 (the ledger owns the benchmark cells; this document does not modify it) |
