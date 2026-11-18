# History migration and review-fix record — 2026-11-18

The public repository was rewritten to source-only history (binaries removed from every commit)
and republished as <https://github.com/s-ilent/DeracinePCVR>. This document records how the local
unified integration, including the later M2 headless work, was migrated onto that history, and what
was then changed for the external code review. Nothing here rewrites published history: the replay
and every fix are new commits on top of the published branch.

## 1. Published baseline

| Item | Value |
|---|---|
| Published branch | `integration/deracine-unified-2026-11` |
| Published tip used as base | `cf10f9a5a551bf6d46b7acf9c9c3ed6710d21126` |
| Its pre-rewrite equivalent | `e3c1694f9ec6f70c0fb444b1721082db3b1520fd` |
| Pre-rewrite source-build commit | `9358d7a` → rewritten `6d3971dd60e9` |

Verified before touching anything:

- `git diff e3c1694 cf10f9a5 -- . ':(exclude)pc-vr/shadps4.exe'
  ':(exclude)pc-vr/pkgtool/PkgTool.exe' ':(exclude)pc-vr/pkgtool/LibOrbisPkg.dll'` → **empty**.
- `git diff --name-status e3c1694 cf10f9a5` → exactly three deletions:
  `pc-vr/shadps4.exe`, `pc-vr/pkgtool/PkgTool.exe`, `pc-vr/pkgtool/LibOrbisPkg.dll`.
- The published branch advertises `cf10f9a5`; the local `origin` still points at the parent
  repository, so the fork is configured separately as `fork`.
- Old history exists only in the offline bundle, per the migration record; no pre-rewrite ref or
  tag was created or pushed.

## 2. M2 replay onto the rewritten history

The M2 integration (`88686f5`) was created **after** the rewrite snapshot, so it had no rewritten
commits. It was replayed in an isolated worktree (`wt-unified-pub`) as new commits on `cf10f9a5`,
using the rewritten equivalents of the headless source commits (`ef4c20d2`, `ae6781c7`, `78f0109a`,
`93fade49`, `2c4f0525`, `2b325e82`, `3ce97343`, `c566db78`). Where the original replay had resolved
a lineage conflict or a fix-up, the verified file content was taken from the old tip, so two pairs
of old commits are folded into one new commit.

### Old → new commit map (M2 integration only)

| Old commit | New commit | Subject |
|---|---|---|
| `5b6bea5` | `58331fb` | tools(headless-vr): launch the emulated-PSVR front end without a headset |
| `beeb77f` | `5268a61` | tools(headless-vr): record the verified M1 result and commit |
| `937818d` | `ea9e537` | vr: synthetic Move hands and Move input from the emulated pad |
| `5f0a3d9` | `f1eade6` | input: frame-relative script steps, guest frame counter, and Move overrides |
| `7aa383b` + `265978c` | `ee5357c` | vulkan: scene-gated HMD eye dump (adapted; the omitted `TakeHmdDumpRequest` definition is folded in) |
| `fc33715` + `bac8476` | `baeb74a` | emulator: test-only timed quit and clean presenter shutdown (lineage-only include removed) |
| `b41dbdb` | `e8a3933` | vr: keep the scripted aim on the mirrored hand |
| `c28bef8` | `7e941ad` | move: test-only `SHADPS4_DBG_MOVE_TRACE` read trace |
| `0c90874` | `c0059aa` | test(benchmark): headless gameplay scene gate, profile and unit suite |
| `99085ad` | `dba2774` | feat(benchmark): dedicated headless gameplay cell |
| `73c4f57` | `7a39c9b` | docs(integration): record the headless M2 integration |
| `88686f5` | `a6946bd` | docs(headless): make the M2 path free of private `_build` dependencies |
| `647b87b` (merge commit) | — | the replay is linear on `cf10f9a5`; the merge structure is not reproduced because the content is |

### Equivalence check

`git diff --name-status 88686f5 a6946bd` → exactly the three removed binaries, **no other path**:
the migrated source tree is byte-identical to the old M2 checkpoint outside the intentionally
removed binaries. `git merge-base --is-ancestor cf10f9a5 a6946bd` succeeds, so publication is a
fast-forward.

## 3. Review fixes on top (all new commits)

| Review item | Commit(s) | Result |
|---|---|---|
| P0 benchmark process ownership | `6ab2b2c`, `b4c853b`, merge `1148b7b`, runner wiring `9a8a474` | cleanup stops only positively owned processes; no name sweep anywhere in `runner.ps1` |
| Private game-shadow enforcement | `909c28e`, merge `038aebb`, `new-shadow-manifest.ps1`, runner wiring `9a8a474` | `-ShadowRoot` mandatory; realpath/manifest/eboot/blocklist proof before `engine.ini`; preflight fail-closed |
| Frame-numbered script lifetime | `2dfe983`, merge `19b706a` | both time domains, stall watchdog, owned worker joined before OpenXR/controller teardown |
| Scene identity vs colour | `20771b5`, merge `eaf3c1a` | `image_validation.py` RGB gate, separate from `scene_signature.py` |
| Frame-time terminology | `34d0d72` | `window_avg_frame_ms` + statistic metadata; validator/report/schema/README; legacy accepted |
| Gameplay band vs renderer state | `2fae2b8` | two-lab M2 status; draws-tight, Mpx/passes-loose band |
| Unreal inspector findings | (none) | delegated to the Atlas agent; no behaviour change |
| Preserve renderer correctness | (none) | MSAA/#5433/#5444 rules untouched |

## 4. Final state

Source content at the documentation commit (the published tip is this plus documentation commits):
`5e618d1` — 25 commits since `cf10f9a5`, 59 files, +7,710 / −161.

Validation on the final tree:

- emulator build `exit 0`; `build\shadPS4.exe` 73,333,760 B,
  SHA-256 `D8FA1D342D635EC258E8B16D9094832C0C13FDE26B29CDA9DA9C12B39C50114B`;
- CTest: **790 tests, 786 passed, 4 failed** — the same four inherited failures (gcn and usbd
  `_NOT_BUILT`, `ReadConstSnapshotDiag.IsolationDisabledAfterTestB`,
  `BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot`); the 20 scripted-input tests pass;
- benchmark Python suite: **115 tests pass** (scene gate, RGB gate, shadow policy, terminology,
  validator/path guard);
- process-ownership PowerShell suite: **13/13**.

No live gameplay or headset run was performed in this phase; GPU/headset acceptance remains a
separate gate. The benchmark's classroom path now also requires a one-time shadow manifest, which
is deliberately not created by the integration agent (the private lab was not modified):
`pwsh -File tools\benchmark\new-shadow-manifest.ps1 -ShadowRoot <private shadow> -Verify`.
