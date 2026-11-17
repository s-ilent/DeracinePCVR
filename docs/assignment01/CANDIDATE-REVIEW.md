# Guardrails integration candidate — review record

Candidate: branch `candidate/guardrails-2026-11-17` at **`2ccc12a`**, worktree
`M:\Development\Deracine-work\wt-guardrails-candidate`, cut from `acda49e`. It is exactly the
fifteen reversible commits of `feature/autonomous-guardrails-2026-11-17` up to `2ccc12a`; the
later `fac43a3` (RenderDoc-image fallback) is **not** in this candidate.

```
2ccc12a docs,tools: correct the evidence claims and close the review's test gaps
818b667 fix(tools,camera): stop hashing the private shadow config and correct the Move test-input comment
9289534 fix(tools): skip truncated frame-stats lines and bound the window by stalls
9863e97 fix(tools): allow the scene's brief dips and the frame-boundary window overshoot
9b890d1 fix(tools): recompute unique FPS over total time and harden validity gates
d1bf237 fix(tools): write the manifest for long runs and find the app's RenderDoc capture
b6b9807 fix(tools): stop the emulator on any failure and serialize captures
fcaa02f fix(devtools): a disabled test-input injector never presses
0f888e7 tools: add the verified classroom benchmark harness and validity gate
de85edf test: cover the virtual camera clock and the headless input schedule
b32b88d test(devtools): optional Cross/Move injection for the headless benchmark
d7c7d5f fix(camera): give the virtual camera sensor its own clock
4b93bd8 diag(vulkan): log the frame count of each frame-stats window
9deb0bb diag(video_core): trigger one RenderDoc capture after N seconds (salvaged)
995e84c docs: install the autonomous engineering contract
```

Each commit is independently revertible; the harness commits are behaviour-free for the runtime,
the camera commit is the only guest-visible behaviour change and stays **experimental**, and the
autopress devtool stays **default-off**.

## Reproduced here

| check | result |
|---|---|
| clean private Release build of the candidate | exit 0, 3.7 min; `build\shadps4.exe`, 72,777,216 B, SHA-256 **`D86CE6CC2D70C035A80825F4504A598D7BA7D7CE9F45B2D0077B8E81E299AFE0`** |
| Python harness suite | **31/31 pass** in this worktree |
| independent log parse of `classroom-valid-r9` (does not import the validator) | log has 190 complete frame-stats lines; the last 80 are the window: 1631 frames / 40.948666 s = **39.8304 FPS**, p50/p95/p99 24.213/39.231/89.356 ms, 473–572 draws, 453.3–574 Mpx, GPU 97–100 %; log, scene and executable SHA-256 all match `run.json` |
| fail-closed protection | preflight/postflight on this worktree: unchanged sentinel → exit 0; same sentinel altered → exit 1, `content changed: …sentinel.bin`, `protected_unchanged=false`. No real protected file was touched |
| new C++ gtests | `shadps4_virtual_camera_clock_test` 8/8 and `shadps4_autopress_test` 6/6 (binaries from the feature worktree at the same C++ source; the candidate's tests tree is not built) |
| active runtime | `pc-vr/shadps4.exe` still `E6A5A165…`; no `pc-vr/` file was written |

Evidence: `_out\candidate-tests\independent-check.json` and the `failclosed\` outputs in this
worktree.

## Deferred (headset session in progress)

The user is using the headset. Per instruction, there is **no VR testing and no heavy GPU work**
while that lasts, so these are not run:

- the tests-enabled CMake tree in this candidate,
- the full CTest in this candidate (540/550 in the feature worktree, ten inherited failures,
  documented in `EVIDENCE.md`),
- the classroom benchmark and RenderDoc capture.

They are the missing gates, not passes. The candidate's build hash differs from the feature
build's `B6830480…` because the evidence build was made at `fcaa02f` before the later
comment-only source change; the C++ behaviour is the same.

## Deferred gate commands (run only when the headset is free)

```powershell
$WT = 'M:\Development\Deracine-work\wt-guardrails-candidate'
$PY = 'C:\Users\Arukas\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'

# tests-enabled tree, the two new targets, then the full suite
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root $WT -Tests `
  -Target shadps4_virtual_camera_clock_test
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root $WT -Tests `
  -Target shadps4_autopress_test
& "$WT\build-tests\tests\shadps4_virtual_camera_clock_test.exe"
& "$WT\build-tests\tests\shadps4_autopress_test.exe"
ctest --test-dir "$WT\build-tests" -j 4 --output-on-failure
```

Expect the ten inherited Windows failures listed in `EVIDENCE.md`; anything else is new and must
be reconciled before integration.

## Integration sequence (proposal, no integration performed)

1. `995e84c` contract docs.
2. `0f888e7` + harness fixes (`b6b9807`, `d1bf237`, `9b890d1`, `9863e97`, `9289534`) benchmark
   tooling — behaviour-free.
3. `4b93bd8` frame-count logging and `9deb0bb` RenderDoc trigger — diagnostics, reversible.
4. `d7c7d5f` camera clock — **experimental**; do not land as a production fix until its 60/120 Hz
   ring-safety test exists. The headless benchmark depends on it.
5. `b32b88d` + `fcaa02f` autopress — dev-only, default-off.
6. `de85edf` tests, `818b667` preflight/Move comment.
7. `fac43a3` RenderDoc-image fallback is **not** part of this candidate; decide separately.

Gates: rebase, build, CTest parity, a classroom run on the integration worktree with independent
review of the screenshot and `run.json`, the RenderDoc capture gate (currently blocked), and user
approval for anything that touches `pc-vr/`.
