# Déraciné PCVR — integration matrix (2026-11-18)

Unified branch: `integration/deracine-unified-2026-11`, source commit `9358d7a` (branch tip adds this matrix and the report; documentation only)
Base: `acda49e` (`recovery/baseline`, recovered clean baseline)
Worktree: `M:\Development\Deracine-work\wt-unified`

Disposition vocabulary: **INTEGRATED** / **INTEGRATED WITH MODIFICATIONS** / **ALREADY SUPERSEDED** /
**ALREADY PRESENT** / **EXPERIMENTAL, RETAINED SEPARATELY** / **IN PROGRESS, RETAINED** /
**EVIDENCE ONLY** / **REJECTED** / **BLOCKED**.

## A. Workstream branches

| # | Branch | Worktree | HEAD | Disposition | Where / why |
|---|---|---|---|---|---|
| 1 | `recovery/baseline` | recovery store `worktrees/baseline` | `acda49e` | **BASE** | Chosen base: clean, builds from a clean checkout, no untracked `renderdoc_app.h`, no mixed-WIP commit. |
| 2 | `integration/upstream-sync-2026-11` (+ `group/A`, `group/B`, `group/C`, same SHA) | `wt-sync` | `24b8620` | **INTEGRATED** | Merge `e1a1dc2`: PRs #5453, #5495, #5599, #5433, #5444, the subresource re-fetch hazard, the mip-chain view clamp, and tests. |
| 3 | `integration/upstream-texture-staging` | `wt-upstream-stage` | `fa38e35` | **INTEGRATED WITH MODIFICATIONS** | Its nine port commits are content-identical to row 2 (verified by patch-id for the shared PRs). Two things the plain port lacks were added: the **coverage guard** (row 16: coverage derives from `carries_contents`) and the **multisampled guard** (`984849e`: `carries_contents = SafeToDownload() \|\| num_samples > 1`, `GpuModified` from the same decision). Mip/coverage tests present. Its docs/research are not duplicated (they exist on its branch). |
| 4 | `candidate/guardrails-2026-11-17` | `wt-guardrails-candidate` | `2ccc12a` | **ALREADY SUPERSEDED** | Ancestor of `5de0c71` (row 5). |
| 5 | `feature/autonomous-guardrails-2026-11-17` | `wt-autonomous-guardrails` | `5de0c71` | **INTEGRATED** | Merge `59ccc93` / `c6cc31e`: AUTONOMOUS_ENGINEERING.md contract, benchmark harness + validity gate, unique-rendered-FPS aggregation, frame-count logging, env-gated RenderDoc trigger, default-off autopress, virtual camera clock (labelled EXPERIMENTAL), tests. |
| 6 | `fix/deracine-version-compat` | `wt-compat` | `6226ca7` | **INTEGRATED WITH MODIFICATIONS** | Its tip sits on the contaminated runtime lineage (633d212/8638928/fc2fb55/…). Only its two self-contained commits were cherry-picked: `1f5aaff`→`35fc0e2`, `6226ca7`→`71baaf1`. No runtime-lineage commit merged. Launcher now reports title/version and real patch state; Astro Bot 1.00/1.04 warning removed; SNDX pinned to `55 48 89 e5 41 56 53 48 89 fb` with fail-closed Expected-byte verification; 1.00 and 1.01 both accepted. |
| 7 | `fix/color-double-encode` | `wt-color` | `e4bf95d` | **INTEGRATED** | `2b1209b`: colour contract; HMD pass-through, window no longer double-encoded; 7 ColorContract tests. |
| 8 | `fix/hmd-eye-frustum-from-layout` | `wt-fovfix` | `3e2c83e` | **INTEGRATED** | `6112917`: eye frustum recovered from the reprojection layout. |
| 9 | `feature/openxr-native-resolution` | `wt-projection` | `51772ed` | **INTEGRATED** | `5dfaf94`: presentation size planned from `xrEnumerateViewConfigurationViews` (`presentation_size.h`, `SHADPS4_VR_RESOLUTION`, `SHADPS4_VR_SUPERSAMPLE`) + presentation-size tests. |
| 10 | `investigation/psvr-projection-geometry` | (no worktree) | `9d88be8` | **INTEGRATED WITH MODIFICATIONS** | Its production `hmd_reprojection.cpp` is byte-identical to row 8 (`3e2c83e`), so no second implementation. Its unique artefacts are restored in `9358d7a`: `vr_projection_math.h` (blob `9e24b90b`), `test_vr_projection_math.cpp` (blob `1b9afdd1`, 1031 lines), `docs/vr-projection-geometry.md` (589-line `9d88be8` revision). The project decision is that this suite stays in the tree. |
| 11 | `investigation/render-quality` | `wt-quality` | `23e4411` | **INTEGRATED** | `ea90be6` (`36bad1e` env-gated diagnostics) and `d8155d7` (`23e4411` optional 3×3 area downsample). Downsample is **off** by default. |
| 12 | `feature/unreal-inspector` | `wt-unreal` | `b2053b1` | **ALREADY SUPERSEDED** | `git cherry` shows its Unreal commit patch-id-identical to `85a21cb` (in row 15); residual tree diff is only acda49e's own backing-write refactor. Zero unique source. |
| 13 | `integration/unreal-inspector` | `wt-integrated` | `5e46d2b` | **ALREADY SUPERSEDED** | Its 3 Unreal commits are patch-id/`git cherry`-identical to row 15; its 8 core commits are the audit/stage1 fixes already rebased into `acda49e` (`-` in `git cherry`). Zero unique Unreal/hand source. |
| 14 | `investigation/hand-rig` | `wt-handrig` | `65b06b2` | **ALREADY SUPERSEDED** | Restricted Unreal/hand/tools diff against `fda5169` is **empty**; only base churn (old `d7cb924` base). |
| 15 | `investigation/hand-rig-live` | `wt-handrig-live` | `fda5169` | **INTEGRATED** | Merge `d075091` / `73685be`: read-only Unreal inspector/devtools, memory snapshot + report export, hand-rig investigation, OpenXR hand-joint retargeter, live hand discovery. 41 new tests pass. Inspector is button-triggered and guest-memory read-only; retargeter constructed only by tests. |
| 16 | `integration/cross-fork-renderer` | `wt-crossfork-int` | `1dd4eb4` (advanced to `f6510f1` during the session by another agent) | **INTEGRATED WITH MODIFICATIONS** | Its diagnostics + coverage commits were extracted onto the unified base in `integration/unified-crossfork-diag` (`f342a67`, `61e8288`, `75bf2aa`, `c7c8bf7`, `d81283b`, `a2fdd41`) and merged as `c5bc3f8`: RENDERSTAT counters, allocator memory, `CoveringCopyOf` upload skip (off), covered-skip assertion, ImageCoverage tests (19). The recycler is **not** in the branch. |
| 17 | `investigation/cross-fork-renderer` | `wt-crossfork` | `e676a43` | **ALREADY SUPERSEDED** | Investigation lineage behind row 16; no separate code to take. Its two untracked research docs remain in the worktree (documentation, not code). |
| 18 | `candidate/image-recycler-2026-11-17` | `wt-recycler-candidate` | `4d244d1` | **EXPERIMENTAL, RETAINED** | Not integrated. Suspected gameplay colour corruption unresolved; benefit unverified; Review R3 (loadOp=eLoad) still open. Must stay opt-in and off. |
| 19 | `recovery/depth-pass` | recovery store | `b6b876f` | **INTEGRATED WITH MODIFICATIONS** | Cherry-picked as `56a6f8a`, then made **opt-in** by `58346e9` (`SHADPS4_DBG_DEPTH_PASS=1`; unset = image copy). Salvaged five correctness fixes preserved. |
| 20 | `recovery/renderdoc` | recovery store | `7130e9f` | **ALREADY SUPERSEDED** | The guardrails trigger (`9deb0bb`, in row 5) is the same trigger with the harness around it. |
| 21 | `test/v0.21.0-plus-fixes` | (no worktree) | `517213c` | **ALREADY SUPERSEDED** | Runtime-lineage devtools intermediate (pad automation, menu-walk, barrier). The barrier reached the unified branch via row 23; the rest are dev-only experiments retained on that lineage. |
| 22 | `integration/r58-presentsize` (**live runtime**) | `DeracinePCVR` | `de9c78c` | **DO NOT MERGE — accounted** | The active runtime lineage. Later functionality carried selectively: presentation sizing (`5dfaf94`), launcher `present=` (`6fc3500`), depth pass (`56a6f8a`, now opt-in), barrier (`267d200`, now opt-in). Deliberately **not** carried: `633d212`'s mixed WIP (frame timeline, alias trace, preserve tracker, HMD panel probe, foveation), pad automation / menu-walk / autostick / hand-pose, `7e90afb` foveation-any, `steady=`/`SHADPS4_VR_STEADY_FPS`, `ad7238d` vblank pacing. Those stay on the branch. |
| 23 | `feature/headless-vr-harness` | `wt-harness` | `6417f36` | **IN PROGRESS, RETAINED** | Active work by another agent; sits on the runtime lineage and carries the quarantined recycler commits. Not integrated. |
| 24 | `feature/headless-vr-scene` | `wt-headless` | `94998e0` (advanced to `a21109c` during the session) | **IN PROGRESS, RETAINED** | Same reason as row 23; synthetic Move hands / scripted timeline / timed quit are devtools, not production. |
| 25 | `diag/lut-dump` | `wt-lut-diag` | `ae702aa` | **EXPERIMENTAL, RETAINED** | The LUT investigation is closed without confirmed root cause; no fix is proposed. Diagnostics not integrated. |
| 26 | `merge/eval-2026-11-17` (`633d212`), `merge/eval-upstream-sync`, `-render-quality`, `-openxr-res`, `-color-fix`, `-hmd-frustum` | `wt-merge` (last on `merge2/hmd-frustum`) | `633d212`, `ef80f80`, `f5d829a`, `168e33d`, `0eb91d8`, `d941812` | **EVIDENCE ONLY** | Prior merge-evaluation branches built on `633d212`. Their question ("does each candidate merge and build?") is answered and superseded by the unified branch. |
| 27 | `merge2/upstream-sync`, `merge2/color-fix`, `merge2/hmd-frustum` | `wt-merge` | `2aa2dae`, `970625e`, `92a0041` | **EVIDENCE ONLY** | Round 2 on `fc2fb55`; the conflicts they documented are resolved structurally in the unified branch. |
| 28 | `fix/libc-cxa-hooks` | (no worktree) | `0699f03` | **ALREADY PRESENT** | Its content is in `acda49e` (`224be22` + `8f59dcf`); only unrelated later files differ. |
| 29 | `audit/stage1-host-guest` | (no worktree) | `e21a9e2` | **ALREADY PRESENT** | All eight fixes were rebased into `acda49e` (`9bdf2bf`, `917fad1`, `9075e12`, `0e810ad`, `c8a0d53`, …); `git cherry` marks them `-`. |
| 30 | `main`, `origin/*`, `upstream/main` | `DeracinePCVR` (detached refs) | `d7cb924`, `143b58a`, … | **REFERENCE ONLY** | Pre-integration fork/upstream history. Not moved, not pushed, not merged. |
| 31 | `integration/unified-compat-guardrails` | `wt-unified-compat` | `59ccc93` | **PROCESS BRANCH — MERGED** | Integration branch for rows 5+6; merged as `c6cc31e`. Kept for review/rollback. |
| 32 | `integration/unified-unreal-quality` | `wt-unified-unreal` | `d075091` | **PROCESS BRANCH — MERGED** | Integration branch for row 15; merged as `73685be`. |
| 33 | `integration/unified-presentation-perf` | `wt-unified-present` | `98aae10` | **PROCESS BRANCH — MERGED** | Integration branch for rows 7–11, 19 (depth pass) and 22 (barrier); merged as `9a617a2`. |
| 34 | `integration/unified-crossfork-diag` | `wt-unified-crossfork` | `a2fdd41` | **PROCESS BRANCH — MERGED** | Extraction branch for row 16; merged as `c5bc3f8`. |

