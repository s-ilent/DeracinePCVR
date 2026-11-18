# M3 — final report (Déraciné headless gameplay and renderer validation)

Date: 2026-11-18. Objective: a reliable, headset-free path from emulator launch to the real
classroom gameplay, with trustworthy benchmark evidence and an integration-ready handoff.

## Status

| item | status | evidence |
|---|---|---|
| M1 headless front end | **PASS** | `_build/headless/runs/m1-candidate-r3`, `m1-candidate-r5-r3` |
| M2 gameplay, independent labs | **PASS** (2 labs) | `runs/m2-off-fixed-1`, `runs/m3-portable-2` |
| Recycler OFF/ON pair | **BOTH-REACHED**, no regression | `RECYCLER-PAIR.json`, `runs/recycler-on-1-try2` |
| A depth/barrier configurations | **PASS** | `A-acceptance.json`, `A-pixel/B-content.json` |
| B recycler pixel correctness (R3) | **PASS** | `B-content.json` |
| C clean shutdown (R6) | **FAIL before repair → PASS after** | `C-shutdown.json`, `runs/r6-clean-1-try2` |
| D RenderDoc gameplay capture | **PASS** (capture; thumbnail black, no mirror) | `D-renderdoc.json`, `rdoc/CUSA11919_frame3341.rdc` |
| E 100 %/120 Hz | **VALID after profile repair**, 120 Hz not met | `E-100pct-valid.json`, `runs/e100-profile-1` |
| Integration handoff | prepared; unified build needs its own band | `_build/team/INTEGRATION-HANDOFF-headless.md` |

## Identity

| what | id |
|---|---|
| renderer candidate | `integration/cross-fork-renderer` @ `1a53ceb` (docs-only `1dd4eb4`), exe `7B658961…` |
| M2/headless build (pre-rewrite) | `feature/headless-vr-scene` @ `a21109c`, exe `6B260523…` |
| M2/headless build (rewritten base) | `c566db7` (same tree except removed binaries) |
| R6 repair | `fix/r6-clean-shutdown` @ `bc524dd` (base `c566db7`), exe `d231fa32…` |
| harness (rewritten base) | `feature/headless-vr-harness-m3`: `8ff9827`, `b9261bc`, `b0ada73` (base `ae6781c`) |
| harness (original ancestry, preserved) | `feature/headless-vr-harness` @ `27fd76f` |
| game | CUSA11919, app 01.01, shadow `CUSA11919-qual`, 300 % / 2× MSAA |
| save fixture | slot 1 `MGS_GameData_1` stage `m00_00_10_10`, `C30D3384…F93C`; `MGS_SystemData` `EAB7FE14…8745D`; `sce_sys/param.sfo` `A5081D3F…` |

The game and save are user-owned and are never committed.

## Independent-lab gameplay manifests

Both used the documented recipe: `SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0`,
`SHADPS4_VR_HANDS=synthetic`, `SHADPS4_VR_MOVE_FROM_PAD=1`, `SHADPS4_MOVE_PITCH=0`, AUTOPRESS to
45 s, pose `pad=+0.30,-0.12,-0.5` at 46 s, pointer hand `move1=6,255` at 60/63/66 s; each lab was
created fresh from the template with the slot-1 save.

| lab | gate | eye | draws | Mpx | passes | fps p50 | frames | GPU |
|---|---|---|---|---|---|---|---|---|
| `m2-off-fixed-1` | 64 s | 2880×3240 | 1866.3 | 606.2 | 41.4 | 33.5 | 77 | 36 % |
| `m3-portable-2` | 63 s | 2880×3240 | 1865.8 | 550.2 | 38.4 | 31.8 | 76 | 32.8 % |

Gameplay image (both eyes, spectral hands, girl at desk, menu UI gone):
`runs/m3-portable-2/frames/gameplay-postwindow__011_…png`, sha256 `110DAC1F…`; 100 % reference
`_build/headless/references/100/gameplay-eye.png`, sha256 `39433415…`.

## Accepted and rejected cells

**Accepted**
- M1 front end: 472.7 draws / 485.3 Mpx / 63 passes, 0 worn lines.
- Recycler OFF `m2-off-fixed-1` and ON `recycler-on-1-try2-try2` (1866.7 draws / 587.6 Mpx /
  40.4 passes / 33.75 fps): matched scene, both reached gameplay.
- A base repeats and legacy (both switches off): draws identical (1866.1–1866.2); base Mpx
  556.8–568.9 / 38.7–39.4 passes; legacy 892.4 Mpx / 57.4 passes, GPU 34→51 %; fps delta +3.2
  inside the 5.4 fps base/base floor — no fps claim.
- B: floor OFF/OFF eye SSIM 0.994509; ON 0.996703; nine paired images; no regression.
- C after repair: `runs/r6-clean-1-try2`, exit code **0**, `clean_exit: true`, all teardown
  markers, no assertion, no leaked process.
- D: scene-gated at 64.7 s on 3308 draws / 660.5 Mpx; 756,701,886-byte `.rdc` opens and
  thumbnails (26,905 bytes); thumbnail black because headless has no mirror.
- E revised: `e100-profile-1`, 960×1080 an eye, 1866.2 draws / 69.3 Mpx / 40.5 passes,
  fps p50 46.2 / p90 48.0 / p99 49.15, 79 frames, GPU 12.8 %; image verified.

**Rejected**
- Old C cells that hard-stopped before `SHADPS4_EXIT_AFTER` (harness bug) and the pre-repair
  clean-exit cell that aborted on the ImGui assertion.
- Earlier "recycler ON stuck on the menu" reports: retracted; they were the harness live-input
  bug (a pose-only live script replaced the input script).
