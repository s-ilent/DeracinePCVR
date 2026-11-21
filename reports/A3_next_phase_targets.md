# Next phase: UE4 PS4 RHI graphics-context vtable and adjacent interfaces

Investigation targets for the phase after the runtime interception work. Nothing here is
verified at runtime yet; this is a prioritized plan grounded in what the completed phase
established and in A2's existing static relocation evidence.

## What the completed phase established

Verified at runtime, headless, on the pinned CUSA11919 01.01 image:

- The method at `eboot.bin + 0x0CBB9C0` (VA `0x800CBB9C0`) executes once per indexed draw.
- It calls `sceGnmDrawIndex` directly, same thread, 1-68 us before the call.
- `NumPrimitives * 3 == index_count` holds for all 128 captured pairs.
- The decoded `PM4CmdDrawIndex2` carries the same index count and index base.
- Caller VA `0x8013A68E8` (RVA `0x13A68E8`).

Not established: the UE4 symbol name of that method. The executable is stripped.

## What A2 already recovered statically

The vtable search is **already done**. Do not repeat it. A2's report records:

- A relative ELF relocation at table slot `0x03EE0190` points to function VA `0x0CBB9C0`.
- The table base is `0x03EDFCA0`.
- That base is loaded at code VA `0x0CACE4B`.
- The instruction at `0x0CACE52` stores the pointer through `[RBX]`, consistent with an object
  vptr assignment.
- The draw entry sits at byte offset `+0x4F0` from the table base.
- The table is vtable-shaped. RTTI and type name are not recovered.

Two candidate vtable address points therefore exist:

| Candidate | Address | Meaning |
| --- | --- | --- |
| Slot | `0x03EE0190` | the relocated pointer to the draw method |
| Table base | `0x03EDFCA0` | the vtable start, loaded at `0x0CACE4B` |

The same table base has further relative relocation targets already identified:

| Slot | Target | Reaches | Interpretation |
| --- | --- | --- | --- |
| `0x03EDFDB8` | FDE start `0x0CADC00` | `sceGnmDispatchDirect` at `0x0CADDA0` | compute dispatch candidate |
| `0x03EDFFE0` | FDE start `0x0CB5130` | `sceGnmSetVsShader` `0x0CB7075`, `sceGnmSetPsShader350` `0x0CB702B` | shader/pipeline-state candidate |

## Target 1: validate dispatch from caller `0x13A68E8`

This is the single highest-value task, and it is a validation task, not a search task.

The question: does the runtime caller `0x13A68E8` reach `0x0CBB9C0` through the vtable slot
`0x03EE0190`, or through a direct call?

Why it matters: the completed phase proved the call happens. It did not prove the dispatch
mechanism. If dispatch is virtual, the whole table is a recoverable interface and every slot
becomes a named operation. If dispatch is direct, the table is still real but the caller is
not using it, and the object layout must be recovered from field accesses instead.

Approach, static first:

1. Disassemble the caller at `0x13A68E8` and its containing FDE. Identify the call instruction
   that reaches `0x0CBB9C0`.
2. If it is an indirect call, decode the operand: which register, which displacement. A
   displacement of `+0x4F0` from a vtable pointer confirms slot `0x03EE0190`.
3. Trace where that vtable pointer came from. The store at `0x0CACE52` through `[RBX]` is the
   likely origin; confirm the object identity.
4. If it is a direct `E8` call, record that plainly and switch to recovering the object layout
   from the caller's field accesses.

Acceptance: the dispatch instruction at the caller is decoded, its target resolution is
explained, and the result either confirms or refutes the `+0x4F0` slot path.

Note the address-space distinction: `0x13A68E8` is a runtime RVA-derived VA from the 01.01
image. The A2 static evidence is on the 1.00 image (`8caee78e...`). The two images are both
approved by the probe contract, but the RVAs must be reconciled before comparing them
directly. Confirm the same table and slot exist at the corresponding offset in 01.01.

## Target 2: enumerate the table

Once dispatch is confirmed, walk the table from `0x03EDFCA0` and classify every slot.

For each slot: the target VA, whether it lands in an executable PT_LOAD and inside an FDE, and
which GNM import the target FDE reaches. Slots that reach no GNM import are either inline PM4
writers or state-only helpers, and both are interesting.

The two known slots give the expected shape: draw at `+0x4F0`, compute dispatch at
`+0x118` (`0x03EDFDB8 - 0x03EDFCA0`), shader state at `+0x340`
(`0x03EDFFE0 - 0x03EDFCA0`). The spacing suggests a large interface, consistent with a UE4
RHI command context.

## Target 3: shader operations

The shader slot `0x03EDFFE0` targets FDE `0x0CB5130`, which calls both `sceGnmSetVsShader`
(`0x0CB7075`) and `sceGnmSetPsShader350` (`0x0CB702B`). One function programming both stages
is consistent with a pipeline-state or shader-binding operation.

What to recover: the shader program address argument, the register-array layout, and the
null/disable branch. The semantic prize is mapping a PS4 GCN program address to a stable shader
identity across frames.

## Target 4: buffer, texture and render-target operations

No buffer, texture or render-target setter is identified yet. That is a gap, not a negative
result. Target 2 is the way to close it: classify slots by the GNM import each reaches.

Look specifically for index/vertex buffer binding near the draw slot, texture and sampler
binding referencing descriptor memory, and constant buffer updates carrying a size and a GPU
address.

## Target 5: the submission boundary

`sceGnmSubmitCommandBuffers` at `0x033C85A0`, with caller `0x0CD6BC4` inside FDE
`[0x0CD69A0,0x0CD6CD2)`, is the frame boundary. A2 found no relocation to its FDE start in the
context table, so treat it as a separate boundary rather than a member of this vtable.

What to recover: whether every frame passes through it, and how it relates to
`sceGnmSubmitDone` at `0x033C86E0` (caller `0x0CD2B4C`).

## Method notes

- Prefer static recovery. A2 shows byte decoding plus FDE containment plus relocation analysis
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
