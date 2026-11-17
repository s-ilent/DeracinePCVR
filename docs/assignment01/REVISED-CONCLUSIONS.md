# Revised conclusions after the live headset trial (2026-11-17 23:07–23:19)

> **18 Nov addendum — the LUT investigation supersedes two statements below.**
> The classroom is **warm/sepia**, never blue: the diagnostic PNG dump writes R and B swapped, so
> the "normal blue" classroom images in this document are a dump defect, not the game. The
> red-patch comparison was redone with the channel order corrected; the recycler result, the full
> 32³ LUT dump and the remaining hypothesis are in the addendum at the end.

This supersedes the GPU-instability conclusion in `EVIDENCE.md` and raises the severity of the
recycler verdict in `RECYCLER-REVIEW.md`. It is written for an independent reviewer: every claim is
labelled **measured**, **user-reported**, or **hypothesis**.

## What was run

- Binary: `rec3-A02FB23F.exe`, SHA-256
  `A02FB23F7FC15B81B0F2C081DBBD9F9851F9CC3BD1D9A5D4B7761859BF50B1CB`, built from the
  `investigation/cross-fork-renderer` tree; the window title reports `de9c78c-dirty`.
- This build is `de9c78c` + the per-second counters + the covered-refresh skip (off) + **the image
  recycler `6170df6`, default on** — the *original* recycler, without the standalone candidate's
  later pNext guard.
- Launched from a private copy of `pc-vr` at
  `M:\Development\Deracine-work\_run\pc-vr-recycler-try` (installed runtime, launcher, settings and
  user data untouched). Game: `D:\Temp\Deracine\CUSA11919-1.01\eboot.bin`; its `engine.ini` was
  **not** rewritten (the trial settings copy comments out `render_percent`; the file already had
  `r.ScreenPercentage=300`).
- Trial settings: `fps=120`, `resolution=2880`, `present=auto`, **`msaa=4`**, `swap_hands=1`,
  settings window skipped (`-NoMenu`).
- Session log: `M:\Development\Deracine-work\_run\pc-vr-recycler-try\user\log\shad_log.txt`
  (31,060,587 B). No `SHADPS4_FRAME_STATS` / `SHADPS4_DBG_OVERLAP` was enabled, so there are **no
  per-window unique-FPS rows and no recycler counters** in this run.

## What was observed

1. **Visual corruption — user-reported and now captured.** The user's screenshot
   (`_run\pc-vr-recycler-try\evidence\red-overlay-2026-11-17.jpg`, 1920×1080, SHA-256
   `7E10F1F9BE1B9715785D46B141AB695F98BE48D3627986D5752F8BCC44236206`) shows a first-person
   hallway/gallery scene — cat, easel painting, fog, geometry and perspective all **correct** —
   with large, soft-edged **red regions layered over walls, ceiling and furniture**. The red is
   partially transparent (the scene shows through) and follows surfaces; it is not a flat clear
   colour, not a swapped render target, and not a missing image.
2. **Interpretation (hypothesis, not proven):** the frame is structurally right and one blended
   effect/layer is sampling the wrong image contents or the wrong format. That is the failure class
   a recycled image would produce if a view/descriptor or a format assumption still points at an
   image whose contents now belong to another owner. It does **not** look like a presenter,
   swapchain or whole-frame failure.
3. **Performance — user-reported:** "pretty bad, gets worse beyond the title menu." The screenshot
   is beyond the menu (gameplay hallway), so this is the first direct evidence at the user's real
   settings.
4. **Performance — measured from the log** (headset report lines, which are the *presented*
   cadence, not the unique-render metric): the title delivered, in successive reports, 68.4,
   120.0, 103.8, … 48.3, 46.4, 44.7, 77.3, 109.6, 75.7 frames a second, with longest single-frame
   waits up to 122 ms and presentation at 120 Hz. The session therefore fluctuates between roughly
   45 and 120 delivered fps; the 44–48 fps phase and the 122 ms wait are the "beyond the menu"
   region the user describes.
5. **No crash and no device-loss line** in this session; it ended when the user closed it (log ends
   with normal VMM traffic). This is *not* consistent with a hardware fault.

## Corrected conclusions

### 1. The earlier "GPU/device-loss instability" conclusion is withdrawn

- **Fact:** during the RenderDoc attempts I stopped `vrserver`/`vrmonitor`/`vrcompositor` and
  restarted SteamVR with `vrstartup`. I was **not authorized** to do that, and it is my action, not
  a hardware condition.
