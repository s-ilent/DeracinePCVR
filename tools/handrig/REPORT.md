# Déraciné — offline hand rig and animation investigation

Branch `investigation/hand-rig`, worktree `M:\Development\Deracine-work\wt-handrig` (based on
`integration/unreal-inspector` @ `5e46d2b`). Nothing here changes the Déraciné runtime branch, frame
pacing, Vulkan presentation or memory pooling.

**Tool and inputs.** A pak reader built on **CUE4Parse 1.2.2.202611** (net10.0) with
**oo2core_9_win64.dll** for Oodle, mounted with the `GAME_UE4_18` container against
`D:\Temp\Deracine\1.0\Image0\gameproject\content\paks\gameproject-ps4.pak` (8,186,633,373 bytes,
dated 2018-04-09). All 9,329 `GameProject` packages parsed without a failure at that container
version, which is the evidence that the version container matches the cooked assets. No asset is
extracted to disk; the tool reads package headers and specific objects and writes JSON/CSV.

Reproduce: `tools/handrig/` (`dotnet build -c Release`, then `handrig inventory|skeleton|mesh|
anim|posefinger|anims`, `python anatomy.py`, `python manifests.py`). Outputs land in
`_build/handrig/` (outside git).

## Verdict

**Yes — Déraciné has a usable articulated hand rig.** The player's visible hands are two
single-hand character meshes (`c0000` right, `c0100` left), each skinned to a full humanoid
skeleton with a **3-joint, 5-digit finger chain per hand**, and the game already poses those fingers
with a **large named pose library** (61 + 132 poses per hand) instead of driving them procedurally.
The animation Blueprint selects those poses through `PoseByName` nodes. There is no continuous
finger IK/FK simulation to fight: the rig is driven by bone transforms, so OpenXR joint rotations
can be pushed into the same channels after animation evaluation.

## A. Asset inventory

| Kind | Count | Notes |
|---|---|---|
| `GameProject` packages | 9,329 | 115,707 exports |
| `Skeleton` | 167 | characters + items |
| `SkeletalMesh` | 278 | characters, items, objects |
| `AnimSequence` | 1,401 | no montages anywhere |
| `SkeletalMeshComponent` | 633 | inside blueprints |
| `SkeletalMeshSocket` | 570 | attach/grip points |
| `AnimBlueprintGeneratedClass` | 273 | cooked: no editor graph |
| `PoseAsset` | 4 | the hand pose library |

Player hand actors (not names guessed from "Hand" — found through the action blueprints that own
hand state, mesh, and pose assets):

| Path | Role |
|---|---|
| `GameProject/Content/model/chr/c0000/model/c0000.uasset` | **right hand mesh**, skeleton `c0000_Skeleton`, 79 bones, **40 finger bones bound, 15 skinned**, 1 LOD, 3 materials (`hand` slot = `c0000_MI`, plus ring materials) |
| `GameProject/Content/model/chr/c0000/model/c0000_Skeleton.uasset` | right rig, 105 bones (79 body + 26 ponytail), 40 finger bones, 28 twist/nub helpers, 7 sockets |
| `GameProject/Content/model/chr/c0100/model/c0100.uasset` | **left hand mesh**, skeleton `c0100_Skeleton`, 79 bones, 40 finger bones bound, 15 skinned, 4 materials |
| `GameProject/Content/model/chr/c0100/model/c0100_Skeleton.uasset` | left rig, 79 bones, 40 finger bones, 27 helpers, 9 sockets |
| `GameProject/Content/Action/c0000/c0000_BP.uasset`, `c0000_MenuBP.uasset`, `c0000_AnimBP.uasset` | right-hand actor: hand transform, grab/animation state, menu, posing |
| `GameProject/Content/Action/c0100/*` | the mirrored left-hand actor |
| `GameProject/Content/model/chr/c0000/motion/RightHand.uasset` | **61 named right-hand poses** (PoseAsset) |
| `GameProject/Content/model/chr/c0000/motion/Item_grasp.uasset` | **132 grasp poses** (PoseAsset) |
| `GameProject/Content/model/chr/c0100/motion/LeftHand.uasset` | 60 named left-hand poses |
| `GameProject/Content/model/chr/c0100/motion/Item_grasp.uasset` | 132 grasp poses |

