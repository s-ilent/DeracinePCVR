# Cross-fork classroom benchmark reconciliation

Two numbers were reported for a nominally similar Déraciné classroom scene:

| | guardrails branch (this work) | cross-fork renderer branch |
|---|---|---|
| reported | **39.83 unique FPS, GPU 97–100 % busy** | **58.7 FPS p50, GPU 51–59 % busy** |
| run | `classroom-valid-r9` | `xf-a2-1`, `xf-a2-1-r3`, `xf-diag2` |
| report | `_out/runs/classroom-valid-r9/run.json` | `wt-crossfork/docs/research/cross-fork-renderer.md` §8; `_build/bench/runs/xf-*` |

**Verdict: they are not a controlled comparison.** They measure two different renderers on
different bases, with different measurement windows, and only one of them has screenshot proof of
the scene. The gap is explained by renderer work present in `de9c78c` and absent from `acda49e`,
not by the image recycler and not by one title configuration being "faster".

## Provenance diff

| item | guardrails r9 | cross-fork xf-a2-1 | same? |
|---|---|---|---|
| source base | `acda49e` (`recovery/baseline`, trusted source baseline) | `de9c78c` (`integration/r58-presentsize`, live-runtime line) | **no** |
| branch | `feature/autonomous-guardrails-2026-11-17` | `investigation/cross-fork-renderer` | no |
| executable | `B6830480…`, 72,777,216 B, built from `fcaa02f` (acda49e + camera/autopress/frame-count/RenderDoc) | `A02FB23F…`, 73,098,240 B, built from `de9c78c` + counters + covered-refresh + recycler | **no** |
| **depth-write representation pass** | **absent** (acda49e has no `CopyColorToDepth32`) | **default on** (`fc2fb55`; `CopyColorToDepth32` present) | **no** |
| **in-pass barrier** | **absent** (no `IsAttachmentImage` in `vk_scheduler.h`) | **default on** (`33ebbf8`; `IsAttachmentImage` present) | **no** |
| presentation / pacing commits | absent | `632306a`, `89f13a0`, `ad7238d` present | no |
| recycler | absent | **on** (`SHADPS4_DBG_IMAGE_RECYCLER` unset = on) | **no** |
| guest render | 300 %, MSAA 2, 2880×3240/eye, packed stereo 5760×3240 | same | yes |
| OpenXR presentation | `SHADPS4_VR_SCALE=0.5`, log: 2880×1632 window, 1440×1632/eye, base eye 2880×3240, scale 0.50 | same setting; `de9c78c` also carries the presentation-size merge, but the env is the scale path | effectively yes |
| vblank | 60 requested and written, read back 60 | `-Vblank 60` (shared harness) | yes |
| MSAA actual | requested 2 (not independently logged in either cell) | requested 2 (quality workstream logged 2× in other runs) | unproven in both |
| autopress | `SHADPS4_DBG_AUTOPRESS=1`, 45 s bound | `SHADPS4_DBG_AUTOPRESS=1` + `SHADPS4_DBG_AUTOPRESS_TO=150000` | **no** |
| warmup / window | 10 s / 40 s, 80 × 0.5 s samples | 10 s / 30 s, 60 × 0.5 s samples | **no** |
| measured window | 40.949 s (Σ frames/fps) | 30 s nominal, 60 samples | no |
| unique FPS definition | Σ frames / Σ(frames/fps) | frame-stats window means (`effective_fps`; the shared harness's p50) | no |
| in-window draws / Mpx | 534.3 draws, 535.0 Mpx | 535.1 draws, 499.7 Mpx (meta mean) | draws yes, Mpx ~7 % |
| passes / frame | **88.0** (window samples 75–94.6) | **66.0** (63 loaded) | **no — +33 % on r9** |
| GPU busy in window | 97–100 % (mean 99.8) | 51–59 % | **no** |
| scene proof | screenshot, similarity 1.0000 to the reviewed classroom+menu frame | draw/Mpx gate only in these cells; the capture A/B was partial and did not capture the classroom with the recycler | **no — r9 stronger** |

## Per-frame work: the numbers line up with the renderer diff, not with the recycler

Both runs do the same guest work: ~535 draws, 19 dispatches, ~0.5 Gpx of render-target traffic,
one 5760×3240 R8G8B8A8 eye picture. What differs is what the emulator does around it:

1. **Passes/frame 88 vs 66.** `33ebbf8` is documented as collapsing the front end from 89 to 65
   render passes by recording a barrier for a non-attachment image instead of ending the pass.
   That is the exact signature measured here: acda49e (no barrier) 88, de9c78c (barrier on) 66.
   More passes at the same draws means more render-pass setup/teardown and more GPU
   synchronization per frame.
2. **Depth/storage representation.** acda49e still recreates the 5760×3240 `R32Sfloat` ↔
   `D32SfloatS8Uint` representation with `vkCmdCopyImage`. The depth/storage investigation
   measured that copy at **3.6–4.9 ms** each (two per frame in the heavy phase) versus **0.166 ms**
   for the depth-write pass that `8638928`/`fc2fb55` install and make default in de9c78c. On a
   25 ms frame, removing ~7–10 ms of GPU work is the difference between 40 and ~55–58 fps.
3. **GPU busy 100 % vs 51–59 %.** r9 is GPU-bound on that extra work; the de9c78c cells have
   headroom and sit at the title's own ~60 Hz front-end cap, so they cannot show a frame-rate
   gain from anything CPU-side (including the recycler).
4. **The recycler is not the cause of the cross-fork number.** de9c78c's own no-recycler cell
   (`xf-diag2`, 58.04 FPS) is within noise of the recycler-on cells (58.66 / 58.55). The
   recycler's measured effect is CPU-thread image create/destroy time, not FPS at this cap. Its
   off-arm comparison (4.15 vs 0.29 ms) is **not usable as a saving estimate**: the off arm landed
   on the calibration screen (176 draws / 273 Mpx), a different workload.
5. **Measurement differences.** 60 samples over 30 s versus 80 over 40 s; autopress allowed to
   150 s versus 45 s; window-mean Mpx 500 versus 535 (the animated overlay and the different
   window length). These change what is averaged but not the pass count or GPU busy.
6. **Machine contention is proven to move this scene by ~30 %.** The crossfork report discarded
   one cell at 41.3 fps / GPU 37 % because *this* branch's Ninja build was running. That is a
   direct demonstration that single cells taken across branches at different times are not
   comparable.
7. **The recovery store's controlled copy-vs-depth A/B sharpens the attribution.** On `defpass.exe`
   (the de9c78c line with the depth switch) the classroom measured copy 51.5 / 54.6 fps and
   depth-pass 58.4 / 50.7 fps — the two pairs disagree in sign and the aggregate is +2.8 % inside
   a ~10 % same-binary noise floor. Even the **copy** arm of that de9c78c-line binary runs at
   51.5–54.6 fps, well above acda49e's 39.8. So the depth pass alone does not explain the gap; the
   rest of the renderer generation (in-pass barrier → 66 vs 88 passes, pacing/presentation, and the
   120 Hz camera in that line) is the larger part. The pass count is the one direct, measured
   correlate: 66 with the barrier, 88 without.

## What would settle it

One binary, one scene phase, interleaved cells:

1. Build `acda49e` + the camera/autopress prerequisite + **only** the depth-write pass
   (`8638928` + `fc2fb55`), with `SHADPS4_DBG_DEPTH_PASS=0/1`; run A-B-A-B on the classroom.
2. Then add `33ebbf8` (barrier-in-pass) with its own switch and repeat; separately record
   passes/frame, GPU busy, frame p50 and the same screenshot identity.
3. Keep the recycler out of that A/B; it has its own candidate and its own metric (allocation
   count, create/destroy CPU time, VRAM).

Nothing in this document is a performance comparison of the two branches; it is a provenance
explanation of why the two reported numbers are not comparable. The only defensible cross-fork
statement is structural: **de9c78c already pays for two default-on GPU optimizations that acda49e
lacks, and its classroom cells are title-capped with GPU headroom, while acda49e is GPU-bound.**