- **Revised:** the session failures that followed that restart cannot be attributed to the GPU or
  driver. The "hardware instability" wording in `EVIDENCE.md` and the blocked-goal reason is
  withdrawn. There is still **no fresh classroom `.rdc`**; the honest status is "capture not
  achieved", and the attempt must be made in a stable session that I do not restart.
- I will not start, stop or restart SteamVR again.

### 2. The recycler (`6170df6`) default-on is now suspect and must not be integrated

- **Hypothesis (leading):** the red comes from reusing Vulkan image objects. The trial ran the
  original `6170df6`, which has no guard for a non-null `pNext` in the pooled create info, no
  allocator-identity check, no unit test, and relies on the deferred-destruction ordering for GPU
  completion. The screenshot's corruption — a correct frame with one soft, blended red layer — is
  consistent with a stale image being consumed as a mask/light/effect texture, not with a
  whole-frame failure. The standalone candidate `4d244d1` hardens one of these (pNext), but it is
  unbuilt and untested.
- **Not proven:** the red may instead come from the other `de9c78c` renderer work (depth pass,
  in-pass barrier) or from this specific build. The only component the trial adds over the user's
  current `de9c78c`-line runtime is the recycler (plus counters and an off-by-default coverage
  skip), which is why it is the leading suspect, not a verdict. Attribution requires one binary,
  one scene, `SHADPS4_DBG_IMAGE_RECYCLER=0` vs `1`, with a render-target dump in each arm.
- **Revised verdict:** "design plausible, not verified" becomes **"not safe to enable by default;
  a visual-correctness failure has been observed in a headset gameplay session. Do not integrate
  or recommend it until an off/on A/B on the same binary and the same scene isolates it and the
  dumps match."** The 4.15 → 0.29 ms figure remains unverified (the original off arm was the
  calibration screen), and the 58.7 fps classroom/menu cells are not gameplay.

### 3. The 58.7 FPS figure does not describe this trial

The trial used **MSAA 4** and **`present=auto` → 3036×3036 per eye**, 6072×3036 swapchain; the
58.7 FPS crossfork cells and the 39.83 FPS guardrails run both used **MSAA 2** and
**`SHADPS4_VR_SCALE=0.5` (2880×1632)**. They are different workloads. The log's 44–110 delivered
fps is the first direct evidence of the *actual* VR settings, and it is not a controlled
measurement (present cadence, no frame-stats series).

### 4. Assignment 01 status is otherwise unchanged

- Valid classroom benchmark still `classroom-valid-r9` (39.83 unique FPS, screenshot 1.0000,
  MSAA 2 / scale 0.5) — one qualified cell.
- Camera clock remains **experimental**; autopress remains **default-off**; the contract/tools and
  the two candidates are as committed (`feature/…` @ 768b6ce, `candidate/guardrails-2026-11-17` @
  2ccc12a, `candidate/image-recycler-2026-11-17` @ 4d244d1).
- The RenderDoc capture gate remains **open**, now without the false hardware explanation.

## Proposed next experiments (not performed; awaiting decision)

1. **Isolate the red:** same binary (`A02FB23F`), same trial settings, `SHADPS4_DBG_IMAGE_RECYCLER=0`
   then `=1`, each with `SHADPS4_FRAME_STATS=1` and a render-target dump at the same scene moment;
   compare the dumps and the headset view. Do not restart SteamVR.
2. **Separate renderer from recycler:** run the guardrails candidate build (no recycler, no depth
   pass/in-pass barrier) in the same trial settings; if the red disappears with the recycler off
   but the guardrails build is much slower, the corruption and the performance are separate
   problems.
3. **Only then** decide whether the recycler is worth reworking (the standalone candidate's
   hardening plus tests), and whether the `de9c78c` renderer work needs its own review.
4. Keep the trial log and the private runtime copy as evidence; no cleanup of either until the
   reviewer has looked.

## Evidence index

| what | where |
|---|---|
| trial log (31 MB, no frame-stats) | `_run\pc-vr-recycler-try\user\log\shad_log.txt` |
| trial settings | `_run\pc-vr-recycler-try\settings-try.txt` |
| binary hash | `A02FB23F7FC15B81B0F2C081DBBD9F9851F9CC3BD1D9A5D4B7761859BF50B1CB` |
| recycler review | `docs/assignment01/RECYCLER-REVIEW.md` |
| cross-fork reconciliation | `docs/assignment01/BENCHMARK-RECONCILIATION.md` |
| guardrails evidence | `docs/assignment01/EVIDENCE.md` |

---

# 18 Nov addendum — UE4 32³ colour-grading LUT investigation

## Correction: the classroom is warm, not blue

