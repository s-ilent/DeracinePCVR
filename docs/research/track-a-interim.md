# Track A — recycler validation, interim measurement record

Candidate under test: `integration/cross-fork-renderer`, worktree `wt-crossfork-int`,
`build/shadPS4.exe` **SHA-256 `7B658961FCE03E367C7CC02FDC278F2B2CB71EB90032944CF76BCD5992A5357F`**
(source `7a2b042`; the recycler is **off** unless `SHADPS4_DBG_IMAGE_RECYCLER=1`).

## Why these cells are headless

Since 2026-11-18 01:07 the workspace is under the user's HEADLESS-ONLY directive
(`_build/team/HEADLESS-ONLY.md`): no SteamVR, no OpenXR, no headset reservation for routine work.
Every cell below uses the verified headless recipe (`SHADPS4_VR=1 SHADPS4_OPENXR=0
SHADPS4_VR_DEMO=0`, autopress, private portable lab), which reaches the same classroom front end
with both eyes at 2880×3240 and **zero** headset-worn transitions. No HMD was involved, so none of
these numbers is an in-headset claim.

## What is measured so far

Front-end phase reached in every cell (draws ≈ 460–483, Mpx ≈ 470–485, passes ≈ 62–65 — the
classroom front end; arms within ~4 % of each other on draws/Mpx, inside the documented 5 % gate).
Counters are per second from `RENDERSTAT` (`SHADPS4_DBG_OVERLAP=1`); per-frame figures divide by
that cell's own median fps.

| cell | arm | samples | fps p50 | draws / Mpx | image creates / s | creates **per frame** |
|---|---|---|---|---|---|---|
| `hl-a3-1-3` | **on** | 40 (40 s held) | 57.1 | 482.7 / 484.8 | 98.0 | **1.72** |
| `hl-a2-1-1` | on | 3 | 58.2 | 465.7 / 478.3 | 45.3 | 0.78 |
| `hl-a2-0-2` | **off** | 4 | 49.4 | 454.2 / 466.9 | 136.3 | **2.76** |
| `hl-a2-1-3` | on | 4 | 43.9 | 478.0 / 482.8 | 50.5 | 1.15 |

Reading it, and what it does not support:

- **Image creations per frame fall by roughly 2–4× with the recycler on** (0.78–1.72 versus 2.76).
  That is the direct, attributable effect the review predicted, and it is the number to trust here.
- **No frame-rate claim.** The headless front end runs 44–58 fps with the GPU 50–70 % busy and the
  spread between cells of the *same* arm (44–58 fps) is larger than any on/off difference. The
  earlier VR-capable frames were 60 Hz-capped for the same reason. FPS is reported for completeness
  and is **not** interpreted.
- **These are not yet accepted cells.** The `hl-a2` cells have 3–4 samples each because the M1
  launcher exits as soon as it has proved the front end; only `hl-a3-1-3` has a 40 s window. A
  matched accepted triple (on → off → on, ≥ 20 front-end samples each) is what the acceptance rules
  in [gameplay-benchmark-procedure.md](gameplay-benchmark-procedure.md) require, and it has *not*
  been completed: the GPU lock was held or a compiler was running in every window I could take, and
  taking it from another workstream is exactly what the protocol forbids.
- No validation errors and no device loss appeared in any cell above.

## The accepted triple: one command, when the machine is quiet

```bash
bash /mnt/m/Development/Deracine-work/_build/team/gpu-lock.sh run \
  --owner wt-crossfork-int --kind headless \
  --reason "headless A/B/A recycler accepted cells" --ttl 3600 \
  --require-quiet --quiet-wait 1800 -- \
  pwsh.exe -NoProfile -File M:/Development/Deracine-work/_build/crossfork/headless-ab.ps1 \
  -Exe M:/Development/Deracine-work/wt-crossfork-int/build/shadPS4.exe \
  -Tag hl-a5 -WindowSeconds 40 -MinSamples 20
```

It runs the three arms, rejects an arm whose log has fewer than 20 front-end samples (single-attempt
sessions, so a cell can never mix two sessions), copies each log to
`_build/headless/runs/<label>/shad_log.txt`, and then:

```powershell
pwsh -File _build/crossfork/analyse-headless.ps1 `
  -Logs "_build/headless/runs/hl-a5-1-1/shad_log.txt,_build/headless/runs/hl-a5-0-2/shad_log.txt,_build/headless/runs/hl-a5-1-3/shad_log.txt" `
  -Labels "ON-1,OFF,ON-2"
```

## Why the GPU could not be taken

Between 02:03 and 02:30 the lock was held by `lead` (headless M2 aim) and `scene-eng` in rotation,
with `clang-cl`/`ninja` builds running between holds; `gpu-lock.sh --require-quiet` refused every
one of my attempts (exit 7) or my waiter lost the slot to theirs. That is the protocol working: two
other workstreams are actively using the machine. Nothing was taken from them and nothing of theirs
was touched.

## Still open (unchanged by the above)

1. The accepted matched triple above (allocation/CPU saving; **not** an fps claim).
2. Gameplay classroom (needs a physical session; the headless path cannot enter gameplay yet — M2
   scripted Move movement is another workstream's milestone).
3. Pixel A/B for R3, in one scene, both arms — `SHADPS4_DBG_IMAGE_RECYCLER=0|1` dumps compared with
   `_build\bench\capture-compare.py`.
4. RenderDoc scene-gated capture: `SHADPS4_DBG_RDOC_CAPTURE_SCENE=400,500` on the headless recipe
   (RenderDoc is explicitly allowed headless by the directive) — not yet attempted.
5. Recycler default stays **off** until 1, 3 and the gameplay gate pass.
