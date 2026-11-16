# Phase 2 — live finger control: status, corrections and procedure

Branch `investigation/hand-rig`, worktree `M:\Development\Deracine-work\wt-handrig`. No guest-memory
write exists on this branch; every step below is read-only unless it says otherwise.

## Status

| Step | State |
|---|---|
| 3 — retargeting mathematics | **done and tested**, now calibrated in the hand's frame |
| Live discovery tooling (helpers for Step 1) | **done**: component probe, transform validators, bone-index and reference-pose checks, panel section |
| Synchronized-boundary override design (Step 4 prep) | **done as a state machine, no writes**: refuses unsafe phases, saves and restores the animation pose |
| 1 — identify live hand components | **needs the running game** |
| 2 — evaluation tracing | **needs the running game**; the pipeline model below is the UE 4.18 model to verify |
| 4 — one-finger visual proof | **blocked**: requires a guarded, frame-tagged writer (not present) and Step 2 evidence |
| 5 — grab blending | design only; the gameplay-state source is explicitly unverified |

New tests: 8 `HandRetarget*` (including the wrist-follow case) + 12 `TransformValidation*` /
`ComponentProbe*` / `HandOverrideSession*`. All pass.

## Correction 1 — array lengths are not 79

The asset data distinguishes three different bone counts and the runtime may use any of them:

| Source | Count | Evidence |
|---|---|---|
| `c0000` mesh's own reference list (`USkeletalMesh.ReferenceSkeleton`) | 79 | `mesh-*.json`: 79 bones, 40 of them fingers |
| `c0000_Skeleton` (the asset the animation system uses) | 105 | `skeleton-*.json`: 79 body + 26 ponytail bones |
| LOD 0 active/skinned subset | 26 bones, of which 15 are the right-hand fingers | `mesh-*.json` `ActiveBoneIndices` |

So a runtime pose array may be 79, 105 or a narrower LOD subset depending on which structure it is.
`ExpectedBoneCounts{mesh, skeleton, lod_active}` carries all three; the probe **accepts a candidate
only if its length equals one of them** and reports the measured count, and
`ValidateFingerChainIndices`/`CompareReferencePose` verify that the rig's bone indices, parent links
and reference rotations actually match the extracted skeleton before any override is attempted. The
inspector exposes the extracted numbers through the asset report; nothing is assumed.

Two more consequences recorded here: LOD selection may change the active set at runtime (the game can
force LOD), and the pose array's length must be re-checked after discovery, not cached from the asset
count alone.

## Correction 2 — a two-read test does not prove the write window

Reading an array twice across frames only shows the animation system runs. The write window has to be
established against the actual UE 4.18 evaluation pipeline:

```
USkeletalMeshComponent::TickComponent
  └─ TickPose()
       ├─ UAnimInstance::UpdateAnimation()            (game thread prep)
       ├─ DispatchParallelEvaluationTasks()           (parallel anim tasks — may run on workers)
       │     └─ ParallelUpdateAnimation()             pose buffers read/written here
       ├─ CompleteParallelUpdate()                    task completion / buffer swap
       └─ RefreshBoneTransforms()                     ComponentSpaceTransforms become current
  └─ OnBoneTransformsFinalized broadcast              (post-animation, pre-skinning signal)
FSkeletalMeshObject::Update(); bone matrices derived and consumed by skinning (render thread)
```

Key facts that change the experiment design:

- `ComponentSpaceTransforms` and `BoneSpaceTransforms` are **double-buffered** (an anim evaluation
  context holds two arrays and swaps them); the "current" buffer alternates, so an override must
  target the post-swap buffer of the frame it runs in.
- Parallel evaluation means the pose is being written by worker tasks between dispatch and
  completion; writing during that span races.
- Skinning matrix derivation happens **after** the component-space pose is final; writing after that
  derivation is too late for the frame, and writing before evaluation is silently overwritten.

Therefore the safe window is **after `CompleteParallelUpdate`/`RefreshBoneTransforms` (the finalized
signal) and before the mesh object derives skinning matrices**. Proving that a candidate pointer is
*in* that window needs a **tagged sentinel experiment**, not two reads: with the guard flag on, write
one joint with a value distinguishable from the animation, read it back at several candidate points
in the same frame and the next frame, and record where it survives. That experiment is Step 4 and is
deliberately not implemented here.

Read-only evidence to collect during Step 2 (safe now, no writes):

1. Locate the transform arrays with `Probe skeletal mesh components` (below). Record length, layout
   and data pointer.
2. Sample the same field's `data` pointer across frames: a pointer that alternates between two values
   is the double buffer; a pointer that never changes is a single backing store.
