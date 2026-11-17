# Déraciné PCVR — Autonomous Engineering Contract

> **Implementation status on this branch.** The text below is the standing policy as supplied in
> the 2026-11-17 handoff. The executable enforcement it requires (validity validator, protected-
> path preflight/postflight, path guard, provenance manifest) is implemented and tested in
> `tools/benchmark/` on `feature/autonomous-guardrails-2026-11-17`, but it becomes operational
> protection for other agents only after this branch is reviewed and integrated. Until then this
> branch is self-validated evidence, not a production guard.

---
# Déraciné PCVR — Autonomous Engineering Contract

**Status:** Proposed standing contract. An engineering agent must implement its enforcement and tests on an isolated branch before anyone treats it as operational protection.

**Scope:** `M:\Development\Deracine-work\DeracinePCVR`, associated independent worktrees, Windows lab and PSVR/OpenXR tooling. This document describes engineering policy, not authorization to modify protected installations. It is intended to live at `docs/AUTONOMOUS_ENGINEERING.md`, with a short mandatory pointer in the applicable root `AGENTS.md`. Read both root and nested `AGENTS.md` files; if instructions conflict, stop and obtain a decision rather than silently overriding them.

## 1. Mission and outcomes

Deliver **verified, reproducible improvements** to the PS4 Déraciné → PCVR runtime and transferable shadPS4/OpenXR components. Agents may independently inspect source, form hypotheses, implement, build, test, measure and commit in their **assigned isolated worktree**. A lead agent may delegate bounded, non-overlapping work. Report material changes, but do not ask the user for routine engineering decisions within the assignment.

**Performance goals, not substitutions:** 60 FPS first, 90 FPS intermediate, ultimately **120 unique game-rendered frames per second** around **3036×3036 per eye** on Steam Frame with correct rendering, gameplay and stereo. Different resolutions, frame interpolation/reprojection, headset compositor refresh and predicted FPS are separately labelled evidence; none is fulfillment of this target. Do not silently change image quality, MSAA, rendering resolution, frame pacing or the success criterion. Do not confuse a render-target pixel-traffic estimate with actual shaded fragments.

A result is **verified** only if a clean reproducible build and task-appropriate automated/visual evidence support it. “Buildable,” “tested in menu,” “measured once,” and “tested in gameplay” are different statuses. Explicitly report which applies.

## 2. Immutable-in-practice reference points

The recovery report from 2026-11-17 is the initial evidence set, **not live Git state**. Verify all identifiers on entry:

- Source baseline `acda49e`; `recovery/baseline` clean build `E2707640637BE229…`.
- Original shipped executable candidate `pc-vr\shadps4.exe.before-r58`, SHA-256 `46C5269EE48133A7…`.
- Active `pc-vr\shadps4.exe` found after failed session: **experimental**, SHA-256 `E6A5A165183EB9F1…`, **not** an approved known-good installation.
- Independent upstream sync: `integration/upstream-sync-2026-11` at `24b8620`, not merged.
- Experimental depth-pass extraction `recovery/depth-pass` at `b6b876f`; **+2.8% aggregate within a ~10% noise floor** in controlled front-end testing; not a verified optimization.
- RenderDoc trigger extraction `recovery/renderdoc` at `7130e9f`; builds, but the old `.rdc` shows **SIE splash**, not classroom.
- Backup/evidence store: `M:\Development\Deracine-recovery-2026-11-17`, including hashed manifest, reports, Git bundles, logs and binaries.
- Multiple other agents own worktrees (`wt-sync`, `wt-unreal`, `wt-quality`, `wt-handrig*`, etc.). None is disposable.

Preserve the original recovery files and do not rely on commit subjects for provenance. Overnight commits `633d212` and `fc2fb55` mixed unrelated work; **never cherry-pick either whole**.

## 3. Ownership and authority boundaries

**May, inside a newly allocated development worktree and private build/lab directories:** edit assigned source/docs, add tests, execute isolated builds, run offline and GPU tests when the HMD slot is available, collect captures, commit narrow logically reversible changes and arrange independent reviews.

**Must not without explicit approval:**

- Change `DeracinePCVR\pc-vr\shadps4.exe`, `pc-vr\launch.ps1`, `pc-vr\settings.txt`, `pc-vr\user\*`, any active installed runtime or user preferences.
- Modify/delete original game files, game configurations, saves, or the shared `D:\Temp\Deracine` images. Use independent game *shadow/config* paths; record exact source/identity.
- Edit another worktree or agent's dirty files; reset/rebase/clean/cherry-pick into protected branches; merge upstream-sync or other streams; force-push.
- Overwrite shared `_build\bench` defaults, any shared lab harness, or evidence store. Move reusable harness code to a version-controlled, project-owned directory and use private copies for experiments. The recovery agent did repair shared `_build\bench\bench.ps1`; preserve that corrected version as evidence, do not make further implicit shared edits.
- Delete/overwrite `.rdc` captures, PNGs, logs, benchmarks, Git bundles, backups or failed experiment output.
- Run tests concurrently on the GPU/HMD when another worktree holds the slot; do not kill unrelated processes.