## B. Worktrees and uncommitted work (all left untouched)

| Worktree | State at inventory | Disposition |
|---|---|---|
| `DeracinePCVR` (live runtime, `de9c78c`) | `M pc-vr/shadps4.exe` (active binary `E6A5A165…`, 73,055,232 B, 17 Nov 15:59); untracked `docs/AUTONOMOUS.md`, `o`, `pc-vr/render-config.ps1`, `pc-vr/shadps4.exe.before-r58`, `pc-vr/shadps4.v0.21.0-bundled.bak`, `pc-vr/user/config.json.bak` | **Untouched.** Active runtime and user config not modified or deployed. |
| `wt-quality` | 14 files: `patch_pattern.h` + test (new `mask_unique` patch-line family with Expected-byte verification), `memory_patcher.*`, `quality_diag.*` (sampled-image/fmask diagnostics), `scripted_input.*` (Move buttons/trigger), `vr_runtime.cpp`, `test_downsample_area.cpp`, both `AGENTS.md` | **IN PROGRESS, RETAINED.** Belongs to the render-quality agent; uncommitted work was audited read-only and never staged. |
| `wt-perf` (detached `de9c78c`) | `M vk_scheduler.cpp` — adds the frame count to the frame-stats line | **SUPERSEDED** by guardrails `4b93bd8` (same instrumentation, integrated). |
| `wt-lut-bisect` (detached `543f255`) | `M move.cpp`, `M pad.cpp`, untracked `autopress.h` | **EXPERIMENTAL, RETAINED** (LUT bisect; closed). |
| `wt-lut-revert` (detached `acda49e`) | staged `image_info.cpp` (revert of `9bdf2bf`) + same move/pad/autopress edits | **EXPERIMENTAL, RETAINED** (LUT revert; build interrupted, closed). |
| `wt-autonomous-guardrails` | `M docs/assignment01/EVIDENCE.md` | Branch integrated; the dirty doc delta is retained in place. |
| `wt-crossfork` | 2 untracked research docs (`recycler-review.md`, `renderdoc-reproducibility.md`) | Documentation retained in place; same content is on the integration branch. |
| `wt-handrig`, `wt-unreal`, `wt-integrated`, `wt-projection` | `M AGENTS.md`, `M src/AGENTS.md` (English agent-doc rewrite in progress) | **Superseded**: the unified branch already carries the English Deracine AGENTS.md. Files left untouched. |
| `wt-compat`, `wt-harness`, `wt-headless` | Clean; their `.git` link is WSL-path based, so Windows git reports them "prunable" (`git -C` fails, WSL git works). | Branches accounted (rows 6, 23, 24); worktrees not modified. |
| `wt-sync`, `wt-upstream-stage`, `wt-crossfork-int`, `wt-merge`, `wt-color`, `wt-fovfix`, `wt-guardrails-candidate`, `wt-recycler-candidate`, `wt-lut-diag` | Clean | Read-only; their commits are integrated or accounted above. |