Each rig poses only one hand: in `c0000` the R- chains are 11–15 units long and the L- chains
collapse to ~0.1 (and mirrored in `c0100`). That is what identifies them as right/left and not
"player body / NPC".

Sockets (from the rigs, i.e. attach points the game uses):

- `c0000`: `GrabPoint` (on `Socket-point`), `Ring_SFX` on `R-Finger2`, `SFX_PlayerHandFairy` and
  `SFX_Appear` on `R-Hand`, `0020_ItemDesc` on `R-Hand`, `90009010`, `1000`.
- `c0100`: `GrabPoint` on `L-Hand`, `GrabPoint_L`, `1010` on `L-Forearm`, `1020` on `L-Finger4`,
  `SFX_PlayerHandFairy`, `SFX_Appear`, `0020_ItemDesc`, `90109010`.

## B. Skeleton analysis

Both rigs: `Master → Pelvis → Spine → {R,L}-Clavicle → UpperArm → Forearm → Hand`, then five chains
per hand:

```
<side>-Finger0 / 01 / 02 (+ Finger0Nub)      thumb
<side>-Finger1 / 11 / 12 (+ Finger1Nub)      index
<side>-Finger2 / 21 / 22 (+ Finger2Nub)      middle
<side>-Finger3 / 31 / 32 (+ Finger3Nub)      ring
<side>-Finger4 / 41 / 42 (+ Finger4Nub)      little
```

Reference pose, chains and per-bone reference transforms are in `hand-rig-manifest.json`
(`joints` holds index, parent, local translation and local quaternion for every finger bone, and
`bones[].worldPosition` the composed reference-pose positions).

**Thumb identity is measured, not assumed** (`finger-anatomy.txt`): digit 0 starts 6.77 units from
the wrist while digits 1–4 start at 15.4–15.7, and its direction leaves the palm plane by 0.69
versus 0.35–0.40 for the others. So `Finger0` is the thumb and its first bone already includes the
metacarpal.

Helper/support bones: `R/L-ForeTwist1..3`, `*UpArmTwistNub`, `*ForeTwistNub`, `*ThighTwistNub`,
`*CalfTwistNub`, plus a `Nub` on every chain end. **No IK bones and no virtual bones exist**
(`virtualBones` is empty in both rigs). Fingers are pure FK: 3 skinning joints per digit, no
metacarpal bones for digits 1–4, no tip bones.

Chain lengths (both rigs, mirrored): thumb 14.63, index 13.90, middle 15.04, ring 14.30, little
11.11 units — so the thumb's 3 bones include the metacarpal, while the other digits start at the
knuckle.

## C. Animation system

- **No montages** in the whole game: grips and gestures are not montage-driven.
- **837 of 1,401 sequences animate finger bones** (counted over both the raw and compressed
  track-to-skeleton maps; cooked PS4 sequences keep only the compressed map).
- All eight player-hand sequences animate fingers: five for `c0000`, three for `c0100`; the richest
  is `a000_040000` with 98 compressed tracks and 40 finger tracks.
- **The finger articulation lives in pose assets.** Per hand: `RightHand` 61 poses / 26 tracks
  (15 finger), `LeftHand` 60 / 31 (15), `Item_grasp` 132 / 31 (15, each side). **All 15 finger
  tracks are articulated**, with per-joint rotations varying up to **114° (RightHand) and 126°
  (Item_grasp)** across the pose set.
- Pose names are object-specific, e.g. `Del_1_Grasp_Cylinder`, `Del_2_Pinch_Thick`,
  `Del_3_Pinch_Thin`, `Del_5_Flower`, `Del_7_Pocket watch`, `Del_9_Key`, `Del_10_Bottle`,
  `Del_12_Stick`, `Del_13_Book`, `Del_18_Glasses`; `Item_grasp` starts `0_Null`,
  `1_Grasp_Cylinder`, `2_Pinch_Thick`, `3_Pinch_Thin`, `4_Thirdfinger`.
