# Cross-fork candidate state — validation target for A–E

Owner: validate-eng (shared task `task-3`). Read-only verification, 2026-11-18 00:40–00:50
(Australia/Adelaide). No emulator, build, `ctest` or RenderDoc session was started while producing
this document. Claims below were checked against the worktree, the git objects and the binary — not
against the research docs alone.

## 1. Exact head, and a correction to the brief

| item | value |
|---|---|
| branch | `integration/cross-fork-renderer` (worktree `wt-crossfork-int`) |
| **head (now)** | **`1dd4eb4225f948e0407a0a23d78802eb8de017cd`** — `docs(research): record the LUT-corruption report as a promotion gate`, 2026-11-17 23:50:34 +1030 |
| brief's target | `1a53cebd4c516a51a938ad8dbc3b36d72392343f` — `docs(research): final reconciliation pass, with the exact landing order`, 21:40:53 |
| relation | `1dd4eb4` is **docs-only on top of `1a53ceb`**: 1 file, +9 lines, `docs/research/cross-fork-integration.md` (new promotion gate 6: an independent LUT-corruption report must be resolved). **The source tree at `1dd4eb4` is identical to the source tree at `1a53ceb`.** |
| commits from main | 12 commits on top of main `de9c78cf...` (`integration/r58-presentsize`) |

The ref moved after the brief was written; the binary predates `1dd4eb4` by ~2 h but no source
changed, so the exe is still the right binary for HEAD's source (see §3).

How the refs were read (the worktree's `.git` file says `gitdir: M:/Development/...`, which git
treats as a relative path, so `git -C wt-crossfork-int ...` fails as warned). The worktree's own git
dir is the reliable handle:

```bash
WGD=/mnt/m/Development/Deracine-work/DeracinePCVR/.git/worktrees/wt-crossfork-int
git --git-dir=$WGD --work-tree=/mnt/m/Development/Deracine-work/wt-crossfork-int <cmd>
# head: git --git-dir=$WGD rev-parse refs/heads/integration/cross-fork-renderer  -> 1dd4eb4...
# full log: git --git-dir=$WGD log --format='%h %ci %s' -12
```

The `.git/worktrees/wt-crossfork-int/gitdir` file also carries the stray `M:/` prefix; nothing was
repaired, as instructed.

## 2. Does the worktree match HEAD?

**Yes, in content.** `git status` shows 1516 entries, but they are **line-ending only**:

```bash
git --git-dir=$WGD --work-tree=.../wt-crossfork-int diff --ignore-cr-at-eol --numstat   # empty
git --git-dir=$WGD --work-tree=.../wt-crossfork-int status --porcelain | grep '^??'    # none
```

Blobs in the repository are LF; the checked-out files are CRLF (`file LICENSE` ->
"ASCII text, with CRLF line terminators"). The same artifact exists in the main checkout
(`git -C DeracinePCVR status --porcelain` -> 1502 entries, all CRLF-only, `core.autocrlf=false`
now), so this is a pre-existing machine/checkout artefact, not an edit of the candidate.

Recycler source files are *tracked* at HEAD (the earlier "untracked" reading was an artefact of
using the main repo's git dir instead of the worktree's): `vk_image_recycler.cpp/.h`,
`image_coverage.h`, `image_stats.h`, `test_image_coverage.cpp` are all in `1dd4eb4`'s tree.

## 3. Binary provenance

| item | value |
|---|---|
| path | `wt-crossfork-int/build/shadPS4.exe` |
| sha256 | `7B658961FCE03E367C7CC02FDC278F2B2CB71EB90032944CF76BCD5992A5357F` |
| size | 73,108,480 B |
| mtime | 2026-11-17 21:35:30.658 +1030 |
| build config | `CMAKE_BUILD_TYPE=Release`, Ninja, `clang-cl` from `C:/Tools/llvm-23.1.5`, project `shadPS4 2.0.0`, Vulkan headers from this worktree |
| build log | `_build/logs/int-build-4.log`: 28 targets, `[27/28] Linking CXX executable shadps4.exe`, exit 0 in 0.4 min |
| ninja tail | `.../texture_cache.cpp.obj` then `shadps4.exe` (link), both in the same ninja run |