3. Correlate array contents with the animation: a finger joint that is animated changes between
   samples; a static joint does not. This identifies which array is the evaluated pose and which is
   cached/previous-fame data.
4. Record the component's mesh/skeleton/anim-instance identities; later the writer's hook must name
   the exact object it writes, not an offset.

## Calibration — hand frame and the animated wrist

`HandRetargeter::Calibrate` now stores offsets in the **hand bone's frame**:

```
C_joint = inverse(hand_reference_world) * reference_world(joint) * inverse(Convert(xr_rest(joint)))
target_world = hand_world_live * Convert(xr_live(joint)) * C_joint
local = inverse(parent_world) * target_world
```

`hand_world_live` must be the **currently animated** hand bone's world rotation (component transform ×
hand bone local rotation from the evaluated pose), not the asset's reference value. The regression
test `FingersFollowTheAnimatedWrist` proves that rotating the wrist moves the whole finger pose with
it while the local finger rotations stay at the reference — which is what makes the calibration valid
during animation. Do not feed a rest-pose wrist rotation at runtime.

## Grab handling stays separate from the mathematics

`HandRetargeter::InteractionWeight(desired, grab_active)` is a pure weighting function. **No gameplay
state source is wired or verified**: `RefHandAnimState`, `RefIdleGrabRate` and the `GrabPoint` socket
are candidates from the asset analysis, but until they are read from a live anim instance and shown
to change at grab transitions, the override runs with a manually chosen weight and grabs are not
detected. The writer must not infer grabs from timers or socket proximity.

## Override session (no writes)

`HandOverrideSession` is the piece that makes a future write reversible:

- `Apply` refuses `BeforeEvaluation`, `EvaluationRunning`, `Unknown` and `SkinningHandoff`; only
  `AfterEvaluation` writes, and it records why it refused otherwise.
- On the first write of a frame it saves the animation rotations it replaces; repeated writes in the
  same frame blend against that saved pose, not against its own output.
- `Restore` puts the saved animation pose back; `SetEnabled(false)` causes the next `Apply` to restore
  automatically. Weight 0 does the same.
- It holds no guest pointers and performs no writes; the caller owns the store, so the experiment is
  reversible and the branch stays read-only until the window is proven.

## Inspector additions (read-only)

In `Debug → Unreal objects` (Ctrl+F10), below the object list:

- **Probe skeletal mesh components** enumerates live `SkeletalMeshComponent` objects, and for each
  reports the mesh, skeleton and animation-instance it references plus every TArray candidate whose
  contents validate as an `FTransform` array. Each candidate prints data pointer, count, stride,
  quaternion/translation/scale offsets and whether it validated.
- Validators applied per candidate: quaternion normalisation, finite translations/scales, scale range,
  and expected-length match. The probe accepts a candidate only when every element passes.

The validators and probe are unit-tested against synthetic component objects, including a rejected
pointer array and a rejected wrong-length array.

## Exact launch and capture procedure

```powershell
# 1. Build this branch's emulator (already contains the probe; no writes)
pwsh -File M:\Development\Deracine-work\_build\build.ps1 -Root M:\Development\Deracine-work\wt-handrig -Target shadps4

# 2. Launch Déraciné
cd M:\Development\Deracine-work\wt-handrig
.\build\shadPS4.exe --game "D:\Temp\Deracine\1.0\Image0\eboot.bin"

# 3. In-game, reach a scene and open the inspector: Ctrl+F10 -> Debug -> Unreal objects
#    a. Discover Unreal objects
#    b. By class: SkeletalMeshComponent      (repeat with AnimInstance if the list is short)
#    c. Probe skeletal mesh components
#    d. Export report    -> M:\Development\Deracine-work\snapshots\deracine_report.json
#    e. Export snapshot  -> deracine.bin / deracine.json
```

`deracine_report.json` is the required evidence for the next step: it must show the live hand
components, their mesh/skeleton/anim-instance identities and the candidate pose arrays with lengths.

Optional offline confirmation of the engine tables from the same session:

```powershell
cd M:\Development\Deracine-work\donors\ps4-unreal-dumper\tools
python uefind.py M:\Development\Deracine-work\snapshots\deracine.bin
python ueobj.py  M:\Development\Deracine-work\snapshots\deracine.bin
```

## What the next iteration needs

1. The report above (component identities + measured array lengths/layouts).
2. A decision on the write window from the read-only sampling (pointer alternation, which array is
   the evaluated pose).
3. Only then: a flag-gated writer plus the tagged sentinel experiment, one joint, idle hand, restore
   on disable, and the user's visual confirmation.

No guest offsets are guessed anywhere in this document; the structures to identify are named, and the
probe reports what the process actually contains.
