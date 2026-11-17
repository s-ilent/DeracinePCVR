# Assignment 01 — evidence, results and limitations

Assignment: *Recover Reproducible Classroom/RenderDoc Workflow and Enforce Engineering
Guardrails* (handoff 2026-11-17). This file records what was actually built, measured and
verified on branch `feature/autonomous-guardrails-2026-11-17`. Anything not proven here is marked
BLOCKED or UNVERIFIED rather than assumed.

## Status summary

| deliverable | status | evidence |
|---|---|---|
| contract installed + AGENTS pointer | PASSED | `docs/AUTONOMOUS_ENGINEERING.md`; commit `995e84c` |
| executable benchmark harness, provenance manifest, independent validator | PASSED | `tools/benchmark/`; commits `0f888e7` + harness fixes; 31 unit tests |
| 10 prior-failure negative cases + positive controls | PASSED | `tools/benchmark/tests/`; 31 tests, listed below |
| camera de-deadlock extracted as its own commit | EXPERIMENTAL (self-validated; production safety unproven) | `d7c7d5f`; 8 unit tests; scene reached headlessly; ring-margin question documented |
| `SHADPS4_DBG_AUTOPRESS` extracted, default off | PASSED (dev-only) | `b32b88d` + `fcaa02f`; 6 unit tests |
| RenderDoc trigger | PASSED (code) | `9deb0bb`; exact `7130e9f` hunk |
| clean build of the combined instrumentation branch | PASSED | `build\shadps4.exe` SHA-256 `B6830480…` |
| gtest suite built and run (CTest) | PASSED with inherited failures | 540/550; see "Tests" |
| screenshot-verified classroom run, unique rendered FPS | PASSED (one run; not a performance claim) | `_out/runs/classroom-valid-r9/` |
| fresh classroom `.rdc` with extracted final image | BLOCKED (GPU device loss) | see "RenderDoc capture" |
| independent reviewer | two read-only reviews: the first found 4 blockers and several gaps (fixed); the second reproduced r9 from the manifest and the log and approved it as one qualified cell | see "Review" |
| integration | NOT PERFORMED (by assignment) | `INTEGRATION-PLAN.md` |

## Isolation and protected state