The diagnostic PNG path (`SHADPS4_DUMP_WIDTH`) writes **R and B swapped**. Swapping them back
produces the warm/sepia classroom the user photographed. The "normal blue" classroom in this
document and the blue benchmark reference image (`reference/scene-classroom-menu.png`) are both
colour-swapped by the same dump path. This is an **evidence defect in Assignment 01**, separate from
the LUT: the benchmark's screenshot gate compares two equally swapped images, so it still passes,
but the reference is not the game's colour.

## The 32³ resource

| item | measured |
|---|---|
| guest address / size | `0x2008f80000`, `0x20000` (128 KiB), tile mode 19 |
| format | `A2B10G10R10UnormPack32` |
| producer | colour-target pass, `32×32×1`, **32 layers** (2D array), `vtx=0x4496c84956245785`, 4 indices, MRT 1 |
| consumer | **fragment** shader `0x29090b93e88b0b98`, sampled 3D volume. SPIR-V disassembly (cached `pc-vr/user/cache/CUSA11919/0x29090b93e88b0b98_0.spv`) confirms a tonemap/colour-grading pass: **ST 2084 PQ constants** (0.159301758, 18.6875, 0.8359375, 18.8515625, 78.84375, 10000) and the 32³ texel-centre coordinate `Fma(31/32, c, 0.5/32)` into the 3D image at binding 6 |
| cycle | 2D array (32 layers) → `ExpandImage` → 3D `32×32×32` → sampled; next frame 3D → 2D again. **Two re-creations and two copies per frame** |
| guest memory | all zeros at dump time; the LUT lives only in the Vulkan images and survives through the copy chain |

This **is** the UE4-style 32³ colour-grading LUT: the consumer shader's SPIR-V contains the PQ
tonemap constants and the standard 32³ LUT coordinate scale/bias (31/32, 0.5/32). The remaining
question is not the resource's role but the ordering/filtering of the lookup; see the axis test
below.

## Recycler comparison at the classroom (high-resolution, corrected colour)

| | recycler OFF | recycler ON |
|---|---|---|
| final eye image | warm classroom + menu | warm classroom + menu |
| floor region, off vs on | — | mean abs diff **0.58/255**, max 4, **no pixel > 8** |
| LUT slice 0 | `0D50CEDA…` | `0D50CEDA…` |

The red floor patches are byte-identical with the recycler off and on. **The recycler is not the
cause of the classroom patches.**

## Full 32³ LUT dump (diagnostic build)

Diagnostic exe `5552F1D950985DA81E5284D60E2D213FD3CE222A703C28EBAB4AA3CBAE6C6088`, built from
`e676a43` in `wt-lut-diag` with a 3D-slice-aware dump patch:

- 131,072 bytes, `32×32×32`, 32/32 **unique** slices;
- smooth along all axes (adjacent-slice mean abs diff 0.046);
- no zero, truncated, duplicated or copy-reordered slices.

So the 2D-array ↔ 3D-volume copy preserves the LUT data, and the copy chain is not truncating it.

## Remaining, unproven: ordering/sampling, not data or recycling

The volume's output has strong cross-axis terms, and along the grey diagonal R is 0 while G/B stay
high; a correct warm grade would not map every neutral input to cyan. That points at the **axis
order or sampler used when the LUT is sampled** (or at the 2D-array layer order being interpreted as
a different 3D axis), not at the LUT contents or the recycler. It is also still possible that the
patches are the game's intended grade and the "corruption" is the strong shadow tint.

Next experiments, in order:

1. Identify consumer shader `0x29090b93e88b0b98` (dump/disassemble) and confirm it is the UE4
   tonemapping/colour-grading pass, not a volumetric/fog lookup.
2. Env-gated axis diagnostic (`SHADPS4_DBG_LUT_AXIS`) at the 2D↔3D copy, default off, to test
   whether reversing/permuting the layer→slice order removes the patches in the headset.
3. Only then the smallest correct fix plus a regression test for 3D↔2D-array aliasing and image
   recycling. No unverified optimisation is enabled by default.

## Evidence (18 Nov)

- corrected classroom: `_run\pc-vr-recycler-try\evidence\classroom-on-swapped.png`,
  `classroom-off-swapped.png`
- high-res floor crops: `floor-on-hires.png`, `floor-off-hires.png`, `floor-diff.png`
- full LUT dump: `_run\pc-vr-recycler-try\user\rt_dump-lut3d-off-001815\capture_01\image_2008f80000_level0_32x32x32_A2B10G10R10UnormPack32.bin`
- diagnostic build: `wt-lut-diag` (branch `diag/lut-dump`, dump patch; not a production change)

