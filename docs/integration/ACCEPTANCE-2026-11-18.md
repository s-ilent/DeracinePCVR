# Acceptance checkpoint — R6 and second-lab gameplay (2026-11-18)

Baseline accepted: `integration/deracine-unified-2026-11` @ `13bea1f5`. This checkpoint adds the
headless agent's R6 ImGui repair, one independently constructed second lab's gameplay cell, the
save-slot provisioning fix that cell exposed, and the headset acceptance checklist. It does not
start a new integration sweep and does not add renderer optimizations; the image recycler remains
excluded and off.

## 1. Build identity

| | |
|---|---|
| Source | `9b918eb` (R6 applied; no C++ change after it) |
| Executable | `wt-accept\build\shadPS4.exe`, 73,333,248 bytes |
| SHA-256 | `4493A7F912732BF946B02004E527388392F2314F526F751F5CE6B375A8E9418D` |
| Toolchain | LLVM/clang-cl 23.1.5, VS 2022 Build Tools, Ninja, Release |

C++ tests: **794 total, 790 passed, 4 failed, 1 skipped** — the same four inherited failures
(gcn and usbd `_NOT_BUILT`, `ReadConstSnapshotDiag.IsolationDisabledAfterTestB`,
`BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`). The new `ImGuiExitGuard` suite passes
4/4. Benchmark Python suite: **116/116**. Process-ownership PowerShell suite: **13/13**.

## 2. R6 — ImGui quick-exit repair (headless agent's fix, applied verbatim)

The published baseline still registered `std::at_quick_exit` calling
`SaveIniSettingsToDisk(GetIO().IniFilename)` **after** the clean-exit path had destroyed the
presenter and the ImGui context, so the callback dereferenced a null `GImGui` ("No current
context", 0xC0000003). The headless agent's `bc524dd` adds an `ExitSaveGuard`: `Shutdown` saves
while the context is alive and marks it destroyed; the quick-exit callback is a no-op then, and
both are idempotent. It was cherry-picked as `9b918eb` with only a test-CMake insertion conflict
resolved (both target blocks kept). No duplicate repair was written.

Live validation on the integrated build (the second-lab cell below): normal quit through
`SHADPS4_EXIT_AFTER` + `SHADPS4_CLEAN_EXIT=1` produced exit code 0, all teardown markers, no
assertion, no orphan process.

## 3. Second independently constructed lab

| | |
|---|---|
| Lab | fresh isolated lab `_build\unified\accept-lab2-sfo\lab`, built from the tracked `pc-vr` template |
| Save | freshly provisioned `_build\unified\accept-lab2-save-sfo`, all four files verified: `MGS_GameData_0`, `MGS_GameData_1` (slot 1, `m00_00_10_10`), `MGS_SystemData`, `sce_sys/param.sfo` |
| Executable | the exact checkpoint build, `4493A7F9…` |
| Entry | menu-gated scripted input (integrated benchmark now detects a settled menu and injects the sweep there, rather than firing fixed presses during the cold load) |
| Result | **VALID**, anchor 61 s, 324/324 windows in band, unique rendered FPS **34.14**, draws **1716–1943** (mean 1865.6), Mpx mean **873.8**, passes mean **56.5**, GPU 53.6 % busy, save verified, clean exit 0 |
| Evidence | `_build\unified\accept-lab2-sfo\run.json`, `shad_log.txt`, `input-live.txt` |

The first cell (`_build\unified\smoke-publish`, pre-R6 executable `D8FA1D34…`) measured the same
scene at 101/101 windows in band and 38.3 unique FPS. The two cells differ in FPS and GPU state, not
in scene or workload class; they are separate labs, runs and executables.

### What the second lab exposed and what was fixed

1. **Save fixture was incomplete.** `provision.ps1` copied only the three data files; the headless
   handoff requires `sce_sys/param.sfo` or the title never loads Continue. The manifest now pins
   `sce_sys/param.sfo` (`A5081D3F…`, 1,596 B), the provisioner creates nested destinations, and the
   runner/gate verify the nested path. Without it the lab reached the menu and ignored every input.
2. **The provisioner did not work at all** because the `-Manifest` string parameter and the parsed
   `$manifest` object were the same case-insensitive PowerShell variable. Renamed; provisioning now
   copies and verifies exactly the manifest files.
3. **Fixed-time entry fails on independently constructed labs** (cold shader compilation shifts the
   menu past the script's clock). The integrated benchmark now watches the frame-stats windows and
   injects the verified sweep when a settled menu is measured, with bounded re-injection. All headless
   switches remain opt-in.

## 4. Process ownership and shutdown

- `runner.ps1`'s trap, `Fail-Run` and normal cleanup all call one `Stop-LabGame`; the only stop
  mechanism is `process_ownership.ps1` (pid + creation time + executable path, re-verified against a
  fresh process table, RenderDoc wrapper resolved through live parent links). The repository has no
  `Get-Process -Name shadps4` sweep in the runner; the old ones are gone from all three paths.
- 13 simulated ownership cases pass, including two simultaneous sessions, PID reuse, wrapper+child,
  partial startup failure and an unreadable snapshot (report, never kill).
- The live gameplay cell's shutdown was clean (exit 0, no assertion, no orphan).
- A live **classroom** runner probe was not executed: its private shadow still lacks the one-time
  `benchmark-shadow.json` manifest, and the integration agent did not modify the private lab.
  Provision it with `pwsh -File tools\benchmark\new-shadow-manifest.ps1 -ShadowRoot <shadow> -Verify`
  before using `runner.ps1`.

## 5. Headset acceptance

`docs/integration/HEADSET-ACCEPTANCE.md` is the checklist: OpenXR initialization, stereo/frustum,
tracking, Move input, in-game audio/SNDX, colour (RGB gate, not scene similarity), presentation
resolution, shutdown/recovery, and the pass/fail/blocked record. **No headset session has been
run**; this remains the only end-to-end acceptance gate.

## 6. What is deliberately unchanged

- The image recycler stays excluded/default-off; no speculative renderer optimization is added.
- Depth pass, barrier, camera clock and downsampling remain opt-in.
- The MSAA/coverage/`GpuModified` #5433/#5444 guards are untouched.
- No deployment to `pc-vr/` or the active installation; no SteamVR/OpenXR service was touched.