## C. Excluded / rejected items (unchanged from the project's own verdicts)

| Item | Disposition |
|---|---|
| `633d212` as a unit (31 files / +1,752 mixed WIP + diagnostics) | **REJECTED as a merge.** Its useful pieces were reconstructed independently: camera clock (`d7c7d5f`), autopress (`b32b88d`), RenderDoc trigger (`9deb0bb`), frame-count logging (`4b93bd8`). The rest (frame timeline, alias trace, preserve tracker, HMD panel probe, foveation) is diagnostics for closed investigations and stays on the runtime lineage. |
| `fc2fb55` (depth pass made default-on, sweeping 10 unrelated files) | **REJECTED as a commit.** The pass itself is integrated as an opt-in (`56a6f8a` + `58346e9`). |
| Image recycler `6170df6` / `2498976` / `4d244d1` | **EXPERIMENTAL, RETAINED.** Zero recycler code in the unified branch. |
| `33ebbf8` barrier-in-pass | **INTEGRATED WITH MODIFICATIONS** (`267d200`), opt-in only (`SHADPS4_DBG_BARRIER_IN_PASS=1`); +2.6 % at 400 % not reproduced in gameplay. |
| `7e90afb` foveation-any (measuring aid) | **REJECTED as a default.** Not integrated. |
| `steady=` / `SHADPS4_VR_STEADY_FPS` / `ad7238d` vblank pacing | **REJECTED.** Not integrated; the governor never acted with the headset asleep. |
| `render_percent=210` preset inference | **REJECTED.** Not integrated. |
| LUT fixes / `SHADPS4_DBG_LUT_AXIS` | **REJECTED.** Investigation closed without a confirmed cause; nothing enabled. |
| `de9c78c` autostick, `b6fd3ee` hand pose, `194958b`/`517213c` pad automation | **REJECTED as production defaults.** Dev-only; retained on the runtime lineage. |
