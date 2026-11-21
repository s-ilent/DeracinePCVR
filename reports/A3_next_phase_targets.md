# Next phase: UE4 PS4 RHI graphics-context vtable and adjacent interfaces

Investigation targets for the phase after the runtime interception work. Nothing here is
verified yet; this is a prioritized plan grounded in what the completed phase established.

## What the completed phase established

Verified at runtime, headless, on the pinned CUSA11919 01.01 image:

- The method at `eboot.bin + 0x0CBB9C0` (VA `0x800CBB9C0`) executes once per indexed draw.
- It calls `sceGnmDrawIndex` directly, same thread, 1-68 us before the call.
- `NumPrimitives * 3 == index_count` holds for all 128 captured pairs.
- The decoded `PM4CmdDrawIndex2` carries the same index count and index base.
- Caller VA `0x8013A68E8` (RVA `0x13A68E8`).

Not established: the UE4 symbol name of that method. The executable is stripped.

## Why a vtable is the right next target

The completed phase proved one method by address. That does not scale: each additional
operation would need its own detour and its own run. A graphics-context vtable would give a
contiguous, ordered table of function pointers, so one recovery pass yields many methods at
once, and the slot order itself carries semantic information about which operations the
backend implements.

The working hypothesis is that `0x0CBB9C0` is one slot in such a table. That is an inference,
not a verified fact. Confirming it is the first task.

## Target 1: locate and validate the vtable

Goal: find the table that contains the known method, and prove the containment.

Approach:

1. Search the image's read-only data for a pointer equal to `0x0CBB9C0` plus the image base
   convention used by the loader. A vtable slot holds the absolute VA.
2. For each hit, read the surrounding pointer array and check that neighbouring entries land
   inside executable PT_LOAD ranges and inside plausible FDE ranges.
3. Cross-check against the known caller `0x13A68E8`: a virtual dispatch site typically loads
   the object pointer, loads the vtable pointer from offset 0, then calls through a fixed slot
   offset. If the caller reaches `0x0CBB9C0` that way, the slot index is confirmed.
4. Record the slot index, the table VA, the table extent, and the RVA of every entry.

Acceptance: the known method's address appears in a pointer table, the caller's dispatch
sequence is decoded and consistent with that slot, and every table entry is classified as
in-range or out-of-range.

Risk: the method may be called directly rather than virtually. If the caller uses a direct
`E8` call, the vtable hypothesis is wrong for this method and the plan shifts to recovering
the object layout from the caller's field accesses instead.

## Target 2: shader operations

The atlas already has static candidates for the two shader setters:

| Item | VA | Status |
| --- | --- | --- |
| `sceGnmSetVsShader` import stub | `0x33c81b0` | static import mapping |
| its caller candidate | `0xcb7075` | static, inside FDE `[0xcb5130, 0xcb865f)` |
| `sceGnmSetPsShader350` import stub | `0x33c8340` | static import mapping |
| its caller candidate | `0xcb6113` | static, inside FDE `[0xcb5130, 0xcb865f)` |

Both callers sit in the same FDE, which suggests one function programs both stages. That
function is a strong vtable-slot candidate for a "set shader" operation.

What to recover: the shader program address argument, the register-array layout, and whether
the same function handles the null/disable branch. The semantic prize is mapping a PS4 GCN
program address to a stable shader identity across frames.

## Target 3: buffer and texture operations

No buffer or texture setter appears in the current atlas. That is a gap, not a finding.

Approach: once the vtable is located, classify slots by the GNM import each one reaches. The
import list is the ground truth for which operations exist. Slots that reach no GNM import are
either inline PM4 writers or state-only helpers, and both are interesting.

Specifically look for:

- Index and vertex buffer binding, which should sit near the draw slot.
- Texture and sampler binding, which should reference descriptor memory.
- Constant buffer updates, which should show a size and a GPU address.

## Target 4: render-target and pipeline state

The atlas has no render-target setter either. The completed phase saw only one draw topology
(triangle list, `NumPrimitives` 1 or 2) and one GNM `type` value (`0x1`), so the current trace
does not exercise the state space.

What to recover: the render-target set call, the viewport/scissor set, and the blend/depth
state set. These are the operations that would let a future backend reconstruct a pass
boundary rather than a flat draw list.

## Target 5: the submission boundary

`sceGnmSubmitCommandBuffers` at `0x33c85a0`, with caller candidate `0xcd6bc4` inside FDE
`[0xcd69a0,0xcd6cd2)`, is the frame boundary. It is the natural place to delimit a frame in
any future trace, and it is already statically mapped.

What to recover: whether every frame passes through it, and how it relates to
`sceGnmSubmitDone` at `0x33c86e0` (caller `0xcd2b4c`).

## Method notes

- Prefer static recovery first. The atlas shows that byte decoding plus FDE containment
  already yields high-confidence candidates without running anything.
- Use the existing probe only to confirm a static candidate, not to discover one. The probe is
  default-off and should stay that way.
- Keep the distinction explicit in every record: a static candidate is not a runtime fact.
- The image is stripped, so names are always inferred from behaviour and call relations. Do not
  present an inferred name as recovered.

## Explicit non-goals

- No new runtime campaign until a static target is worth confirming.
- No guard infrastructure expansion.
- No renderer changes.
- No merge or push.