**Is it newer than its sources?** Newest file under `src/src`: `texture_cache.cpp` at 21:35:03
(< binary 21:35:30). `find src/src -newer build/shadPS4.exe` is empty. The only newer source file is
`src/tests/CMakeLists.txt` (21:36:10), which builds the test targets and does not enter the exe.
The binary is therefore consistent with the `1a53ceb`/`1dd4eb4` source.

The sha256 matches the value the Lead quoted in `_build/team/COORDINATION-crossfork.md`
(`7B658961…`) and is **different** from the old default-ON investigation build
(`A02FB23F…`, `_run/pc-vr-recycler-try/shadps4.exe`, v0.16.1) that another session was running.
No conclusion from that session's log transfers to this binary.

## 4. Test suite status

Already run by the Lead at 21:36; **not re-run** here (CPU-heavy, would pollute another agent's
timing cell, and re-running needs the Lead's OK).

| item | value |
|---|---|
| evidence | `_build/logs/int-ctest2.log`, `wt-crossfork-int/build-tests/`, `Testing/Temporary/LastTest.log` |
| result | `98% tests passed, 10 tests failed out of 590`, 22.0 s |
| failures | 8 × `*_NOT_BUILT` (GCN, USBD, memory_patcher, patch_repository, patch_state, patch_domain, patch_session, preset) + `ReadConstSnapshotDiag.IsolationDisabledAfterTestB` + `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot` — exactly the documented pre-existing Windows set (`_build/SETUP.md`) |
| new tests | 19 `ImageCoverage.*` tests, all Passed (e.g. `EqualImagesCoverEveryLevel`, `ColourIntoDepthCoversNothing`, `MoreDestinationLevelsWithFewerLayersCoverNothingWhole`) |

A fresh `ctest` is available on request but is not needed to trust the binary.

## 5. Recycler switches and counters (source-verified)

### 5.1 Environment flags that matter to this validation

| flag | read in | semantics |
|---|---|---|
| `SHADPS4_DBG_IMAGE_RECYCLER` | `vk_image_recycler.cpp:13-28` | **unset/empty -> off**; `0`/`false`/`off`/`no` -> off; any other value -> on. Off is the default per `0a09829`. The header comment at `vk_image_recycler.h:58` ("Unset means on") is **stale**; the implementation and the commit are authoritative. |
| `SHADPS4_DBG_OVERLAP` | `image_stats.h:17-21` | enables the per-second `RENDERSTAT` line *and* the `ScopedTimer`s. Any value, including `0`, enables it (`getenv != nullptr`). |
| `SHADPS4_DBG_SKIP_COVERED_REFRESH` / `SHADPS4_DBG_COVERED_REFRESH` | `texture_cache.cpp:1003,1016` | A1 upload-skip diagnostic, off by default; not used by A–E. |
| `SHADPS4_FRAME_STATS`, `SHADPS4_FRAME_STATS_EVERY` | `vk_scheduler.cpp` | frame-stats windows the scene gate and FPS are read from. |
| `SHADPS4_DBG_RDOC_CAPTURE`, `SHADPS4_DBG_RDOC_CAPTURE_SCENE` | `vk_scheduler.cpp:39,70` | RenderDoc trigger by seconds and/or by a frame-stats window (`draws,mpx` lower bounds). One-shot, process must be under RenderDoc. |
| `SHADPS4_DBG_DUMP_HMD_FRAMES` | `vk_presenter.cpp:1177` | comma-separated **absolute HMD hand-off frame ids**; dumps all registered GPU images to `<UserDir>/rt_dump-<frame>/capture_NN/`. PNGs capped at `SHADPS4_DUMP_WIDTH`; file names keep the real image size and format. **This is the only HMD dump path in this candidate.** |
| `SHADPS4_DUMP_WIDTH` | `vk_presenter.cpp:1695` | max dumped PNG width; code default 1280 when unset (the comment above it says 640 — stale), harness passes 1920. |
| `SHADPS4_TRACE_ADDR`, `SHADPS4_TRACE_SIZE` | `alias_trace.h:31-48` | enable `ALIASSTAT`, the only source of `depth_copies` (per traced allocation). Without `SHADPS4_TRACE_ADDR` there is no `ALIASSTAT` line and `depth_copies` is null. |
| **absent from this build** | grep `wt-crossfork-int/src/src` = 0 hits | `SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT`, `SHADPS4_DBG_DUMP_HMD_AT_SCENE`. These live in scene-eng's `wt-headless` patch. **C (clean-exit) and scene-gated dumps cannot be done on this binary.** |

### 5.2 `RENDERSTAT` fields (per second, cleared after each line)

Source: `image_stats.h:194-215`. Exact line:

```
RENDERSTAT expands= covered_expands= refreshes= upload_mips= upload_mb= skipped_mips= skipped_mb=
images_created= images_destroyed= image_mb= recycler_hits= recycler_misses= pool_images= pool_mb=
recycler_destroys= copies= copy_mb= buffered_copies= create_ms= destroy_ms= overlap_ms= depth_ms=
expand_ms= refresh_ms= vram_mb= vram_blocks=
```

* `images_created` / `images_destroyed` — VMA image create/destroy calls that second.
* `recycler_hits` / `recycler_misses` — pool served vs had to create. **Off-arm check: both must be
  0 every second; an ON cell must show misses > 0 on the first frames and hits > 0 afterwards.**
  A cell where the ON arm shows all-zero hits and misses did not actually enable the recycler.
* `create_ms` / `destroy_ms` — wall time inside the create/destroy paths that second.
* `pool_images`, `pool_mb`, `recycler_destroys` — gauges of what the pool holds / has released.
* `copies`, `copy_mb`, `buffered_copies` — whole-image copies recorded (the recycler's correctness
  cost, not just its speed).
* `refreshes`, `upload_mips`, `upload_mb`, `skipped_mips`, `skipped_mb` — guest-memory refresh work.
* `vram_mb`, `vram_blocks` — the emulator's own VMA totals.
* `depth_copies` — **not in RENDERSTAT**; only in `ALIASSTAT` (see above).

Contract mapping used by the harness (`result.json`): `*_per_frame = per-second mean / fps_mean` of
the same measured window; `refresh_uploads = refreshes per frame`; `depth_copies` = `ALIASSTAT`
per frame (null here unless `SHADPS4_TRACE_ADDR`). `passes_mean`, `draws_mean`, `mpx_mean` come
from `frame stats at …` lines; `eye_width/eye_height/format/samples` come from
`PrepareHmdFrame: HMD frame …` lines (menu: 5760x3240 SBS R8G8B8A8Unorm s1, i.e. 2880x3240 an eye).

## 6. Where R3 and R6 live, and their current status

| item | documented at | state in this candidate |
|---|---|---|
| **R3** — recycled contents are never treated as valid, but `eLoad` passes can read them; a tracker mistake would show the previous owner's pixels | `docs/research/recycler-review.md` R3 (line 31), §2.3 (line 145), §4 item 3 (line 494); promotion criteria 3 in `cross-fork-integration.md` §3; outstanding item 3 in §5 | **OPEN — no pixel A/B in a matched scene exists.** Promotion gate 6 (`1dd4eb4`) also requires the independent LUT-corruption report to be resolved on this binary. |
| **R6** — the teardown ordering (`ImageRecycler::Shutdown()` then `vmaDestroyAllocator`) is correct by construction but never executed, because the process leaves through `std::quick_exit` (`emulator.cpp:326,543,587,660,681,690`) which skips `~Presenter`/`~Instance` | `recycler-review.md` R6 (line 34), §2.5 (line 221), §4 item 5; outstanding item 5 in `cross-fork-integration.md` §5 | **OPEN and not reachable on this binary**: no clean-exit env exists, so the harness hard-stops the emulator (`Stop-Process -Force`). C is BLOCKED until a build with `SHADPS4_EXIT_AFTER` + `SHADPS4_CLEAN_EXIT` (wt-headless) is the target. |

## 7. What is already validated vs what is open

Validated (by the Lead's earlier phase, re-checked here against files/source):

* the branch builds cleanly; the exe is newer than all emulator sources;
* 580/590 tests pass, 10 documented pre-existing failures; 19/19 `ImageCoverage` tests pass;
* recycler defaults off (`0a09829`) and the env parser means off for `0/false/off/no/unset`;
* R1/R2/R4/R10/R13 fixes are in the tree (`117a3bb`), the A1 copy-coverage assertion is in (`7a2b042`);
* the RenderDoc scene-gated trigger is compiled in (`3d375a4`), not yet exercised live;
* the candidate reaches the headless menu at ~480–493 draws / 60 fps (Lead's report); gameplay is
  not reachable without scene-eng's patch — so **A/B/D/E will be marked CALIBRATION-ONLY until a
  gameplay-matched cell exists**;
* binary identity vs the old default-ON investigation build (`A02FB23F…`) is settled: different build.

Open / blocked:

* A — matched gameplay cells (on/off/off-on), need gameplay scene or Lead's explicit approval to
  report calibration-only menu cells;
* B — R3 pixel A/B (noise floor first: off-vs-off, then off-vs-on; menu eye images 5760x3240 split
  left/right; fixed HMD frame ids in this build);
* C — R6 clean exit: **BLOCKED** on this binary (no `SHADPS4_EXIT_AFTER`/`SHADPS4_CLEAN_EXIT`);
* D — RenderDoc scene-gated capture of the classroom, .rdc validated with `renderdoccmd` (v1.47);
* E — 100 % / 120 Hz configuration cell, reported separately from the 300 % benchmark.

## 8. Machine state at the time of this check (00:50 local)

* GPU slot (`_build/team/hmd-slot`): **free**, queue empty, no USER-HOLD at the last check
  (harness-eng self-tests acquired/released at 13:59Z).
* `vrserver.exe` (SteamVR background) running; **no** `shadps4.exe`, `ninja`, `clang-cl`, `link`,
  `msbuild`, `renderdoccmd` process.
* No emulator or capture was started by this agent.
* From 01:07 the machine is **headless-only** by user directive (`_build/team/HEADLESS-ONLY.md`):
  no SteamVR/OpenXR/headset cell, every run `SHADPS4_VR=1 SHADPS4_OPENXR=0 SHADPS4_VR_DEMO=0`;
  `testslot.sh` refuses non-headless reasons (exit 8). The A–E runner already holds the lock with
  `--kind headless` and passes no OpenXR switch of its own (the harness owns that contract).

## 9. Evidence paths

* candidate tree: `wt-crossfork-int/`; binary `wt-crossfork-int/build/shadPS4.exe`
* build logs: `_build/logs/int-build-1..4.log`; tests: `_build/logs/int-ctest2.log`
* research docs: `wt-crossfork-int/docs/research/{cross-fork-integration,recycler-review,gameplay-benchmark-procedure,renderdoc-reproducibility,upstream-reconciliation}.md`
* old investigation session (not this build): `_run/pc-vr-recycler-try/user/log/shad_log.txt`
* harness: `_build/headless/headless-vr-gameplay.ps1`; lock: `_build/team/gpu-lock.sh`
* comparison tools: `_build/bench/capture-compare.py`, `_build/bench/capture-diff.py`

## 10. The validation build (`wt-headless`) — why A–E run there and why the comparison is still valid

The candidate binary cannot reach gameplay headless: the menu needs a *located* controller of a
*worn* headset (`_build/PASSES-AND-PACING.md` §20). Scene-eng's `wt-headless` worktree
(`feature/headless-vr-scene`, based on `1a53ceb`) adds test-only, env-gated hooks for exactly that.
Checked read-only at 00:50–01:40; **it had no build yet** (`wt-headless/build/shadPS4.exe` absent),
so A–E are prepared against its interface and not yet run.

### 10.1 The renderer is the candidate's renderer

```bash
wsl.exe -e git -C /mnt/m/Development/Deracine-work/wt-headless diff 1a53ceb --name-only -- src/src/video_core
# src/src/video_core/amdgpu/liverpool.cpp        (Stop() refactor: joinable check, used by clean exit)
# src/src/video_core/amdgpu/liverpool.h
# src/src/video_core/renderer_vulkan/vk_presenter.cpp   (scene-dump hook, absolute dump folder)
# src/src/video_core/renderer_vulkan/vk_scheduler.cpp   (scene-dump gate + ALIAS/AB helpers already in 1a53ceb)
# src/src/video_core/renderer_vulkan/vk_scheduler.h
```

`git diff 1a53ceb` over every recycler file is **empty** — `vk_image_recycler.cpp/.h`,
`texture_cache/image.cpp/.h`, `texture_cache/texture_cache.cpp/.h`,
`texture_cache/image_stats.h`, `texture_cache/image_coverage.h`, `vk_instance.cpp`. The recycler
code, its keys, its counters and its default-off switch are byte-identical to the candidate.

### 10.2 The hooks are no-ops without their environment

* `SHADPS4_DBG_DUMP_HMD_AT_SCENE` (`vk_scheduler.cpp`): the parser returns `draws=0` when the
  variable is unset/empty; `NoteHmdDumpSceneWindow` returns immediately on `!request.Wants()`, and
  `TakeHmdDumpRequest` returns false unless armed. With the variable unset the presenter never
  calls `DumpGpuImages` from this hook.
* The absolute-folder branch in `DumpGpuImages` only changes behaviour when the requested folder is
  absolute; the normal `rt_dump-<frame>` and `rt_dump-scene` names stay relative to the user dir.
* `SHADPS4_EXIT_AFTER` and `SHADPS4_CLEAN_EXIT` (`emulator.cpp`) are `getenv` checks; unset means
  the old `std::quick_exit` path, exactly as in the candidate.
* The rest of the diff is input/VR (`scripted_input`, `pad`, `vr_runtime`, `video_out`, `emulator`,
  `sdl_window`) — test-only scaffolding that does not enter the Vulkan renderer's frame path.

**Consequence:** both arms of every comparison run the *same* `wt-headless` binary, the *same*
harness and the *same* env set; the only difference is `SHADPS4_DBG_IMAGE_RECYCLER=0|1`. A/B/A is
therefore a valid single-binary comparison, and the docs' rule (arms must agree on draws/Mpx within
5 %, fps only beyond the repeat spread) applies unchanged. The results are about the recycler code
as it exists at `1dd4eb4`, exercised through the test-only scene-entry path; they are not a
measurement of the menu.

### 10.3 R6 is reachable only on this build

The clean-exit path destroys the presenter through `GnmDriver::ResetPresenter()`, which stops the
GPU/present threads and resets `presenter`; that runs `~Presenter` → `~Instance` →
`ImageRecycler::Shutdown()` → `vmaDestroyAllocator`. The log markers C requires are:

```
SHADPS4_EXIT_AFTER=…
SHADPS4_CLEAN_EXIT: destroying the presenter before exiting
SHADPS4_CLEAN_EXIT: the guest threads are paused
SHADPS4_CLEAN_EXIT: stopping the GPU and present threads
SHADPS4_CLEAN_EXIT: destroying the presenter
SHADPS4_CLEAN_EXIT: the presenter is destroyed
SHADPS4_CLEAN_EXIT: the presenter is gone
```

The path may also refuse: `SHADPS4_CLEAN_EXIT: the presenter was left alone (R6 blocked)` /
"did not pause". That is **not** a pass — the presenter (and therefore
`ImageRecycler::Shutdown()` + `vmaDestroyAllocator`) did not run, so C reports BLOCKED.

`ImageRecycler::Shutdown()` itself logs nothing, so "the recycler teardown line" does not exist;
C evidences the teardown through these presenter-destruction markers plus exit code 0, no
validation errors and no crash markers.

### 10.4 Scene dump: hook vs harness

`SHADPS4_DBG_DUMP_HMD_AT_SCENE="<draws>,<mpx>"` is a **lower-bound** gate, and the menu
(~480–493 draws, ~460–500 Mpx) is inside that band, so the hook can fire on the menu before gameplay
starts. B therefore defaults to the harness's own gameplay-gated dumps (menu-baseline + gameplay
candidate, selected by the harness's loading-dip + image-difference verdict), and only falls back to
the hook or to fixed HMD frame ids when asked (`-BDumpMode scene|frames`). A black or menu dump is
treated as INCONCLUSIVE, never as a pass.



## 11. The gameplay scene (M2 confirmed), the save it needs, and a suspected R3 case

### 11.1 M2: headless menu -> Continue -> gameplay

scene-eng's proof cell is `_build/headless/probe-scene/evidence/m2-confirm-1` (label in
`label.txt`; the exact input script in `input-script.txt`, and the harness's own default for
`-Gate gameplay` is `_build/headless/inputs/menu-continue-move.txt`). Essential recipe (the harness
now defaults to it for gameplay cells):

* `-Gate gameplay` turns on `-SyntheticHands auto`, `-Autopress` with `-AutopressSeconds 45`
  (passes the camera/headset check screen, stops before the menu) and `SHADPS4_MOVE_PITCH=0`.
* input: fixed head pose; `pad=0.30,-0.12,-0.5` at 46 s (mirrored pointer hand on Continue);
  `move1=6,255` presses at 60 s and 63 s (Move button + trigger).
* result: menu -> Continue highlight -> loading dip -> classroom gameplay from ~49–65 s (the
  earlier 90 s figure was the long recipe; the confirmed one is faster).

**Actual signature for this save/scene (not the brief's 480-494):**

| phase | draws/frame | Mpx/frame | passes/frame | fps | notes |
|---|---|---|---|---|---|
| menu / front end | 465–500 | ~480 | ~65 | ~58–60 (paced) | must never be reported as gameplay |
| loading dip | 42–55 | low | low | — | between menu and gameplay |
| **gameplay** | **1866–1900** | **620–720** | **42–46** | **30–35** | HMD 5760x3240 s1 R8G8B8A8Unorm, 2880x3240 an eye |

Gate and band adopted before measurement (Lead, 03:0x): gameplay gate `draws >= 800`; measured
band **draws 1500–2300, Mpx 550–750, passes 35–55**; the image gate still requires the gameplay
eye picture to differ from the menu baseline (menu vs gameplay SSIM measured 0.417, masked hash
unequal — see `evidence/gameplay-reference-candidate/`). The runner reports the **actual** band it
sees, never the nominal one.

### 11.2 The save the classroom loads from

Only a lab carrying slot 1 (`MGS_GameData_1`, stage `m00_00_10_10`) plus `MGS_SystemData` loads the
classroom; the quality-lab save (slot 0, stage `m00_00_80_10`) sits on the menu forever. Source:

`_run/pc-vr-recycler-try/user/home/1000/savedata/CUSA11919/SaveData`

| file | bytes | sha256 |
|---|---|---|
| `MGS_GameData_0` | 17,047 | `366B88B24B59A4D77BEEBFC72A1848724B36DC53DBC83A5E3FD29474FCE49D9F` |
| `MGS_GameData_1` | 17,313 | `C30D33846D35B090B3F89D4D1E6AD8C73B6B6DA264852BFEC9ECF8CD4214F93C` |
| `MGS_SystemData` | 2,897 | `EAB7FE140A57988167D45D8ABCB38C4AA0F518062CE26CD77D2BF027FD48745D` |

The harness (`Get-SaveDataInfo`, added by harness-eng) refreshes every lab from that source, throws
if slot 1 is missing, and records `meta.save_data` with the lab file hashes. The validation runner
preflights the same source (refuses to run cells without slot 1 unless `-SkipSaveCheck`), writes
`results/SAVE.json`, and compares every cell's `meta.save_data` with the source hashes (`cell.save`).

### 11.3 Suspected R3 case: the recycler-ON arm may not leave the menu

Reported by the Lead (2026-11-18 03:1x): a harness gameplay smoke with
`SHADPS4_DBG_IMAGE_RECYCLER=1` stayed on the menu at ~485 draws for 130 s, while scene-eng's
winning M2 runs (recycler OFF) reached gameplay in ~49–65 s. **This is a hypothesis about one run,
not yet a reproduced finding.** If it reproduces under a controlled pair:

* it is a concrete correctness failure (the recycler-on arm behaves differently before any timing
  can be compared) — exactly the class R3 exists to catch, and the recycler stays off;
* the ON cell is **invalid for timing** ("recycler arm did not enter gameplay"), not a slow cell;
* the evidence to keep is the log, the stuck-menu band (~485 draws / ~480 Mpx), the absence of a
  loading dip, the ON-arm recycler counters (to prove the switch was on), and the OFF cell that
  did reach gameplay.

The runner implements this as `-SmokePair` (one OFF cell, then one ON cell, ON with at most two
attempts) and as a first-class verdict: A acceptance returns `FAIL-R3-SUSPECTED` with the ON cell's
menu evidence, and B reports `BLOCKED` rather than pretending a pixel A/B exists. No matrix cell
runs until the Lead confirms the OFF gameplay cell.
