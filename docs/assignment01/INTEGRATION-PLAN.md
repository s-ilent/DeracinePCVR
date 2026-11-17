# Assignment 01 — proposed integration plan

**No integration has been performed.** This branch (`feature/autonomous-guardrails-2026-11-17`,
worktree `M:\Development\Deracine-work\wt-autonomous-guardrails`) is an isolated candidate. It was
cut from `acda49e`; the active `pc-vr/`, launcher, settings, user config, game data, shared
`_build/bench`, shared `lab` and the recovery store are untouched (see EVIDENCE.md).

## What the branch contains

Fourteen source/docs commits on top of `acda49e`, plus evidence documents:

1. `995e84c` — contract docs and the `AGENTS.md` pointer.
2. `9deb0bb` — exact `7130e9f` RenderDoc trigger hunk.
3. `4b93bd8` — frame count in the frame-stats line (enables measured window duration).
4. `d7c7d5f` — virtual camera clock (experimental; see the safety note below).
5. `b32b88d`, `fcaa02f` — default-off `SHADPS4_DBG_AUTOPRESS` devtool and its disabled-state fix.
6. `de85edf` — two gtest targets for (4) and (5).
7. `0f888e7` — benchmark runner, validator, preflight/postflight, path guard, report/compare,
   schema, reference, tests.
8. `b6b9807`, `d1bf237`, `9b890d1`, `9863e97`, `9289534` — harness fixes found by the first
   end-to-end runs and the independent review (failure cleanup, manifest serialization, unique-FPS
   aggregate, window bound, scene-gate majority, drift vs observation, truncated lines, capture
   lookup).
9. `818b667` — preflight no longer hashes the private shadow config; Move comment corrected.

## Recommended integration order

1. **Contract docs first** (`995e84c`). They change no behaviour, and the pointer makes the
   governance visible to every later agent. Independent of everything else.
2. **Benchmark tools** (`0f888e7` plus the harness fixes `b6b9807`, `d1bf237`, `9b890d1`,
   `9863e97`, `9289534`). Also behaviour-free for the runtime. They can be used against existing
   builds immediately. The validator requires per-window frame counts (`timing.incomplete_stats_lines`
   records unreadable stats lines); it no longer has an observed-duration fallback.
3. **Frame-count logging** (`4b93bd8`) and **RenderDoc trigger** (`9deb0bb`). Both are
   diagnostics-only and reversible. Land them before any performance work so that later claims
   are measured on the new series.
4. **Camera clock** (`d7c7d5f`) — **experimental, prerequisite only; not recommended for
   production yet.** The change removes the guest-present dependency and keeps the "one frame per
   read" bound, so it cannot deadlock. What it does **not** preserve is the old invariant "a frame
   for every two the title presents": the guest keeps a three-entry camera ring, so the safety
   margin is now three read intervals (25–50 ms at 60/120 Hz) instead of three present intervals.
   A title presenting more slowly than ~20 FPS at 60 Hz (or ~40 FPS at 120 Hz) could still find
   the entry it is reading overwritten. The 200 ms timeout in the old code also means the fault
   was a repeated stall, not a permanent deadlock. Before production, instrument camera reads vs
   presents at 60 and 120 Hz and repeat on a worn headset. The headless benchmark depends on this
   change; it is the reason the fix is on the branch at all.
5. **Autopress devtool** (`b32b88d`, `fcaa02f`) — must stay explicit, default-off, and never in a
   production default. It is only needed by the headless benchmark.
6. **Tests** (`de85edf`) — come with the two source changes above.

If the team wants a smaller first step, 1–3 (docs, tools, diagnostics) are independent of the
camera/autopress changes and can land alone. The benchmark scene still needs 4+5 to be reached
headlessly; with a worn headset, 4 is not needed to reach the scene manually (but the deadlock
fix is still the suspected general bug).

## Gates before integration

- Rebase onto the current integration line and re-run the private build.
- Run CTest on the rebased tree; compare against the same tree without these commits. The
  inherited Windows failures observed here are: `shadps4_usbd_test` (`sys/time.h`),
  `shadps4_patch_domain_*` (`jni.h`), `shadps4_memory_patcher_test`/`shadps4_patch_*`/
  `shadps4_preset_test` (`_QUEUE_USER_APC_FLAGS` redefinition against Windows SDK 10.0.26100) and
  `shadps4_gcn_test` (link: `EmitContext::DefineSceneEasu`, `NumberUtils::S10ToSnorm`). Two
  runtime tests fail deterministically in untouched sources:
  `ReadConstSnapshotDiag.IsolationDisabledAfterTestB` and
  `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`.
- Re-run the classroom benchmark on the integration worktree with the same scenario, and have a
  second person/agent review the screenshot and the `run.json`. The one valid cell on this branch
  is `classroom-valid-r9` (39.83 FPS, screenshot similarity 1.0000); it is a single cell on a
  machine at 97–100 % GPU busy, not a performance comparison.
- **RenderDoc gate is open:** no fresh classroom `.rdc` was obtained. The injected attempt
  device-lost before capture, and later attempts device-lost even without a flushed RenderDoc
  log. Do not integrate the RenderDoc runner path as "verified" until a fresh capture exists with
  an extracted image matching the classroom reference and `artifacts.rdc` recorded in `run.json`.
- Confirm the active `pc-vr/` hashes are unchanged, or install a reviewed build only with the
  user's explicit approval. The camera change is **experimental**: do not put it in a
  production-ready recommendation until the ring-safety test described above exists.

## Conflicts expected

- `docs/AUTONOMOUS_ENGINEERING.md` is new; no conflict.
- `AGENTS.md`/`src/AGENTS.md`: this branch adopts the *current* project versions (English, tests
  expected). If the integration line has since changed them, take its version and re-apply only
  the contract pointer.
- `vk_scheduler.cpp`: the RenderDoc trigger and frame-count line may conflict with later
  diagnostic work on the overnight line (the trigger is exactly `7130e9f`).
- `camera.cpp`: the integration line's own camera work (if any) supersedes the clock extraction;
  resolve by keeping the invariant (sensor never depends on guest presents; one frame per read)
  rather than the diff shape.

## Do not integrate from here

- The depth-write pass (`b6b876f`), barrier (`33ebbf8`), foveation-any, `steady=`, 120 Hz pacing
  follow, presentation-size changes, autostick/hand-pose/menu-selection attempts.
- `633d212` or `fc2fb55` as units (sweeping commits).
- `integration/upstream-sync-2026-11` (separate workstream).
