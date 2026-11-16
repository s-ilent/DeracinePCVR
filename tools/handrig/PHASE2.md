# Phase 2 — live finger control: status and procedure

Branch `investigation/hand-rig`, worktree `M:\Development\Deracine-work\wt-handrig`. Step 3 of the
Phase 2 brief is implemented and tested; steps 1, 2, 4 and 5 need the running game and are specified
below so the next session can execute them without re-deriving anything.

## What is done (Step 3 — retargeting mathematics)

`src/src/core/unreal/hand_retarget.{h,cpp}` — pure math, no engine or guest dependencies:

- `HandRetargeter::ConvertOrientation` — OpenXR (right-handed, Y-up) → Unreal (left-handed, Z-up).
  The axis map is a reflection, so the conversion goes through a 3×3 matrix (`M·R·Mᵀ`) and back to a
  quaternion; the result is always a proper rotation of the same magnitude.
- `Calibrate(xr_rest)` — per-joint offset `C = reference_world · Convert(xr_rest)⁻¹`, measured once
  from a rest capture. This absorbs axis conventions per joint and removes the need for any
  left/right mirror special case.
- `Retarget(xr_live, hand_world, out_local)` — world target `Convert(xr_live)·C`, then local
  `parent_world⁻¹·target_world`; the wrist is *not* taken over (the game keeps owning the hand bone,
  `hand_world` anchors the digit roots). Optional per-joint bend clamp.
- `Blend(animated_local, target_local, weight, out)` — slerp between the game's evaluated pose and
  the tracking pose; weight 0 returns the game pose untouched.
- `HandRetargeter::InteractionWeight(desired, grab_active)` — the override yields (weight 0) while a
  grab is active, so the authored `Item_grasp` poses and the `GrabPoint` socket stay authoritative.

Important property, measured by the tests: local rotations are parent-relative, so applying the same
world rotation to every joint of a chain produces *no* local bend. OpenXR joint orientations are
absolute, which is the right input; converting them to additive per-joint angles would be wrong.

Tests (`src/tests/core/test_hand_retarget.cpp`, 8 cases, all passing):
basis conversion preserves angle; rest capture reproduces the reference pose; an isolated index bend
moves only that digit; a fist bends every digit; an impossible 170° bend is clamped to 90°; blending
interpolates 0 / 0.5 / 1; the left rig produces mirrored magnitudes; grab forces weight 0.

The emulator target links with the retargeter in the CORE list; only `HandRetargeter` is not yet
instantiated by anything because the live writer does not exist (see Step 4 blockers).

## Step 1 — live hand component identification (needs the game running)

Use the read-only inspector that already exists in this branch:

1. Launch the hand-rig build (see commands below), reach a scene.
2. **Ctrl+F10 → Debug → Unreal objects → Discover Unreal objects**.
3. *By name* `c0000` and `c0100`: the hand actors' components. Expect
   `SkeletalMeshComponent` objects whose names follow the actors' component names
   (`SkeletalMesh`, `HandPositionScene`, …), plus the `c0000_BP_C` / `c0100_BP_C` actor classes'
   default objects.
4. *By class* `SkeletalMeshComponent` (633 exist in the package data; only a handful are live),
   `AnimInstance`, `c0000_AnimBP_C`, `c0100_AnimBP_C`.
5. For each component found, record: object address, `Outer` (the actor), the `SkeletalMesh`
   reference (must resolve to `c0000` / `c0100`), the skeleton (must resolve to `c0000_Skeleton` /
   `c0100_Skeleton`), and the LOD index (both meshes have a single LOD, so LOD 0 is the only one).
6. **Export report** — the JSON captures the discovery addresses, class histogram and the matching
   objects in one file; attach it to the session notes.
7. Semi-transparency warnings: the game may spawn both hand actors twice (left/right variants of the
   same mesh) or swap meshes via `SetMeshType`. Confirm by toggling `swap_hands` in
   `pc-vr/settings.txt` and re-running discovery; also watch for `c0000_CD` (a separate mesh asset
   that exists in the pak).
8. Validate identity and lifetime, never reuse stale addresses: re-run discovery after a scene
   transition and compare; the inspector's `IsLive()` marks recycled slots.

## Step 2 — animation evaluation trace (needs the game running)

Structures to locate, in UE 4.18 order (all reachable from the component object once found):

| Structure | What it is | How to confirm |
|---|---|---|
| `SkeletalMesh` (component field) | asset reference | resolves to `c0000` / `c0100` |
| `AnimScriptInstance` | the `UAnimInstance` (class `c0000_AnimBP_C`) | object class name matches, and its class has `RefHandAnimState` |
| `ComponentSpaceTransforms` | `TArray<FTransform>`, `Num == bone count` (79) | scan the component for an array whose count equals the skeleton's bone count and whose elements are unit quaternions (stride `0x30`) |
| `BoneSpaceTransforms` | `TArray<FTransform>` | second array with the same count and stride |
| `CachedComponentSpaceTransforms` | previous-frame copy | same shape; useful to see the write window |
| GPU skin matrices | `FSkeletalMeshObject`/`GPUSkinVertexFactories` bone matrix array (`FMatrix44f`, stride `0x40`) | count equals bone count, values are affine matrices |