- The animation blueprints survive as cooked packages: the editor graph is **not** recoverable, but
  the anim-graph node objects remain as structs. `c0000_AnimBP` contains **137 `PoseByName` nodes,
  2 `SequenceEvaluator`, 1 `TwoWayBlend`, 1 `BlendListByEnum`, Root**; `c0100_AnimBP` has the same
  shape. So the fingers are driven by *named pose lookups blended per state*, not by procedural
  animation maths.
- Blueprint-side state that selects poses (export/variable names, evidence of intent, not behaviour
  yet): `GetHandAnimState`, `RefHandAnimState` (byte), `RefIdleGrabRate`, `RefFarForwardRate`,
  `RefPsmovePullRate`, `TriggerInputToHandGrabRateCurve`, `GetHandLRType`, `GetMotionControllerHandType`,
  `GetPlayerHandTransform`, `SetMeshType`, `bPsMove`, `HandPositionScene`, `GrabPoint`.

Consequence for blending: the game has a discrete pose space, not continuous finger motion, which is
exactly what a retargeter can override or blend against — with the constraint that any blend weight
must respect the grab states so items keep registering.

## D. OpenXR retargeting design

OpenXR exposes 26 joints; the rig offers **17 controllable targets** per hand (`<side>-Hand` plus 15
finger bones). Mapping (`openxr-mapping.csv`, both sides):

| OpenXR joint | Game bone | Handling |
|---|---|---|
| `PALM`, `WRIST` | `<side>-Hand` | palm drives the hand bone; the wrist pose is applied to the hand actor transform |
| `THUMB_METACARPAL` | `<side>-Finger0` | first thumb bone is the metacarpal (measured: base at the wrist) |
| `THUMB_PROXIMAL` | `<side>-Finger01` | |
| `THUMB_DISTAL` | `<side>-Finger02` | |
| `THUMB_TIP` | — | position only, no bone |
| `*_METACARPAL` (4 fingers) | — | folded into the hand bone |
| `*_PROXIMAL` | `<side>-FingerN` | direct |
| `*_INTERMEDIATE` | `<side>-FingerN1` | direct |
| `*_DISTAL` | `<side>-FingerN2` | direct |
| `*_TIP` | — | position only; overlays/finger-tip effect or next phase |

Retargeting mathematics (per joint, both sides handled by each rig's own reference data, so left/right
mirroring needs no special case):

1. From the reference pose in `hand-rig-manifest.json`, compute each finger bone's world rotation
   `R_bone_ref` (the tool already composes the chain; the manifest holds local quaternions and
   world positions).
2. At runtime, map the OpenXR joint into the hand's space: `R_xr` is the joint's orientation in the
   actor's coordinate system (OpenXR is Y-up right-handed; the Unreal hand actor is Z-up
   left-handed, so convert with a fixed basis change and one reflection, then apply the actor's
   world transform).