- Old E attempts under the 300 %-derived gate: invalid as timed cells (no clean 100 % anchor),
  not performance results.
- Ad hoc manual lab runs outside the documented construction: startup crashes; not reproducible
  evidence.

## R6 repair

**Root cause.** `ImGui::Core::Initialize` registered a `std::at_quick_exit` callback that calls
`SaveIniSettingsToDisk(GetIO().IniFilename)`. On the clean-exit path the presenter (and its Vulkan
`Instance`) is destroyed before `quick_exit`; `Instance::~Instance` calls `ImGui::Core::Shutdown`
→ `DestroyContext`. The callback then ran with `GImGui == nullptr` and aborted
(`imgui.cpp:4961`, exit `0xC0000003`). The ordinary exit path never called `Shutdown`, so the bug
was latent.

**Fix (`bc524dd`).** `Shutdown` saves while the context is alive and marks it destroyed; the
quick-exit callback is guarded by that state and by `GetCurrentContext() != nullptr`. Whichever
runs first saves exactly once; the other is a no-op. `Shutdown` is idempotent and keeps the
existing waitIdle/TextureManager/Vulkan/SDL/DestroyContext ordering. Normal quick-exit behaviour
(where `Shutdown` never runs) still saves from the callback. The assertion is not suppressed.

**Tests.** New `shadps4_imgui_exit_guard_test` (4 cases: shutdown→quick-exit, quick-exit→shutdown,
repeated quick-exit, no-save) passes. The earlier uncommitted experiment (quick_exit immediately
after the presenter) is superseded; its exact diff is preserved at
`_build/r6/experiment-emulator-cleanexit.patch` (source `a21109c`, sha256 `2DBA6C6C…`).

**Validation.** `runs/r6-clean-1-try2`: attempts 1 crashed at startup (known 0xC0000005), attempt
2 reached gameplay; `SHADPS4_EXIT_AFTER=110`; guest threads paused, GPU/present stopped, presenter
destroyed, "presenter is gone"; **exit code 0**, no assertion, no leaked process, GPU lock
released.

## E — resolution-specific 100 % profile

The 300 % gate cannot classify a 100 % cell: target writes are ~1/9 of the 300 % workload. The
profile keeps scene classification resolution-independent through **draws** (menu ~465, loading
42–55, gameplay ~1866; the 800-draw anchor is far from the menu) and calibrates the **Mpx/pass
band** from the verified 100 % classroom capture: Mpx 55–90, passes 30–55, settle 15 %,
960×1080/eye, 1920×1080 side by side. Menu and gameplay classification remain separate.

Result: 46.2 fps p50 at 100 %, GPU 12.8 % — **120 unique fps was not reached**. Low GPU
utilization does not identify a specific CPU bottleneck; no such claim is made. The earlier
32–49 fps observations are consistent but were not valid timed cells.

## Renderer evidence retained

- A/B tables and raw captures above are kept. The image recycler remains **opt-in**; default is
  off.
- Measured recycler effect on gameplay: images created 4.006→2.013 per frame, create
  0.851→0.019 ms, destroy 2.292→0.008 ms, hits 2.018, misses 2.013, pool 200.4 MB, VRAM
  3270.9→3475.8 MB. **Its fps benefit remains unproven**; no regression was found in the pixel
  comparison.
- The historical headset colour-corruption observation is not invalidated by these headless
  tests. The compared implementations: candidate `1a53ceb`/`7B658961` with recycler off/on in
  these cells; the headset report came from the earlier default-on build before the four
  lifecycle fixes. Do not treat changed draw/pass/Mpx as comparable fps experiments without
  scene equivalence. No new broad LUT investigation was performed.

## Tests and packaging

- New R6 guard tests: 4/4 pass (`wt-r6/build-tests/tests/shadps4_imgui_exit_guard_test.exe`).
- Scripted-input timeline: 6/6 pass (`wt-headless/build-tests/tests/shadps4_scripted_timeline_test.exe`).
- Full `-Tests` tree on this branch still hits the two documented Windows baseline failures
  (usbd `sys/time.h`, memory_patcher `_QUEUE_USER_APC_FLAGS`) and leaves unrelated targets
  NOT_BUILT; both are pre-existing and unrelated.
- Repo (`feature/headless-vr-harness-m3`): runner, inputs, scene gates, profiles, validation
  runner, save manifest, R6 fix patch and docs. Raw saves, game data, `.rdc`, large images and
  generated logs stay outside Git.
- The runner derives its root from its location and accepts `-Root/-SourceLab/-SavedataSource/
  -Shadow/-Eboot`, so a new lab does not need machine paths edited into the script.

## Integration handoff

The unified branch (`integration/deracine-unified-2026-11`, rewritten `cf10f9a5`; worktree
`wt-unified`) has its own M2 (`99085ad`, merged in `647b87b`). It is **not replaced**. Missing M3
functionality to add: the R6 ImGui lifecycle fix, the portable A–E runner and inputs, the
save-slot manifest, the resolution profile, and the two-lab evidence. The unified build reaches
the same classroom at ~1866 draws but a different pixel/pass workload (~860 Mpx / 55.7 passes
vs. the candidate's 550–650 / 38–46), so its benchmark band must be re-derived from a live
unified capture before any comparison. Patches: `_build/handoff/patches/{harness,r6}`.

## Remaining headset-only limitations

No physical-headset latency, comfort, tracking quality, optics/IPD, reprojection quality,
compositor pacing, haptics, audio, display-power or thermals can be validated headlessly. The
desktop presentation is a test-only destination; the guest still executes its normal GNM/Vulkan
command stream, caches, depth/stencil and allocations. The red-colour observation needs a
headset session to confirm its user-visible effect, even after the headless correctness tests.
