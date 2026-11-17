# LUT investigation — closeout (2026-11-18)

Status: **closed without a confirmed root cause or validated fix**, by decision after the
investigation did not converge. No LUT-related code is enabled by default, and no fix is proposed.

## What was established (measured)

- The 32³ resource is guest `0x2008f80000`, 128 KiB, `A2B10G10R10UnormPack32`, tile 19.
- Producer: a colour-target pass, `32×32×1`, **32 layers** (2D array), `vtx=0x4496c84956245785`,
  4 indices. Consumer: fragment shader `0x29090b93e88b0b98`, which samples a 3D image at binding 6
  with `Fma(31/32, c, 0.5/32)` and carries the ST 2084 PQ constants — a tonemap/colour-grading
  pass. The resource is the UE4-style 32³ colour-grading LUT.
- The cache re-creates and copies between the 2D array and the 3D volume **twice per frame**
  (`ExpandImage` + `CopyImage`), with no persistent dual view.
- A full 32-slice dump of the 3D LUT (diagnostic build) was complete: 32/32 unique slices, smooth,
  no truncation or duplication.
- Recycler OFF vs ON at the classroom: the floor region is identical (mean abs diff 0.58/255,
  max 4, no pixel > 8) and LUT slice 0 is byte-identical. The recycler did not change the LUT data
  in these captures.
- Vulkan core validation was enabled and reported **0 VUID / Validation Error** lines in the LUT
  runs.
- The classroom is warm/sepia; the blue in the diagnostic captures was a **dump bug** (R and B
  written swapped), not the game. The same bug affects the benchmark's reference PNG.
- Snapshots r9 (`acda49e` + guardrails) and the `633d212` merge build are effectively identical in
  the classroom (floor diff 0.47/255, max 1, zero pixels > 8), including the red floor patches.

## What is not established

- The red floor patches were **not** reproduced as a confirmed defect in a controlled A/B, and no
  commit was proven to introduce them. The user states the initial build did not show them; that
  statement is the authoritative regression signal. The earlier claim that the red was "not
  introduced by recent changes" was wrong and has been withdrawn.
- The only renderer change in the `1d2f393` (golden) → `acda49e` window is `9bdf2bf`
  ("match small mips and multi-layer subresources", `ImageInfo::MipOf/SliceOf`). It is a
  **hypothesis**, not a confirmed cause: the clean-side build at `543f255` crashed in a later-fixed
  libc path, and the headset was not available to capture the classroom for a clean/bad comparison.
- Whether the patches are the intended grade, an axis/sampling error, or a subresource-matching
  regression is unresolved. The LUT data itself was intact in the one full dump.

## Approaches that failed or were abandoned

- Colour comparison using the diagnostic PNGs before discovering the R/B swap.
- `/` `SHADPS4_DBG_LUT_AXIS=flip` diagnostic (built, `EB3C16FA…`): flipping the 32-layer order
  inverted the scene colours, which is the expected effect of the probe but did not isolate the
  red patches; the run was stopped by the user and is not a fix.
- Bisect builds of `543f255` and `9bdf2bf` (clean/bad sides): the clean side crashed in a
  later-fixed libc path before reaching the classroom, so the runtime comparison did not complete.
- Asking the user to press buttons was wrong; the investigation should have been driven entirely
  by the agent.

## Preserved evidence

- `_run\pc-vr-recycler-try\evidence\` (43 files, ~50 MB): red screenshot, corrected classroom and
  floor images, LUT contact sheet and 32³ raw dump, shader SPIR-V disassembly, r9/merge snapshots,
  diff scripts, preserved logs.
- `wt-lut-bisect` (543f255 + autopress hook, built; clean side crashed) and `wt-lut-revert`
  (acda49e + autopress − 9bdf2bf, build interrupted) — experimental only, nothing enabled.
- `_run\pc-vr-recycler-try\shadps4-lut-axis-flip.exe` (`EB3C16FA…`) and the flip settings file.

## Quarantine and re-entry

- Image recycler `6170df6` stays **disabled** for performance testing until its correctness is
  independently established.
- No LUT fix, flip, dump patch or diagnostic is enabled by default.
- Reopen only if new evidence directly establishes a cause (e.g., a clean-vs-bad capture that
  isolates `9bdf2bf`, or a reference frame from the initial build showing the patches absent).
- A corrupted frame is never a valid performance result; any performance run must pass the visual
  check.