Use `git add -- <explicit paths>` and review `git diff --cached` before each commit. Never use `git commit -a`, `git add -A`, or wholesale commits that absorb unrelated work. New commits must identify source/patch provenance and validation in the message or ledger.

**Protection must be real:** a Git branch, shell prompt or text policy does not physically prevent writes by a process with shared Windows filesystem permissions. Use allowlisted paths, fail-closed wrappers, read-only snapshots and before/after hashes; propose OS-enforced isolation separately if available/authorized. Do not change user filesystem permissions or security settings without approval. Explicitly state which barriers are *preventive* and which only *detect* violations.

## 4. Source of measurement truth: two different scenes

The recovery audit established:

- **Light front-end:** approximately 36–40 draws and 225–260 Mpx of target traffic; not the performance benchmark.
- **Classroom with floating main-menu overlay:** ~479–581 draws, ~84–91 passes and ~504–600 Mpx in observed runs. Historical anchor: 532 draws / 561.3 Mpx / 89 passes / 39.6 FPS, but these ranges are *diagnostic signatures*, not an immutable game definition.
- **Actual gameplay:** not proven reached by the recovered headless automation. Do not call the classroom-plus-menu capture gameplay. The presence of the overlay in the exact pre-session benchmark frame remains unknown.
- The existing `.rdc` (`CUSA11919_frame532.rdc`, SHA-256 beginning `479C633580CA2A1D…`) contains an SIE splash at frame 532; it proves the tooling once captured something, **not** the benchmark scene.

For the **existing historical classroom-menu scenario**, reproduce an explicitly pinned test:

- Lab/shadow game `lab\pc-vr-quality` and `D:\Temp\Deracine\CUSA11919-qual` only if still valid and privately copied/owned.
- Guest `r.ScreenPercentage=300`, `r.MSAACount=2`, packed stereo 5760×3240 (2880×3240/eye); `SHADPS4_VR_SCALE=0.5` if required by that test's established presentation path.
- `-Vblank 60` explicitly recorded. `SHADPS4_DBG_AUTOPRESS=1` **explicit, default OFF**; the clean `acda49e` source lacks the hook. Reconstruct it as a dedicated dev-only patch; never pretend the clean baseline reaches the heavy scene by itself.
- Two consecutive 0.5-second samples with `draws >= 400 && target_Mpx >= 500` may serve as a **coarse entrance gate**, followed by screenshot/RenderDoc proof of the actual scene and the overlay state. Retain the known reference screenshot and test its identity via a reviewed image comparison, not metrics alone.
- Warmup 10s, measurement window 40s **of actual elapsed duration** with 80 valid 0.5s windows; fail if timing/sample counts disagree. Record unique FPS and p50/p90/p95/p99 frame times as supported by valid data.

**Crucial:** `xrBeginFrame`, headset present cadence and compositor frames are **not** unique game render frames. The overnight harness incorrectly treated XR-present delimiters as a rendered-frame timeline and reported 120 FPS for a ~41 FPS scene. The authoritative recovered metric is `SHADPS4_FRAME_STATS` rendered-frame windows, independently cross-checked against frame counts and elapsed time. Never auto-select a timing source based merely on the presence of a log marker. Unknown/ambiguous metric provenance invalidates a run.

## 5. Benchmark validity is machine-enforced

Every benchmark cell should have a machine-readable `run.json` (or equivalent) with schema version, timestamp, test scenario ID and scene image evidence, repository worktree path, HEAD SHA, dirty-state manifest, executable SHA-256, launcher SHA (if used), host GPU/driver, game build hash, OpenXR runtime/headset state, guest internal and OpenXR swapchain resolutions **separately**, actual Vulkan sample count, display vblank, env flags and overrides, automation actions, warmup/window boundaries, timing source and samples, draw/pass/Mpx counters, validation-layer status and capture filenames/hashes.

