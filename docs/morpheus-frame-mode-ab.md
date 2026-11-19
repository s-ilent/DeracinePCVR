# Native frame mode, measured: a bounded 60 → 120 → 60 replication

What this is: the replication of the first 60-against-120 comparison of Déraciné's `frame_mode`
(`pc-vr/settings.txt` → the group of Unreal settings that `pc-vr/render-config.ps1` writes). The
first pair suggested a gain; this bounded replication **does not reproduce it**.

Read this together with `docs/psvr-morpheus-ue4-settings.md`, which owns what the settings are and
where they come from. This document owns only the measurement.

**Headline:** two runs of the *same* 60-mode configuration differed by **7.2 %**. The 120-mode arm was
**+5.4 %** over the 60-mode mean on the full windows, **+1.8 %** against the 60-mode arm whose window
had the *same* pixel load, and **+0.6 %** across the pixel range the windows actually share. No effect
is detectable at this sample size.

**Attribution.** `frame_mode=120` is a **group of five settings**, not `PSVRFrameSequence` alone: it
also sets `bSmoothFrameRate=False`, the 22..250 smoothed range, `t.MaxFPS=0` and
`r.FinishCurrentFrame=0`. Nothing below isolates any one of them, so the result — with the first
pair's positive result included — belongs to the configuration as a whole. (The one key measured in
isolation anywhere is `r.FinishCurrentFrame`, historically: 48.7 against 44.0 frames a second in the
cold state, outside this harness and not comparable with these cells.)

## 1. What was held fixed

| | |
|---|---|
| scenario | `deracine-headless-gameplay-300-msaa2-vblank120`, profile `tools/benchmark/reference/scene-gameplay.json` |
| gate / advisory band | `min_draws 800`, `min_mpx 450`, `consecutive_samples 2`; the profile's advisory band is draws 1500–2300, Mpx 550–750, passes 35–55 (the observed cells sit above the advisory Mpx, as the ledger's do) |
| runner | `tools/benchmark/headless-gameplay.ps1`, sha256 `9DC86093D1818848BF72C655E72C2C1828E444A38D2BE49264E6E96A70D60FB5` |
| executable | `_out/gpu01-depthpass-2026-11-19/bin/shadPS4.exe`, sha256 `8991510F22CB8D4EC376DB4CA3458C713E4B5E90780FD994A780812B144E635A` |
| game | private shadow `_out/morpheus-ab-2026-11-19/shadow`, `eboot.bin` sha256 `8DA8E46AC5FDC48E4E9B59E459920A266039993DC13A2D92740F8803E46FDF75` (the same shadow and eboot as the first pair) |
| save | `_build/unified/accept-lab2-save-sfo`, slot 1, stage `m00_00_10_10` |
| guest settings under test | `render_percent=300`, `guest_msaa=2` — identical in every arm |
| presentation | the runner's own defaults, identical in every arm (`SHADPS4_TITLE_EYE_WIDTH=2880`, `SHADPS4_TITLE_RESOLUTION=6`, `SHADPS4_VR_SCALE=0.5`, `SHADPS4_VR_REFRESH_RATE=120`, `SHADPS4_VR_RESOLUTION` unset) |
| diagnostics | `SHADPS4_GPU_TIMING=1`, `SHADPS4_DBG_OVERLAP=1`, `SHADPS4_DBG_DEPTH_PASS=0` — **the M2 cells' set, identical in every arm** |

**Provenance trap, recorded as the ledger requires:** the runner's `run.json` `env` block does **not**
record `SHADPS4_GPU_TIMING`, `SHADPS4_DBG_OVERLAP` or `SHADPS4_DBG_DEPTH_PASS`. They are recorded
per arm in `_out/morpheus-ab2-2026-11-19/<arm>.mode.json` and in the table below, not inferred.

The only thing that differs between arms is the shadow's `gameproject\saved\config\ps4\engine.ini`,
written by `pc-vr/render-config.ps1`. The 60-mode arms' file and the 120-mode arm's file hash to the
**same values as in the first pair**, so the control produced byte-identical configurations across
both batches: `78C0D8AC…` for 60, `1E895554…` for 120.

## 2. The arms

`n` is the number of in-band 0.5 s window samples; frame times are those window averages
(`1000/fps`), percentiles by the validator's own definition; GPU executing and command-thread times
are the medians of the `gpu timeline` line printed with each window.

| arm | mode | engine.ini | valid | unique fps | p50 ms | p90 ms | p99 ms | GPU exec ms | cmd thread ms | GPU busy | draws (med) | Mpx (med) | n |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `a60` | 60 | `78C0D8AC…` | yes | **34.06** | 28.82 | 33.60 | 36.23 | 13.81 | 24.31 | 51.4 % | 1,865.0 | 914.4 | 233 |
| `b120` | 120 | `1E895554…` | yes | **37.17** | 26.67 | 28.26 | 32.94 | 11.94 | 23.85 | 47.2 % | 1,866.0 | 871.9 | 240 |
| `c60` | 60 | `78C0D8AC…` | yes | **36.50** | 27.25 | 28.49 | 30.84 | 11.92 | 23.85 | 46.5 % | 1,866.0 | 871.9 | 224 |

Each arm's `unique fps` here is recomputed from the log's own frame-stat series (total frames ÷ total
time) and agrees with the runner's verdict to within 0.05 fps.

## 3. Local noise, and what the difference is worth