- Worktree: `M:\Development\Deracine-work\wt-autonomous-guardrails` (new, created for this
  assignment). Branch: `feature/autonomous-guardrails-2026-11-17`, cut from `acda49e`
  (`acda49e709f81d6ab64824f3482ce006abdf8f4c`, "fix(ngs2): give the user effect its detector
  pointers", 2026-11-17 01:44 +1030).
- Private lab runtime: `wt-autonomous-guardrails\_lab\pc-vr` (copy of `lab\pc-vr-quality`; the
  original is untouched). Private game shadow:
  `D:\Temp\Deracine\CUSA11919-guardrails-2026-11-17` (eboot copy; own `gameproject\saved`;
  read-only junctions to the shared content). No file under the shared `D:\Temp\Deracine` or
  `lab\pc-vr-quality` was modified.
- Preflight/postflight hashed 33 protected files and recorded 16 worktrees. The active runtime
  stayed byte-identical for the whole session:

| protected file | bytes | SHA-256 |
|---|---|---|
| `DeracinePCVR\pc-vr\shadps4.exe` (active, experimental per recovery) | 73,055,232 | `e6a5a165183eb9f121133c368bb6641d661f48e3dfc3974b105a67d7c1ad3b80` |
| `DeracinePCVR\pc-vr\launch.ps1` | 60,341 | `55adc73322bf27e3d2ee0d4e481495404f1c5b34028eeedbb03c0178ba2c9e29` |
| `DeracinePCVR\pc-vr\settings.txt` | 2,723 | `917688692f1b6602fe12402f369cec3b4516a2de4c0cb31d448c80cda202e04b` |
| `DeracinePCVR\pc-vr\user\config.json` | 3,524 | `bf745e9deacfbf70b2d29c5c360919a33fa76fbc39ac8fbccffb2dd3e68e8169` |

  The recovery store, shared `_build\bench`, shared `lab\pc-vr-quality` and every pre-existing
  worktree were recorded before and after each session; no unexpected change was reported.
- Environment: Windows 11 (10.0.26100 SDK), AMD Ryzen 9 7900X, clang-cl from LLVM 23.1.5 at
  `C:\Tools\llvm-23.1.5`, VS2022 Build Tools, Ninja, CMake, externals tree with `magic_enum`
  pinned to v0.9.7, `-DHAVE_ENDIAN_H:INTERNAL=`. SteamVR (`vrserver`, `vrmonitor`) was running.

## Commits

Base `acda49e`; branch head at the time of writing: `818b667`.

| commit | subject | scope |
|---|---|---|
| `995e84c` | docs: install the autonomous engineering contract | `docs/AUTONOMOUS_ENGINEERING.md`, both `AGENTS.md` |
| `9deb0bb` | diag(video_core): trigger one RenderDoc capture after N seconds (salvaged) | `vk_scheduler.cpp`, exact `7130e9f` |
| `4b93bd8` | diag(vulkan): log the frame count of each frame-stats window | `vk_scheduler.cpp`, one log line |
| `d7c7d5f` | fix(camera): give the virtual camera sensor its own clock | `camera.cpp`, `virtual_camera_clock.h` |
| `b32b88d` | test(devtools): optional Cross/Move injection for the headless benchmark | `autopress.h`, `pad.cpp`, `move.cpp` |
| `de85edf` | test: cover the virtual camera clock and the headless input schedule | two gtest files + CMake |
| `fcaa02f` | fix(devtools): a disabled test-input injector never presses | `autopress.h` |
| `0f888e7` | tools: add the verified classroom benchmark harness and validity gate | `tools/benchmark/` |
| `b6b9807` | fix(tools): stop the emulator on any failure and serialize captures | `runner.ps1` |
| `d1bf237` | fix(tools): write the manifest for long runs and find the app's RenderDoc capture | `runner.ps1` |
| `9b890d1` | fix(tools): recompute unique FPS over total time and harden validity gates | runner, validator, schema, tests, README |
| `9863e97` | fix(tools): allow the scene's brief dips and the frame-boundary window overshoot | validator, tests |
| `9289534` | fix(tools): skip truncated frame-stats lines and bound the window by stalls | runner, validator |
| `818b667` | fix(tools,camera): stop hashing the private shadow config and correct the Move comment | preflight, move.cpp |

Commit `b6b9807`'s message says "shad4 process"; the intended word is shadps4. No source meaning.
The camera commit `d7c7d5f` overstates its safety claim ("cannot find the ring entry overwritten");
the independent review showed the old invariant was "a frame for every two presents" and the new
one only bounds the sensor to one frame per read. See "Camera safety" below; the status is
experimental.

## Builds and executables

| build | commit | bytes | SHA-256 |
|---|---|---|---|
| instrumentation, first clean build | `acda49e` + camera/autopress/RenderDoc | 72,777,216 | `CC07BA25A55BBC0915FBB4135DFF5F51E6C748261D69BA5882C8BC591C6FB97E` |
| instrumentation + frame-count logging | + `4b93bd8` | 72,776,704 | `278535B0E23C5BC83AC273F006E781E6AD1592E67A20CCC5F250F8E483D61487` |
| final build (autopress disabled-injector fix) | `fcaa02f` | 72,777,216 | `B6830480CBE33FD5E2DBF374ED213234C7E09792BAF8F9814E7781D890AFD774` |

The final build is what the classroom and RenderDoc runs used. `run.json` records
`built_from_head` only when the session declares it (`-BuiltFromCommit`); for r9 that is
`fcaa02f`, with `build_commit_source: declared by the session`. The *session* HEAD is
`source.head_sha` (r9: `9289534`). The validator checks that a declared build commit exists in
the worktree; it cannot prove the binary was produced from it, only that the declaration is
consistent with the repository. Every build is a clean private Release build with the pinned
toolchain; rebuild command:

```powershell
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-autonomous-guardrails
```

Build logs: `_out\build-emulator.log`, `_out\build-instrumented-2.log`.

## Tests

### Harness unit tests (Python)

```
& $PY -m unittest discover -s tools\benchmark\tests -v
Ran 26 tests ... OK
```

10 negative cases, one per prior failure, plus positive controls and the defects found in review:

| prior failure | test |
|---|---|
| compositor/present 120 Hz reported as 41 fps scene | `test_present_cadence_is_not_a_frame_rate_source` |
| claimed 40 s window actually 0.65 s | `test_claimed_window_shorter_than_recorded` |
| 38-draw light front end / SIE splash with fabricated FPS | `test_splash_screenshot_cannot_be_the_classroom` |
| 500+ draws but wrong or missing screenshot | `test_wrong_screenshot_fails_even_with_heavy_draws` |
| missing autopress, vblank, resolution, GPU/driver, exe hash | `test_missing_required_provenance` |
| vblank 60 vs 120 cells claimed comparable | `test_different_vblank_cells_are_not_comparable` |
| path-less delete / `..` / out-of-root cleanup | `test_cleanup_paths_must_be_absolute_inside_the_private_root`, `test_path_guard.py` (junction escape, empty, relative, deny root) |
| truncated / nonmonotonic / missing samples | `test_timing_series_must_be_contiguous_and_monotonic` |
| metadata names a different executable | `test_executable_hash_must_match_the_inspected_file` |
| content drift between A and B | `test_content_drift_refuses_an_ab_claim` |
| wrong FPS aggregate (harmonic mean) | `test_unique_fps_is_total_frames_over_total_time` |
| window that leaves the heavy scene | `test_in_window_workload_must_stay_in_the_scene` |
| harmless observations invalidating a shared-lab run | `test_observed_protected_changes_do_not_fail_but_drift_does` |
| stale failed directory looking valid | `test_failed_run_directory_is_never_valid` |
| duration estimated without frame counts | `test_missing_frame_counts_are_not_estimated` |
| requested vs actual size mismatch | `test_actual_presentation_must_match_requested` |
| — positive controls | `test_valid_run_passes`, `test_same_binary_replicate_is_inconclusive` |

### C++ unit tests

`build-tests` (separate `ENABLE_TESTS=ON` tree) built the two new targets and they pass:

- `shadps4_virtual_camera_clock_test`: 8/8.
- `shadps4_autopress_test`: 6/6.

### Full CTest

`ctest --test-dir build-tests -j 4 --output-on-failure`: **540 of 550 passed (98%)**, 3.89 s.
Ten are not our changes:

- Not built (inherited compile errors in files unchanged from `acda49e`):
  `shadps4_usbd_test` (`tests/libraries/test_usbd_event_loop.cpp` includes `sys/time.h`),
  `shadps4_memory_patcher_test`, `shadps4_patch_repository_test`, `shadps4_patch_state_test`,
  `shadps4_patch_domain_test`, `shadps4_patch_session_test`, `shadps4_preset_test`
  (`_QUEUE_USER_APC_FLAGS` redefinition in `src/src/common/ntapi.h` against Windows SDK
  10.0.26100), `shadps4_gcn_test` (link: `EmitContext::DefineSceneEasu`, `NumberUtils::S10ToSnorm`).
- Runtime failures in untouched sources: `ReadConstSnapshotDiag.IsolationDisabledAfterTestB`
  (expects `ShouldIsolateReadConstBuffer` false, gets true) and
  `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot` (expects true, gets false).
  `git diff acda49e` for these sources is empty; the failures are deterministic and unrelated to
  the camera/pad/log changes. A second full baseline CTest tree was **not** built; this is an
  honest gap, and the classification rests on the unchanged-file evidence above.

Log: `_out\ctest-all.log`.

## Classroom benchmark

**One valid, screenshot-verified run: `classroom-valid-r9`.**

```powershell
pwsh -File tools\benchmark\runner.ps1 `
  -Exe build\shadps4.exe -Out _out\runs\classroom-valid-r9 -Capture `
  -Python <python-with-Pillow> -BuiltFromCommit fcaa02ff41522b510d3d72351199330dbe86c549 `
  -TimeoutSeconds 220
# -> VALID classroom-valid-r9, exit 0
```

| item | value |
|---|---|
| worktree HEAD at session | `9289534e516e45dcaf0a11f028a0fe2eeec50850` (`9289534`, harness fixes; emulator unchanged) |
| executable | `B6830480CBE33FD5E2DBF374ED213234C7E09792BAF8F9814E7781D890AFD774`, built from `fcaa02ff41522b510d3d72351199330dbe86c549` |
| scenario | `deracine-classroom-menu-300-msaa2-vblank60`, 300% / MSAA 2 |
| guest / actual | 2880×3240 per eye; stereo read back from the log `5760x3240` |
| presentation / actual | `SHADPS4_VR_SCALE=0.5`; log `2880x1632 window, per eye 1440x1632, base eye 2880x3240, scale: 0.50x` |
| vblank | requested 60, written 60, read back 60 |
| autopress | `SHADPS4_DBG_AUTOPRESS=1`, 24 logged bursts |
| scene anchor | sample #64, 514 draws, 552.8 Mpx, 85.1 passes at 35 s |
| **unique rendered FPS** | **39.83** — total frames / total measured time over 80 × 0.5 s windows |
| measured window | 40.949 s (Σ frames/fps), runner-observed 41.426 s |
| frame time | p50 24.21, p90 25.47, p95 39.23, p99 89.36 ms (0.5 s window means) |
| content in window | 534.3 draws, 535.0 Mpx, GPU busy 99.8 %; 79/80 samples above the entry gate |
| screenshot | `scene.png`, 1280×720, SHA-256 `6b9d2f2acb7cfb45ad183a28b4760cb32f19749d5c7b651fea6d748763c02611`, similarity **1.0000** to the reviewed classroom+menu reference, taken 15.5 s after the anchor |
| postflight | protected files unchanged, 0 unexpected, 0 observed |

Validator output (independent recomputation, exit 0):

```
VALID  classroom-valid-r9
  unique rendered FPS: 39.83 (80 x 0.5 s windows, 40.95 s)
  frame time p50/p95/p99: 24.21 / 39.23 / 89.36 ms (window means)
  scene: classroom-main-menu-overlay (similarity 1.0000)
```

The screenshot was also reviewed visually by the Lead: it is the same classroom and the same
floating **New game / Load game / Continue / System** menu as the reference.

### What this number is, and is not

- It is one valid measurement of the classroom front end on this machine and build. It is not a
  comparison, a speedup, or a statement about gameplay.
- The GPU state was not clean/quiet: after the RenderDoc device loss the machine needed a SteamVR
  restart, and this session ran at 99.8 % GPU busy with a 24 ms p50 while earlier runs of the same
  binary in the same configuration reached 52–57 fps. **The same-binary noise floor was not
  re-measured in this final state**, so the value must not be compared with the historical
  40–58 fps anchors as if the machine were the same.
- Frame times are means of 0.5 s windows, not individual frames.
- The scene gate is a screenshot signature plus in-window draws/Mpx, not object recognition; the
  overlay presence is inferred from matching the reference that contains it, then confirmed by
  eye.

### Attempts and harness defects found on the way

| run | outcome |
|---|---|
| `classroom-r2` | scene reached, but the dump at anchor+2 s was black (fade) and the old validator rejected the observed 41.8 s window; useful evidence, invalid |
| `classroom-r3` | scene reached, screenshot 1.0000, then the runner died writing the manifest (`@()` over a generic List of ordered dictionaries) |
| `classroom-benchmark-r4` | same manifest crash; its 80 rows were used to prove the harmonic-mean and window-duration defects |
| `classroom-rdoc` | RenderDoc injected (RenderDoc log present); the app reached the anchor and then ended; device-lost during submit at 20:24:11 is inferred from timing (the app log was not preserved and the RenderDoc log has no loss line); no `.rdc` |
| `classroom-benchmark-final` | emulator hung at the light front end; the runner timed out; no manifest |
| `classroom-valid-r5` | the hung emulator later crashed with 0xC0000005 when the run was cleaned up; no manifest |
| `classroom-valid-r6`, `r8` | completed; rejected for a wrong 39-char build SHA (operator error), a truncated stats row, and a 1.1 s hitch overshooting a too-tight window bound — each cause was fixed and tested |
| `classroom-valid-r7` | known intermittent crash, exit `-2147483645` |
| **`classroom-valid-r9`** | **valid** (above) |

The independent reviewer found four real defects in the first harness version (harmonic-mean FPS,
an unsatisfiable 40 s window bound, observed-vs-unexpected drift mixed together, and manifest
serialization). All were fixed in `9b890d1`, `9863e97`, `9289534` with new tests; the sequence
above is the evidence that each fix was exercised on hardware.

## RenderDoc capture

**BLOCKED: no fresh classroom `.rdc` was obtained.** The old capture remains only a splash.

Attempts (all in `_out/runs/`):

1. `classroom-rdoc` (20:23): `renderdoccmd capture` injected (RenderDoc log
   `%TEMP%\RenderDoc\RenderDoc_2026.11.17_20.23.21.log` shows the Vulkan capture layer, the
   capture template the app sets to `user\captures\CUSA11919`, and the device capturer; the app
   log was not preserved). The runner recorded the app reaching the anchor and ending while the
   scene screenshot wait was running. **Inference, not proven:** a device loss at 20:24:11
   (20:23:26 trigger initialisation + a 55 s trigger would be due at ~20:24:21; the app log and a
   loss line in the RenderDoc log are both missing) and therefore "before the trigger". No `.rdc`.
2. `classroom-rdoc-r9` (20:49) and `classroom-rdoc-r10` (20:52): RenderDoc **was attached** — the
   app log has `renderdoc.cpp:128 SetOutputDir: RenderDoc capture path` (only reachable when the
   API is non-null) and `CollectToolingInfo: Attached debugging tool: RenderDoc` — but no
   RenderDoc log was flushed and no `.rdc` appeared. Both processes device-lost in the VR present
   path (`Device lost during waiting for a frame`, `DEVICE_LOST_SNAPSHOT lastPresent=936`;
   `Device lost during submit`) before the classroom scene and before any trigger could fire.
3. The same device loss occurred **without** RenderDoc: `classroom-valid-r7` crashed with
   `-2147483645` (the known intermittent `AddressSpace::Protect` class), and the hung
   `classroom-benchmark-final` later died with 0xC0000005; a SteamVR restart was needed before
   `classroom-valid-r9` completed on the same build and configuration.

The blocker is the machine's Vulkan/GPU stability under the 300 % VR load, not a defect introduced
by this branch; RenderDoc attachment itself worked. It is **not proven** that RenderDoc caused the
loss (there is no RenderDoc-attached/capture-disabled control), nor that the in-app trigger ever
fired (no `RDOC capture triggered` line survives), nor which trigger value any attempt used (it is
not recorded in a surviving run). The RenderDoc integration is byte-identical to
`recovery/renderdoc:7130e9f`, builds, and is off by default; the runner now also finds the app's
own capture path (`user\captures\CUSA11919_frameNNN.rdc`, set by
`src/src/video_core/renderdoc.cpp`) instead of trusting `renderdoccmd -c`.

What is preserved: `INVALID.txt` and `console*.txt` for every attempt; `shad_log.txt` and
preflight for the attempts that produced them (`classroom-rdoc-r9`, `classroom-rdoc-r10`, and the
earlier `classroom-rdoc` had no log copy because the Fail-Run log copy landed later); the RenderDoc
log above; preflight/postflight where the run reached them. No source state or rendering target
was changed to force a capture.

To finish this gate when the GPU is stable: restart SteamVR, confirm a classroom run completes,
then

```powershell
pwsh -File tools\benchmark\runner.ps1 -Exe build\shadps4.exe `
  -Out _out\runs\classroom-rdoc -Capture -DumpWidth 640 `
  -RdocCapture 45 -RenderDocCaptureDir _out\runs\classroom-rdoc\rdoc `
  -Python <python-with-Pillow> -BuiltFromCommit <build-sha>
```

and check `run.json` for `artifacts.rdc`, `artifacts.rdc_png` and
`artifacts.rdc_png_similarity ≥ 0.85`; the validator checks the capture hash and the extracted
image against the same classroom reference.

## Camera safety (experimental)

The virtual-camera change makes the sensor advance on its own clock and never more than one
frame per read. It removes the dependency that stalled each `sceCameraReadState` for up to the
200 ms timeout (the old code waited for a guest frame that the reading thread itself might never
release). Unit tests cover the state machine, and the headless classroom now reaches its scene.

**Unproven:** the old code's stated invariant was "a frame for every two the title presents",
which protected the guest's three-entry camera ring with three *present* intervals of margin.
The new code gives three *read* intervals (roughly 25–50 ms at 60/120 Hz). A title presenting
slower than about 20 FPS at 60 Hz (or 40 FPS at 120 Hz) could still overwrite the entry it is
reading. The 200 ms timeout also means the old fault was a repeated stall, not a permanent
deadlock. **Do not promote this to a production fix until an instrumented camera-read vs
present-rate test at 60 and 120 Hz, plus a worn-headset session, establishes the ring safety.**
It is on the branch because the headless benchmark needs it.

## Review

Two independent read-only reviews were run (separate agent, own context; no edits to this tree).

**Review 1** found four blockers in the first harness version and several unenforced claims:

- the manifest writer crashed on the per-sample list (PowerShell `@()` over a generic List);
- unique FPS was the harmonic mean of window rates, not total frames / total time (3.3 % low);
- the 40 s window bound was unsatisfiable once the frame-boundary overshoot was accounted for;
- `protected_drift` mixed harmless observations with real drift, so every shared-lab run failed;
- plus: scene proof covered only the anchor screenshot, actual-vs-requested configuration was not
  validated, a failed run could leave a stale valid-looking manifest, the trap could kill an
  unrelated shadps4, and the camera commit overclaimed its ring safety.

All were fixed in `9b890d1`, `9863e97`, `9289534`, `818b667` and the tests were extended from 20
to 31 cases. Remaining review findings that are documentation-level are corrected in this file.

**Review 2** independently recomputed r9 from `run.json` **and** from `shad_log.txt` and matched
every number (Σ frames 1631, Σ(frames/fps) 40.948666 s, 39.8304 FPS, all percentiles, 79/80 gate,
hashes, scene similarity 1.000000). Its verdict, quoted:

> Can this number be quoted? **Yes — as exactly one valid cell, with qualifiers; no as a
> performance result.** 39.83 unique rendered FPS for `deracine-classroom-menu-300-msaa2-vblank60`
> (classroom + main-menu overlay, 300 % / MSAA 2, 2880×3240/eye guest, 1440×1632/eye OpenXR at
> scale 0.50, vblank 60), one valid run (`classroom-valid-r9`), 80 windows over 40.95 s, build
> `b6830480…` (declared built from `fcaa02f…`), validator exit 0, screenshot similarity 1.0000,
> in-window 473–572 draws / 453–574 Mpx, GPU 97–100 % busy.

Review 2 also identified one residual internal gap: `shad_log.txt` has 190 complete frame-stats
lines while the series has 188, and r9's `incomplete_stats_lines` says 0. The two missing windows
are pre-window (emulator 32 s and 46 s) and were read as fragments that failed the whole regex, so
the old counter (which only caught fragments that matched through `submits`) missed them. The
measured window is unaffected — the reviewer verified its 80 rows line-by-line against the log —
and the runner now counts any line that announced a frame-stats report but could not be read
whole. The validator still hashes the log rather than re-parsing it; that remains a stated
limitation, not a hidden one.

## Barriers: what is preventive and what only detects

- **Preventive for the harness's own operations:** `path_guard.py` refuses empty/relative/`..`
  paths, paths outside the private roots, paths under the active runtime/recovery/other
  worktrees, and reparse points that resolve outside the allowed root. The runner calls it before
  its copies, deletes and the session-state writes (lab executable/settings, engine.ini, frames,
  log, diag, run.json, captures, scene, RenderDoc capture copy). The guard does **not** cover the
  guard config itself, `INVALID.txt`, the preflight/postflight writes, or the fallback copy and
  stale-file removal inside `Fail-Run`; those are fixed paths inside the already-validated private
  output root.
- **Detection only:** `preflight.ps1`/`postflight.ps1` hash the protected files and compare. They
  cannot stop another process under the same Windows token, and no ACL, ownership or security
  setting was changed. A protected change makes the run invalid; it does not restore anything.
- **Not a sandbox:** a tool that ignores the guard can still write anywhere this user can. The
  protection is real only inside the committed harness path plus the hash comparison.

## Unresolved limitations

- The classroom is the front end, not gameplay. Nothing here proves quests, Move selection,
  gameplay pacing or the 120 FPS target; those need a worn headset and a real session.
- Frame times are means of the emulator's 0.5 s windows, not individual frames: p95/p99 describe
  windows. The unique FPS is the emulator's rendered-frame rate, not headset presentation.
- The screenshot identity is a normalised 64×36 grayscale correlation against one reviewed
  classroom frame; it separates classroom from splash/front end by a wide margin (0.93–1.00 vs
  ≈0), but it is not object recognition and a large art/lighting change requires re-reviewing the
  reference instead of lowering the threshold.
- The menu-overlay presence is inferred from matching the reference image (which contains the
  overlay) and confirmed by the reviewer's visual check; there is no independent overlay detector.
- The full baseline CTest comparison and a complete run of the 10 inherited failures were not
  repeated; see "Tests".
- RenderDoc CLI can open a capture and export its stored final backbuffer thumbnail; the deep
  replay UI (per-event pipeline state inspection) was not used. The extracted image is the
  capture's final backbuffer, not the 5760×3240 eye target; the harness's own render-target dump
  provides the full-resolution eye picture.