Timing evidence to collect (read-only, no writes):

- Read `ComponentSpaceTransforms` twice a few frames apart while the hand idles: the array changes
  every frame (animation runs) even when nothing visibly moves.
- To find the write window without writing: locate the GPU skin matrix array and compare it against
  the component-space transforms of the same frame — the skinning matrices are derived from the
  component-space pose *after* the animation update, so the safest override target is the
  component-space array **after** the animation update and **before** matrix derivation.
- Confirm same-frame ordering by checking whether a change in the component-space array is visible in
  the GPU matrix array of the next frame (it must be, if the window is right).

If the array cannot be located safely, the fallback is the animation-node path: the AnimBP keeps 137
`PoseByName` nodes, and `RefHandAnimState`/`RefIdleGrabRate` select them — driving those inputs would
give authored poses but not continuous tracking, so it is a fallback for grips only, not for Step 4.

## Step 4 — minimal live override experiment (blocked on a guarded writer)

The Phase 1/2 branch is deliberately **read-only**: there is no guest-memory write path, no
game-thread suspension, and the Phase 2 brief forbids forcing direct writes without a safe window.
Prerequisites before the one-finger experiment:

1. A guarded writer: explicit experimental flag (e.g. `hand_override=1` in `pc-vr/settings.txt` or a
   devtools toggle), default off, refusing to write when the flag is off or discovery is not live.
2. A game-thread-safe application point: the write must land after the animation update and before
   skinning matrix derivation. Candidate hooks: the emulator's per-frame point where the guest
   signals frame end, or a hook on the component's pose update once its address is known. Verify
   with the two-read test above before enabling writes.
3. Validation loop: on an idle hand, replace one joint (middle proximal or distal) with a fixed
   rotation; assert (a) that finger deforms, (b) other fingers and the wrist are unchanged, (c) the
   override survives animation updates for N frames, (d) toggling the flag off restores the authored
   pose within one frame, (e) grabbing still works (override weight 0 by `InteractionWeight`).

Deterministic test input: rotate Middle proximal by +60° about the hand's local Y, then +60° about
local X, then back to zero, one second each; the retargeter's `Blend` path can be exercised with
weight 1/0.5/0 without any OpenXR session.

## Step 5 — preserving gameplay animation (design, unblocked once Step 2 lands)

- Read `RefHandAnimState` (byte) and `RefIdleGrabRate`/`RefFarForwardRate`/`RefPsmovePullRate`
  from the live anim instance; treat them as the game's pose-selection state.
- When a grab starts, ramp `InteractionWeight` to 0 over ~150 ms (slerp per frame) so the authored
  `Item_grasp` pose wins; when the grab ends, ramp back to 1.
- Never disable pose selection, collision or the `GrabPoint` socket; the override only writes the 15
  finger local rotations, never the hand, wrist or actor transform.
- Log every transition (grab state, weight, joint targets) so the user can confirm in-headset that
  the game's authored grasp is not distorted.

## Commands for the runtime session

```powershell
# Build the investigation branch's emulator (retargeter linked, inspector panel available)
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-handrig -Target shadps4

# Launch Déraciné
cd M:\Development\Deracine-work\wt-handrig
.\build\shadPS4.exe --game "D:\Temp\Deracine\1.0\Image0\eboot.bin"

# In-game: Ctrl+F10 -> Debug -> Unreal objects
#   Discover Unreal objects
#   By name:  c0000      By class: SkeletalMeshComponent   (repeat for c0100)
#   Export report  ->  M:\Development\Deracine-work\snapshots\deracine_report.json
#   Export snapshot -> deracine.bin/.json

# Offline confirmation of the engine tables and object layout
cd M:\Development\Deracine-work\donors\ps4-unreal-dumper\tools
python uefind.py M:\Development\Deracine-work\snapshots\deracine.bin
python ueobj.py  M:\Development\Deracine-work\snapshots\deracine.bin
python uegen.py  M:\Development\Deracine-work\snapshots\deracine.bin M:\Development\Deracine-work\snapshots\sdk
```

Send back `deracine_report.json` (small) plus the `uefind.py` output; with those I can tell whether
the component and anim-instance candidates are correct and which transform-array candidates the
probe should target.

## Remaining obstacles to 15-joint-per-hand tracking

1. **Guarded guest writer + frame-phase hook** — the only hard new capability; everything else is
   discovery and mapping.
2. **No symbols**: component and transform-array offsets must be found by structure and validated by
   count/stride/unit-quaternion plausibility on the live process, not hardcoded.
3. **Animation ordering**: the game evaluates the AnimBP (pose assets, blends) every frame; a write
   before evaluation is silently discarded. The two-read test settles the window.
4. **Thread affinity**: pose evaluation may run on a task thread; writes must be synchronised with
   the game thread or applied at a known barrier.
5. **Grab interaction**: the finger override must yield to `Item_grasp` (weight 0) or item grabbing
   will look wrong even if it still functions.
6. **Verification is visual**: the success criterion is a visibly moving finger on the original mesh,
   checked by the user in-headset; that is the one step that cannot be automated here.
