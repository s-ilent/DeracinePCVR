# Performance objective — trustworthy baseline and next experiment (2026-11-18)

## Objective

**120 unique rendered FPS** at approximately **3036×3036 per eye** on Steam Frame, with correct
stereo, colour, depth, transparency and frame pacing. Milestones: 60 and 90 unique FPS. Presented /
reprojected frames do not count. Quality reductions must be explicit, measured options, never
hidden substitutions. No engine FPS cap is assumed. The classroom heavy scene is a valid performance target; 120 must be
measured on a GPU-bound workload with the unique-FPS path, not inferred from a capped/paced scene.

## Measurement path

Use the guardrails validator (`tools/benchmark/`): per-window frame counts, unique FPS =
Σframes / Σ(frames/fps), provenance manifest, screenshot similarity, protected-path preflight and
postflight. Do not compare cells across builds, resolutions, MSAA, settings or scene phases.

## Trustworthy measurements (as recorded)

| cell | build | workload | unique FPS | GPU busy | notes |
|---|---|---|---|---|---|
| `classroom-valid-r9` | `B6830480…` (`acda49e` + guardrails) | classroom+menu, 300 %, MSAA 2, presentation `SHADPS4_VR_SCALE=0.5` (2880×1632 window) | **39.83** | 97–100 % | one cell, screenshot 1.0000, 88 passes, 535 Mpx; reference PNG is R/B-swapped |
| `xf-a2-*` | `A02FB23F…` (`de9c78c` + recycler) | same scene and settings | 58.7 p50 (window means, not the validated unique metric) | 51–59 % | **not GPU-bound**; 60 Hz vblank plus the guest submit/vblank cadence measured in the historical investigation (one frame per two refreshes). Not an engine cap. Renderer differs (66 passes, depth pass, in-pass barrier) |
| `xf` 400 % uncapped | `A02FB23F…` | GPU-bound heavy samples, present = guest 7680×4320 | 26.4 → 35.5 → 40.9 | 95 %+ | depth-write pass +34 %, runtime-size presentation +15 % (reported by the cross-fork work, not yet independently re-validated) |

The target configuration — 300 % guest, authored MSAA, **3036×3036/eye OpenXR output** on the
classroom heavy scene — **has not yet been measured with the unique-FPS path**. The 3036×3036 trial
produced headset-presented cadence only (45–50 FPS, GPU 95–100 %), which is not a valid unique-FPS
result. The 3036×3036 trial produced
headset-presented cadence only (45–50 FPS, GPU 95–100 %), which is not a valid unique-FPS result.

## Noise floor

Not established. `classroom-valid-r9` is a single cell; the cross-fork runs show 52.4–58.7 FPS
spread at the cap. A same-binary, same-scene baseline repeated three times is required before any
A/B claim.

## Established bottlenecks (evidence, not predictions)

1. **Depth/storage representation transition**: the guest `0x2068000000` 5760×3240 surface is used
   as `R32_SFLOAT` and `D32_SFLOAT_S8_UINT`; the cache re-creates it ~2×/frame and copies 74.65 MB
   per operation. Measured `vkCmdCopyImage` **3.6–4.9 ms** per operation (~2/frame) vs **0.166 ms**
   for the depth-write pass. The commit `b6b876f` is **not yet independently confirmed**; the
   earlier +34 % was measured by another workstream.
2. **Pass/attachment traffic**: 66–89 passes/frame, 63 loading their targets, 462–572 Mpx/frame at
   300 %/2×MSAA. Reducing genuine passes/barriers is a structural win; its FPS effect is hidden at
   the 60 cap.
3. **Authored 2× MSAA** and 300 % guest render: ~12 ms/frame of GPU at 300 % versus 8.3 ms for
   120 FPS; closing that needs a measured quality decision or broad GPU work, not a hidden scale.
4. **32³ LUT alias** re-creates/copies twice per frame (0.25 MB/frame, 118 image expansions/s). The
   cross-fork report judged it not a cost centre; it is also an open correctness risk (see
   `LUT-CLOSEOUT.md`). Do not optimize it by changing the LUT contents.

Quarantined: image recycler `6170df6` (disabled for performance testing), depth-preservation skips,
illegal storage views, speculative resolution/FPS extrapolations.

## Next experiment (highest impact, smallest correct change)

**Same-binary interleaved A/B of the depth/storage representation pass and the in-pass barrier** on
the real target workload:

- binary: a clean rebuild from the chosen base with both features env-gated
  (`SHADPS4_DBG_DEPTH_PASS=0/1`, plus an explicit barrier gate if absent), recycler forcibly off;
- scene: the GPU-bound classroom heavy scene (reachable with autopress), guest 300 %, authored MSAA, OpenXR 3036×3036/eye; gameplay once reachable;
- measurement: unique FPS per window, 3 repeats per arm, interleaved, same binary/scene/settings;
- correctness: depth and colour target dumps compared (`mean abs / 255`), 0 VUIDs, visual gate;
- acceptance: the pass is retained only if the same-workload unique FPS improves beyond the noise
  floor and the dumps/VUIDs are clean. Otherwise it is reverted and the next target is selected.

Hardware status: the headset is not available for a session; the A/B is prepared offline and
nothing will be claimed until it is measured on the target workload. Where hardware is unavailable,
safe offline research/implementation continues; no production executable, launcher, shared save or
SteamVR service is touched.


