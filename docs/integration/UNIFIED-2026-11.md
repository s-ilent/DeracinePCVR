# Déraciné PCVR — unified integration report (2026-11-18)

## A. Unified integration branch

| | |
|---|---|
| Branch | `integration/deracine-unified-2026-11` |
| Tip | source content `647b87b` (headless M2 merge); later commits are documentation only |
| Source commit of the shipped binary | `647b87b` |
| Base | `acda49e` (`recovery/baseline`) |
| Worktree | `M:\Development\Deracine-work\wt-unified` |
| Commits since base | 86 (six top-level merges plus two workstream merges) |
| Diff since base | 158 files, +23,455 / −331 |
| Executable | `wt-unified\build\shadPS4.exe`, 73,319,936 bytes |
| SHA-256 | `1537F061CBE83915D9A1B080420191C5F10BCE9B73F2FE931EA657C25B2AEBCB` |

The branch contains no commit from the contaminated runtime lineage: `633d212`, `8638928`,
`fc2fb55`, `194958b`, `33ebbf8`, `517213c`, `de9c78c` are verified non-ancestors, and no
`pc-vr/shadps4.exe`, `settings.txt`, `settings.json` or user config is changed. The only `pc-vr/`
delta is the compat work on `launch.ps1` and the two tracked `CUSA11919.xml` patch files.

### Reproducible build

```powershell
git -C M:\Development\Deracine-work\DeracinePCVR worktree add `
    -b integration/deracine-unified-2026-11 M:\Development\Deracine-work\wt-unified acda49e
# externals are not submodules (src/.gitmodules is only a fetch list); copy an existing pinned set
robocopy M:\Development\Deracine-work\DeracinePCVR\src\externals `
         M:\Development\Deracine-work\wt-unified\src\externals /E /MT:24 /NFL /NDL /NJH /NJS /NP
git -C M:\Development\Deracine-work\wt-unified\src\externals\magic_enum describe --tags   # must be v0.9.7

$env:NUMBER_OF_PROCESSORS = 8
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-unified -Target shadps4
# tests live in a separate tree because ENABLE_TESTS=ON replaces the emulator target
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-unified -Tests
# two inherited Windows-only targets cannot compile under SDK 10.0.26100; keep going
cmake --build M:\Development\Deracine-work\wt-unified\build-tests --parallel 8 -- -k 0
ctest --test-dir M:\Development\Deracine-work\wt-unified\build-tests -j 8
```

Toolchain: VS 2022 Build Tools 14.44 + LLVM/clang-cl 23.1.5 + Ninja, Release, CPU only.
`build.ps1` already carries the three local quirks (`-DHAVE_ENDIAN_H:INTERNAL=`, pinned
`magic_enum`, separate tests tree). A clean emulator build is ~4 min on this machine.

## B. Integration matrix

Full accounting of every branch and worktree is in [`INTEGRATION-MATRIX.md`](INTEGRATION-MATRIX.md).
Summary over 45 local branches plus the 5 integration process branches and 33 worktrees:

- Integrated: upstream-sync, guardrails, color fix, frustum fix, openxr native resolution,
  render-quality, hand-rig-live, compat (two commits), crossfork diagnostics/coverage,
  depth-pass (opt-in), headless M1 tooling.
- Integrated with modifications: upstream-texture-staging (guards carried, duplicates avoided),
  deracine-version-compat (contaminated ancestry excluded), psvr-projection-geometry (docs/tests
  restored; source identical to the frustum fix), depth-pass/barrier (default flipped to opt-in),
  headless M2 (lineage-only RenderDoc scene gate and frame_timeline include removed; everything
  test-only and off by default; single-lab verified).