3. Per-joint offset absorbs axis conventions: `C_joint = R_bone_ref * R_xr_rest⁻¹`, calibrated once
   from the XR hand at the same rest pose as the asset (or from the user's T-pose).
4. Target world rotation: `R_target = R_xr * C_joint`; local bone rotation:
   `R_local = inverse(R_parent_target) * R_target`, written into the bone's local rotation channel.
5. Finger length/anthropometry: do **not** copy positions. Rotations only, so the game's own bone
   lengths are preserved. Clamp per-joint against the observed pose-asset range (e.g. ±60° per
   phalanx, ±114° across the whole pose set) to avoid self-intersection.
6. Grip priority: when the game state says "grabbing", blend the retargeter down (weight → 0) and let
   the `Item_grasp` pose win, otherwise objects will not register with the `GrabPoint` socket.
   Conversely, when idle/interacting, weight 1 gives full OpenXR finger tracking.

### Three implementation approaches

1. **Modify existing animation parameters or nodes.** Feed pose selection/blend weights (the
   `BlendListByEnum` / `RefHandAnimState` / `RefIdleGrabRate` inputs) through the emulator and let
   the game's own `PoseByName` network pose the fingers. *Feasible now*, but it can only pick
   discrete authored poses — no continuous per-joint tracking — and it fights the game's state
   machine. Best use: grip fidelity, not finger tracking.
2. **Bone-transform overrides after animation evaluation.** Write the 15 target local quaternions
   after the pose is evaluated and before skinning. *Shortest path to real finger tracking*: no
   re-cooking, no graph surgery, reuses existing skinning. Needs (a) the anim instance/component
   pointers, (b) the evaluated transform array layout, (c) a game-thread-safe hook so writes land
   between evaluation and skinning, (d) Phase-2 write capability. Risk: overwriting the grab pose
   (mitigated by state-aware blending), and writing during the wrong phase (mitigated by hooking
   the evaluation step, not the renderer).
3. **Host-side retargeter integrated with emulator-side Unreal instrumentation.** The emulator owns
   the retargeter and pushes transforms at a defined emulation hook (e.g. after the skeletal mesh
   component's pose update), with per-title layout learned by the inspector rather than hardcoded.
   *Most reusable* and the right end state for Déraciné + Summer Lesson + other UE4 PSVR titles;
   more engineering than (2) because it needs a stable hook inside the emulated engine path.

**Recommendation.** Target (2) as the first playable milestone (fastest evidence that the rig
accepts OpenXR poses), designed from day one as (3): keep the retargeter and the joint map in the
emulator's `Core::Unreal` layer, learn pointers/layout with the inspector, gate writes behind a
setting, and keep the game's pose assets authoritative whenever a grab is active. Do not attempt to
reconstruct or patch blueprint bytecode — the cooked graph is not editable, and the surviving node
objects are only useful as evidence (137 `PoseByName` nodes confirm the design).

## Manifests produced

`_build/handrig/`: `summary.json`, `hand-rig-manifest.json`, `openxr-mapping.csv`,
`finger-anatomy.txt`, `animation-inventory.csv`, `pose-inventory.csv`, `asset-dependencies.csv`,
`animations.csv` (all 1,401), `inventory-exports.csv`, `inventory-references.csv`,
`pak-entries.csv`, plus per-asset `skeleton-*.json`, `mesh-*.json`, `anim-*.json`,
`posefinger-*.json`. No asset bytes are stored and no game content is committed.

## Unknowns requiring live validation

1. **Engine tables** in the running game: which name table (UE 4.18 predates `FNamePool`, so expect
   `TNameEntryArray`) and the object array layout — the inspector reports both; run `uefind.py` on a
   real snapshot to confirm.
2. **The live hand actors.** Which component is `c0000`/`c0100` per hand at runtime, whether both
   exist as separate actors, and what `SetMeshType` / `swap_hands` selects. Locate the
   `SkeletalMeshComponent` for each and confirm the skeleton reference matches.
3. **The anim instance.** Its class (`c0000_AnimBP_C`), the current `RefHandAnimState` value and the
   `PoseByName` node that is active — needed for state-aware blending (approach 2).
4. **Bone transform arrays.** Offset of the component-space / bone-space transform arrays in this
   build, the write window relative to `TickPose`/skinning, and whether the game rewrites bone
   transforms later in the frame.
5. **Grab interaction.** Which socket/notify drives grabbing (`GrabPoint` on `Socket-point` vs
   `GrabPoint` on `L-Hand`), and whether overriding finger bones breaks item attach or collision.
6. **Per-joint skin weights** (assumed full because the 15 finger bones are in the LOD's active bone
   set): confirm visually that fingers deform all the way to the fingertip.
7. Whether any scene swaps the hand mesh for a glove/held item variant (`c0000_CD` etc.).

## Limits of this investigation

Pose-asset transforms, skeleton reference poses, skinning bone sets, socket definitions and
animation track maps are read directly from the cooked assets. The AnimBlueprint *graph* is not
recoverable (only node objects survive), and no runtime behaviour was observed — everything about
hands at runtime above is a hypothesis anchored to static evidence. Nothing was written to the game
or the emulator, and no copyrighted asset is included in this report or the manifests.