The two **60-mode** arms are the same configuration, so their difference is this workload's local
run-to-run spread: **34.06 against 36.50 fps = 7.18 %** (their windows are also not at the same pixel
load — 914.4 against 871.9 Mpx median).

| comparison | 60-mode | 120-mode | change | vs local noise |
|---|---|---|---|---|
| full windows | 35.28 (mean of 2) | 37.17 (1) | **+5.4 %** | 0.75 × |
| shared Mpx range 882.0–957.2 | 34.57 | 34.78 | **+0.6 %** | 0.08 × |
| pooled median ±5 % (828.3–915.5) | 35.69 | 37.40 | **+4.8 %** | 0.67 × |
| pooled median ±10 % (784.7–959.1) | 35.41 | 37.21 | **+5.1 %** | 0.71 × |

Samples surviving each cut, per arm — the matched figures rest on far fewer of the 120-mode arm's
samples than of a60's:

| cut | a60 | b120 | c60 |
|---|---|---|---|
| shared Mpx range | 225 of 233 | **30 of 240** | **26 of 224** |
| pooled ±5 % | 155 | 229 | 216 |
| pooled ±10 % | 225 | 239 | 222 |

## 4. The cleanest comparison in the batch

`c60` and `b120` came out matched as closely as this harness produces them — identical median Mpx
(**871.9** both), identical median draws (**1,866** both), and identical medians for GPU executing
(**11.92 ms** both) and command thread (**23.85 ms** both) — with fps **36.50 against 37.17**, i.e.
**+1.8 %**. Whatever the difference between the modes is, it is smaller than the difference between
two runs of the same mode.

## 5. What this means

- **The first pair's improvement is not reproduced.** That pair measured 34.39 against 40.76 over its
  windows and 34.58 against 38.38 at a matched load (+11 %). Its two windows were not at the same
  pixel load (946.5 against 871.9 Mpx median), and the band that produced the +11 % retained only
  **36 of the 120-mode arm's 207 samples** — a selected fifth of the run. The matched cuts in this
  replication swing from +0.6 % to +4.8 % depending on the band and how many samples survive, which
  is the same fragility.
- **No effect is detectable here.** Every figure is at or below the 7.2 % local spread, and the
  best-matched pair is +1.8 %.
- **A small effect is not excluded.** This design cannot resolve a real 2–5 % difference against a
  7 % spread; that would need several runs per arm, not one or two. Anyone re-opening this should
  budget for that, and should treat the matched-cut sample counts above as the first thing to check.
- **No fourth arm was run.** The replication's own criterion was to add a second 120-mode arm only if
  the improvement remained plausible; at +5.4 % against a 7.2 % spread and +1.8 % against the
  best-matched 60-mode arm, it did not.
- **The two batches are not comparable with each other** and are not pooled: this batch ran with
  `SHADPS4_GPU_TIMING=1` and `SHADPS4_DBG_OVERLAP=1`, the first pair did not. For scale, the same
  120-mode configuration at the same median Mpx measured 40.76 in the first batch and 37.17 here —
  9.7 % apart, of the lower — so between-batch variation is at least as large as anything being looked
  for.

## 6. Failures and limits

- **One arm-run was lost** before gameplay to the pre-existing
  `address_space.cpp:568 Protect: Unreachable code!` crash (exit `0x80000003`, 21.9 s in, 26 draws;
  the ledger records the same crash costing other arm-runs). It was the 60-mode arm, so it is not
  attributable to the mode. The attempt is kept as `_out/morpheus-ab2-2026-11-19/a60-attempt1-crash/`
  and was retried; the retry is the `a60` above.
- **No headset.** Unique rendered frames as the emulator reports them; nothing about latency,
  comfort, the compositor, or whether a session behaves differently.
- The diagnostics themselves cost frame time (the command-thread medians are 23.9–24.3 ms against
  26.7–28.8 ms frames, so the frame is bounded near the command thread). That is uniform across arms
  but means none of these numbers is a gameplay frame rate to quote as such.
- Percentiles are of **0.5 s window-average** frame times, not of individual frames, as
  `tools/benchmark/validator.py` defines them; the runner used here writes no such block, so they are
  computed from the log with the validator's own percentile definition.

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

`run-arm.ps1` is the whole fixed environment in one place: the shadow, the binary, the save source,
the runner, `render_percent=300`, `guest_msaa=2` and the three diagnostic variables. It refuses to
run unless the mode is 60 or 120, and it records the engine.ini hash before and after.

## 8. Files

| what | where |
|---|---|
| this batch | `_out/morpheus-ab2-2026-11-19/` — `run-arm.ps1`, `analyze-ab2.ps1`, `a60/`, `b120/`, `c60/` (each with `run.json` and `shad_log.txt`), `<arm>.mode.json` (mode, control output, engine.ini hash and text, hashes of binary/runner/eboot, diagnostics), `a60-attempt1-crash/` |
| the first pair | `_out/morpheus-ab-2026-11-19/` — `AB.json`, `analyze-ab.ps1`, `arm60/`, `arm120/`, `shadow/` |
| the benchmark campaign's 120-configuration cells | `_out/gpu01-depthpass-2026-11-19/arm0-b1/`, `arm0-b2/`, and their manifest with the `game_config` hash |
| what the settings are | `docs/psvr-morpheus-ue4-settings.md` |
| the scenario's own record | `docs/performance/target-ledger.md` §4.5–4.6 (the ledger owns the benchmark cells; this document does not modify it) |