- Already superseded or already present: candidate/guardrails, three Unreal lineages,
  recovery/renderdoc, fix/libc-cxa-hooks, audit/stage1-host-guest, test/v0.21.0-plus-fixes,
  merge/eval-* and merge2/* (evidence only).
- Experimental, retained separately: image recycler (both branches), diag/lut-dump, wt-perf's
  frame-count duplicate, wt-lut-* bisect/revert, M3 validation work.
- Rejected: `633d212` as a unit, `fc2fb55` as a commit, foveation-any, `steady=`, vblank pacing,
  the `render_percent=210` preset, LUT fixes, autostick / hand-pose / pad-automation defaults.

## C. Build and regression evidence

Executable: see §A. Tests tree rebuilt from the same tree; final CTest:

```
776 tests, 772 passed, 4 failed, 1 skipped (HostOverride.ActiveExactHostMatchFromJsonFile, inherited)
FAILED:
  69  shadps4_gcn_test_NOT_BUILT                     (inherited link failure)
 108  ReadConstSnapshotDiag.IsolationDisabledAfterTestB (inherited)
 463  BachataRuntimeClient.ValidatesSocketPathUnderRuntimeRoot (inherited)
 465  shadps4_usbd_test_NOT_BUILT                    (inherited POSIX sys/time.h)
```

Baseline comparison, same machine and toolchain:

| Tree | Tests | Passed | Failed | Notes |
|---|---|---|---|---|
| `acda49e` (batch 1 of this integration) | 542 | 532 | 10 | documented inherited Windows set |
| unified `fc01c02` (before M2) | 770 | 765 | 4 | +228 tests, 0 new failures, 6 inherited failures fixed |
| **unified `647b87b` (with M2)** | **776** | **772** | **4** | +6 `ScriptedTimeline` tests; same four inherited failures |

Fixed relative to the documented baseline: `shadps4_memory_patcher_test` (compat `1f5aaff`) and
the five patch-family targets (`patch_repository`, `patch_state`, `patch_domain`, `patch_session`,
`preset`), which now link because `ntapi.cpp` was added to their source lists (`fc01c02`). The two
remaining unbuildable items are the POSIX-only usbd test and the jni-dependent
`patch_domain_shared` support library, both inherited and untouched.

New suites all pass: `VrProjectionMath` (1031-line suite restored), `VrPresentationSize` (10),
`ColorContract` (7), `QualityProbe` (3), `ImageCoverage` (19), `MipChain` (6), `AutoPress` (6),
`VirtualCameraClock` (8), `ScriptedTimeline` (6, from M2), Unreal inspector/report/snapshot (23),
hand retarget/live (18), plus the compat patch-manager cases. The headless gameplay benchmark's own
suite (`tools/benchmark/tests`, standard-library Python) is **45/45**, including the 14 gameplay-gate
cases; it runs without a GPU.

**Validation limitations (explicit):** no emulator, game, SteamVR or GPU run was performed. The
HMD slot was held by another agent (`wt-crossfork-int`) for the whole integration, and the project
rule is that launching the game/SteamVR requires explicit permission. Therefore PSVR/OpenXR
initialisation, stereo presentation on hardware, audio/SNDX in-game behaviour, frame statistics on
the classroom and colour on the headset are **not** validated by this report; only compilation and
offline tests are. The executable hash is build-specific (PE debug GUID); compare code, not hashes.

## D. Functional comparison — live runtime vs unified branch

Live runtime of record: `integration/r58-presentsize` @ `de9c78c` (the checked-out branch of the
active `pc-vr/` folder; the deployed binary there is `E6A5A165…`, untouched).

| Function | Live runtime `de9c78c` | Unified `647b87b` | Why |
|---|---|---|---|
| Déraciné launch path, OpenXR presentation, stereo, PSVR services, Move tracking, audio | yes (base acda49e + later) | **yes** (same base lineage) | Nothing removed from the base. |
| Resolution controls: `SHADPS4_TITLE_EYE_WIDTH`, `SHADPS4_VR_SCALE`, `render_percent` | yes | **yes** | Preserved unchanged. |
| Runtime-native presentation size (`SHADPS4_VR_RESOLUTION`, wall-size 3036×3036 target) | yes, default = runtime recommendation | **yes, same default**; `guest` and the legacy scale still selectable | Carried as `5dfaf94`. |
| Launcher `present=` control | yes | **yes** (`launch.ps1` only; `settings.txt` untouched) | Carried as `6fc3500`. |
| Colour double-encode fix | not on `de9c78c` (on `wt-color`) | **yes** + tests | `2b1209b`. |
| Eye-frustum recovery | yes (via `632306a`) | **yes** | `6112917`. |
| Projection-math guard suite + geometry doc | yes (in `de9c78c`) | **yes, restored** | `9358d7a`. |
| Team benchmark harness, contract, unique-FPS gate, RenderDoc trigger | partly (mixed into `633d212`) | **yes, reconstructed** from the guardrails branch | `59ccc93`. |
| Frame-count logging for unique-FPS windows | yes (`633d212`) | **yes** | `4b93bd8`. |
| Virtual camera clock | yes (`633d212` WIP) | **yes, labelled EXPERIMENTAL** | `d7c7d5f`; ring-safety test still missing. |
| Test-input injection (`SHADPS4_DBG_AUTOPRESS`) | yes, default off | **yes, default off** | `b32b88d`/`fcaa02f`. |
| Depth-write representation copy | **default ON** (`fc2fb55`) | **opt-in only** (`SHADPS4_DBG_DEPTH_PASS=1`) | A-B-A-B did not reproduce the gain. |
| Barrier inside pass | ON (`33ebbf8`) | **opt-in only** (`SHADPS4_DBG_BARRIER_IN_PASS=1`) | +2.6 % at 400 %, no gameplay proof. |
| `633d212` mixed diagnostics (frame timeline, alias trace, preserve tracker, HMD panel probe, foveation) | present | **absent** (reconstructed pieces noted above) | Diagnostics for closed investigations; mixed unreviewable commit. |
| `7e90afb` foveation-any, `steady=`, `ad7238d` vblank pacing, autostick/hand-pose/menu-walk | present | **absent** | Measuring aids / unverified pacing / dev input experiments. |
| Upstream texture-cache & memory fixes (#5433/#5444/#5453/#5495/#5599 + re-fetch + view clamp) | **absent** | **present** + tests + MSAA guard | Merge `e1a1dc2`, guard `984849e`. |
| Image coverage optimizer + RENDERSTAT counters | absent | **present, opt-in** (`SHADPS4_DBG_COVERED_REFRESH`, `SHADPS4_DBG_OVERLAP`) | Extracted from the cross-fork branch. |
| Image recycler | absent | **absent** (retained separately) | Quarantined. |
| Unreal inspector / snapshots / report export / hand rig / retargeter | absent | **present** + 41 tests, guest read-only | `73685be`. |
| Déraciné version compatibility, SNDX Expected-byte gate, honest launcher report | absent | **present** + tests | `71baaf1`/`35fc0e2`. |
| Headless launch / synthetic Move hands / scripted entry / timed quit | in progress on feature branches | **present, all opt-in and off by default** | M2 merge `647b87b`; see §G. |
| Dedicated headless gameplay benchmark (gate, band, unique FPS, save pin) | absent | **present** (`tools/benchmark/headless-gameplay.ps1` + `gameplay_gate.py` + `reference/scene-gameplay.json`) | New infrastructure; live cell still to be run. |

## E. Integration report — conflicts, risks, remaining steps

### Conflicts actually resolved
1. `src/tests/CMakeLists.txt` (presentation merge): both branches appended independent gtest
   target blocks at the same anchor. Kept all Unreal/hand blocks and the presentation-size block.
   Nothing semantic disagreed.
2. Inside the presentation workstream: `blit_helper.cpp` (199 lines) — kept the salvaged
   `CopyColorToDepth32` with its recovery-review fixes and added the `RenderAttachment.image`
   field the barrier commit needs; dropped the quarantined timestamp-reporting variant.
3. Inside the compat workstream: none (the two cherry-picks applied cleanly; the only overlap,
   `tests/CMakeLists.txt`, auto-merged).
4. Upstream-sync vs diagnostics: the previously documented `ExpandImage` conflict does not arise on
   this base because the diagnostics were re-derived without `633d212`'s tracing. The coverage and
   #5444 guards are reconciled explicitly (see §F).

### Risks carried forward
- **Camera clock** (`d7c7d5f`) changes guest-visible timing. It is the headless-harness
  prerequisite and is labelled experimental; the 60/120 Hz ring-safety test does not exist.
- **Presentation size** defaults to the runtime's recommendation (as the live runtime does), not to
  the title's guest size. It was validated by the single-run +15 % measurement on the runtime
  lineage, not by repeated gameplay cells.
- **Depth pass and barrier** are code-complete and validation-clean in earlier captures but opt-in;
  they must not be promoted without a gameplay A/B.
- **MSAA guard** corrects a real content-loss path introduced by the plain #5444 port; it is
  compile/unit-level verified, not GPU-verified.
- `launch.ps1` and the two CUSA11919 patch XMLs change the tracked runtime configuration. They are
  not deployed; the active `pc-vr/` was not written.

### Remaining steps before safe deployment
1. Rebuild from `647b87b` on a clean machine and confirm the same test result (documented above).
2. Run one **headless gameplay cell** with `tools/benchmark/headless-gameplay.ps1` inside the GPU
   lock (first live use of the new benchmark), then repeat it from a **second independently
   constructed lab**. Until that second lab passes, M2 and any gameplay cell are single-lab
   evidence only.
3. On a worn headset, with permission: VR smoke test — PSVR/OpenXR init, both eyes, tracking,
   audio; confirm the M2 switches (all unset in a normal session) change nothing; then the
   classroom benchmark for unique rendered FPS. Do not use compositor cadence.
4. A/B the presentation-size default against `SHADPS4_VR_RESOLUTION=guest` (colour/scale, per-eye
   imageRect and swapchain) and record repeated cells.
5. Instrument camera reads vs presents at 60 and 120 Hz before promoting the camera clock.
6. Gameplay A/B for the depth pass and the in-pass barrier; keep them opt-in until reproduced.
7. RenderDoc capture gate (a fresh classroom `.rdc` with a matching extracted image) before the
   harness's capture path is called verified. The gameplay image reference is still uninstalled
   (candidate exists; needs visual review).
8. If the recycler is ever promoted: matched-scene cells + pixel A/B for Review finding R3.
9. Publication: no push was performed. The branch is committed and ready; coordinate with the
   repository-history cleanup agent (history rewriting in progress) before publishing.

## F. Explicit reconciliation with `integration/upstream-texture-staging`

Handover asked for this workstream to be incorporated without duplicates. Verified facts:

- The branch is based on `1a53ceb`, which sits on the **live runtime lineage** (`de9c78c`), so it
  cannot be merged wholesale. Its nine port commits map 1:1 onto commits already integrated from
  `integration/upstream-sync-2026-11`: `a487b11`↔`7ec75b2` (#5453), `42c6ba4`↔`24b8620`, `3e97a0c`↔
  `09c7ecc` (#5495), `b6f90d2`↔`6be27a6` (#5599), `c437dc1`↔`e94f26d` (#5433), `79379ce`↔`8ccb420`,
  `2f6464b` (cherry-picked as `2e9eb1a`), `0c89560`↔`4610df7` (tests). No duplicate implementation
  was introduced.
- **Guard 1 (coverage):** `covered` is derived from `carries_contents`; when the copy is skipped,
  the upload skip collapses to "skip nothing", enforced by `ASSERT(carries_contents ||
  covered.CoversNothing())`. This was carried through the cross-fork extraction (`d81283b`) and the
  coverage is opt-in.
- **Guard 2 (multisampled sources):** the plain #5444 port (`9af68f2`) gates the copy on
  `SafeToDownload()` alone, but `RefreshImage` returns immediately for `num_samples > 1` without
  clearing `Dirty`, so `SafeToDownload()` is false for every MSAA image and the copy — which ran
  unconditionally before #5444 — was being dropped. `984849e` restores it:
  `carries_contents = SafeToDownload() || num_samples > 1`, with `GpuModified` inherited from the
  same decision. This is the staging branch's `048303c` second guard.
- **#5433 propagation:** `ResolveOverlap` marks the parent `GpuModified` after `CopyMip`
  (`2e9eb1a`), so a later expansion under #5444's gate cannot drop the subresource #5433 preserved.
- Recycler: zero recycler code in the unified tree; the counters exist but are never called and
  read 0, and are documented as such.
- Image-aliasing / depth / LUT checks: offline tests cover the mip-chain, subresource matching,
  image coverage and colour-contract paths and pass. Runtime aliasing, depth preservation and LUT
  behaviour were **not** re-measured (no GPU work); the LUT investigation remains closed and nothing
  LUT-related is enabled.

## G. Headless gameplay M2

Handover added `feature/headless-vr-harness` and `feature/headless-vr-scene` @ `a21109c` (M2:
synthetic Move interactions, menu traversal and gameplay entry with no HMD). Both sit on the live
runtime lineage, so only their **committed checkpoints** were cherry-picked onto the unified base
(merge `647b87b`); neither branch nor its worktree was modified, and the in-flight validation matrix
was not touched.

- Integrated: `tools/headless-vr/` (headless launcher, GPU lock, M1 rules) and M2's synthetic
  tracked hands, Move-from-pad input, frame-relative scripted timeline (+6 tests), scene-gated HMD
  eye dump, test-only timed quit / clean presenter shutdown, aim fix and Move read trace.
- Adaptations: the HMD dump commit depended on the lineage's RenderDoc scene gate and
  `Tally::PeekWindow()`; the scene gate is **not** imported (this tree keeps the time-based
  trigger), `PeekWindow()` was added locally as a non-consuming read, and a lineage-only
  `frame_timeline.h` include that the 3-way merge carried in was removed.
- Opt-in only: `SHADPS4_VR_HANDS`, `SHADPS4_VR_MOVE_FROM_PAD`, `SHADPS4_INPUT_SCRIPT`,
  `SHADPS4_DBG_AUTOPRESS`, `SHADPS4_EXIT_AFTER`, `SHADPS4_CLEAN_EXIT`,
  `SHADPS4_DBG_DUMP_HMD_AT_SCENE`, `SHADPS4_DBG_MOVE_TRACE` are all off/unset by default; a normal
  headset session sets none of them.
- Dedicated benchmark (alongside the classroom one): `tools/benchmark/headless-gameplay.ps1`,
  `tools/benchmark/gameplay_gate.py`, `tools/benchmark/reference/scene-gameplay.json`,
  `tools/headless-vr/input/gameplay-entry.txt` (the committed M2 sequence) and
  `tools/headless-vr/save-slot/` (manifest + checksum-verifying provisioning). Gate: draws ≥ 800;
  band: draws 1500–2300, Mpx 550–750, passes 35–55, ≥ 90 % of the window inside; unique FPS =
  total frames / total measured time. The menu cannot pass the gate.
- Provenance: `tools/headless-vr/HEADLESS-M2.md` records the procedure, evidence and limits. The
  M2 gameplay workload was measured as **1866–1900 draws, 620–720 Mpx, 42–46 passes** per frame
  (menu: 465–500 draws).
- Reproducibility: **single lab only**. The M2 entry was proven in one independently constructed
  lab; a second lab has not passed, so no cell from this infrastructure is presented as generally
  reproducible yet. No emulator was launched during this integration and the GPU lock was not taken.
- Not included: M3 renderer validation, the image recycler, the LUT investigation.