A separate deterministic validator (not the experiment's scoring path) must **reject**:

1. XR/compositor/present cadence presented as game-rendered FPS; time window claiming 40s but actually lasting <1s.
2. Wrong scene, missing/unreadable/mismatched screenshot, or numeric scene gate without visual identity proof.
3. Missing/unrecorded autopress, vblank, render scale, game identity or executable SHA.
4. Mismatch between requested and *actual* game/eye/guest/Vulkan configuration; 60Hz and 120Hz tests inadvertently mixed.
5. Insufficient valid samples, overlapping warmup, incomplete frame times, nonmonotonic timestamps, out-of-bound content drift.
6. Invalid/missing path for any destructive cleanup. Never perform a pathless `Remove-Item`; clean only a private directory verified to be under an allowlisted root, and never on parse failure.
7. Dirty or unrecognized executable and unverified benchmark/source pairing when testing against a pinned reference.

Invalid results get `valid:false` and an explicit reason; cannot enter headline summary tables. Never silently repair bad metadata to make a failing run pass. Preserve raw logs and failure manifests. Unit/negative tests must deliberately feed the validator examples of every prior failure.

A/B comparisons need **identical workload**, a baseline self-repeat to measure noise, interleaved A–B–A–B or A–B–A, independent screenshots and sufficient cells to distinguish gain from noise. Never interpret a 2.8% aggregate as a win against a ~10% noise floor. Use Vulkan timestamps for specific GPU transfer costs, with query capacity and actual sample coverage verified. GPU busy × frame duration is **not** precise per-pass GPU timing.

## 6. Investigation discipline and delegation

Each investigation creates a short durable record: immutable evidence anchors; narrow hypothesis; expected proof; code scope; fallback; run IDs; outcomes; exact next step. Promote only with proof.

If new evidence contradicts prior measurements, **stop dependent conclusions** and reconcile source, scene, FPS provenance, timestamps, sample counts, settings and build identity. Do not erase or rewrite the older result. The failed session chased an incorrect scene conclusion for nine rounds; the first contradictory measurement should have raised an invalidity gate.

After two or three failed attempts on one hypothesis, reassess based on evidence and select a distinct bounded experiment or useful independent task. No endless speculative loops, no unrequested target changes, no off-by-default experimental features silently enabled.

Delegate only when responsibilities and file/worktree ownership are disjoint:

- **Implementer:** source and tests in own worktree.
- **Reviewer/validator:** independently inspects diffs, run metadata, scene captures, Vulkan legality, test failures and claimed benefit; does **not** edit implementer's tree.
- **Integrator:** after approval, promotes reviewed commits to an *isolated candidate* branch, not the user's runtime.

A reviewer may veto; lead must not override evidence. If no independent reviewer is available, status remains *self-validated only* and cannot be represented as independent verification.

## 7. Test, reporting and promotion gates

At minimum: clean-from-source build; applicable CTest (configure tests explicitly, do not call tests skipped by `ENABLE_TESTS=OFF` a pass); unit/negative tests; GPU and Vulkan core+sync validation when relevant; image evidence; headless classroom benchmark where applicable; actual gameplay/headset testing for behaviour that depends on interaction and headset state. A missing gate is an honest **gap**, not a pass.

For every session checkpoint: state branch/worktree/HEAD, working-tree dirt, artifact hashes, scenario and config, tests and result statuses, measured vs estimated metrics, evidence paths, failure hypotheses and next safe action. Keep run metadata together with immutable captures. Never claim asynchronous work will complete after the session unless an actual automation has been set up.

**Candidate promotion requires:** verified provenance, narrow diff, reproducible build, test and screenshot gates, independent review, explicit fallback, documented limitations and **user approval** for changes to `pc-vr/`, production launchers, settings, game data or protected integration branches. Passing a shader unit test or idle menu screenshot alone cannot validate gameplay correctness.

## 8. Standing permission and stopping rules

Inside assigned isolated paths, take initiative: implement, test, debug, pivot to independent tasks, preserve evidence, and deliver useful outputs without waiting for step-by-step approval. Outside those boundaries, halt the affected operation but continue other authorized work. Do not ask the user to babysit routine failures. If hardware is absent, complete offline tests, capture analysis or source-level work and clearly mark blocked hardware gates.

The immediate objective for the first post-recovery session is **engineering guardrails and repeatable diagnostic infrastructure**, not reaching 120 FPS. The depth-pass candidate `b6b876f`, upstream-sync `24b8620`, unverified presentation-size and pacing changes stay out of this assignment's default build.

---

## Appendix — enforcement in this repository

The contract above describes policy. These committed files are the part of it that runs:

- `tools/benchmark/validator.py` — independent validity gate for benchmark runs; rejects the
  ten prior failure modes (compositor cadence as FPS, short windows, wrong/splash scene,
  draw-count-only scene proof, missing provenance, mixed vblank cells, unsafe cleanup paths,
  truncated/nonmonotonic samples, executable/hash mismatch, content drift).
- `tools/benchmark/preflight.ps1` / `postflight.ps1` — hash the active runtime, launcher,
  settings, shared harnesses, recovery evidence and game identity before and after a session; a
  protected change makes the run invalid. This is detection, not prevention: no ACL or permission
  is changed.
- `tools/benchmark/path_guard.py` — allowlist/denylist guard called before the runner's copies,
  deletes and settings writes; fails closed on empty, relative, `..`, out-of-root and
  reparse-point-escaping paths. It is preventive for the harness's own operations only; it is not
  a sandbox for arbitrary tools under the same Windows token.
- `tools/benchmark/runner.ps1` — the pinned, noninteractive classroom session. It writes
  `run.json` with the provenance the contract requires and exits with the validator's status.
- `tools/benchmark/report.py` / `compare.py` — reports only valid runs and never substitutes
  compositor cadence for unique rendered FPS; A/B refuses different workloads, 60/120 Hz mixes,
  content drift, and differences inside the measured noise floor.
- `tools/benchmark/tests/` — negative tests for every prior failure, plus positive controls.

Read `tools/benchmark/README.md` for the exact scenario, commands and limitations.