# A9: minimal semantic capture contract

A passive capture design for the recovered command list. This is a specification, not an
implementation. It states what can be recorded safely and what cannot.

## The lifetime constraint that shapes everything

The executor at `0x13764D0` resets the command allocator after the walk and runs **no**
destructors. So:

- A command payload is valid only between append and the allocator reset.
- A borrowed pointer into a payload dangles after the next execution.
- Reference-counted resource fields are not released by the executor, so their lifetime is
  governed elsewhere and is not observable from the command list alone.

Therefore a capture must **copy scalar values during the walk** and must not retain pointers into
command payloads. That is the single most important rule in this contract.

## What can be captured safely

### 1. Command type, via verified vtable identity

The command's `vptr` at `+0x00` is one of the five verified vtables:

| Vtable | Command |
| --- | --- |
| `0x03F26A08` | SetViewport |
| `0x03F26A28` | SetRenderTargets |
| `0x03F26A48` | SetStreamSource |
| `0x03F26A68` | DrawPrimitive |
| `0x03F26A88` | DrawIndexedPrimitive |

Recording the vtable value is safe and exact. An unrecognised vtable should be recorded as
unknown rather than guessed.

### 2. Sequence number and list identity

The walk order is list order, which is append order. A monotonically increasing counter per
execution gives the sequence. The list identity is the global the executor read
(`0x47C07D0` or `0x47C07D8`), which distinguishes the primary and secondary lists.

Both are scalars and safe.

### 3. Execution thread

The executor already computes thread identity at `0x1376510` and `0x137652B` via
`scePthreadGetthreadid`. A capture can record the same value at the dispatch site, which is
cheaper and more accurate than inferring it from function names.

Safe, and it is the only reliable way to answer "which thread ran this".

### 4. Indexed-draw arguments

From the `DrawIndexedPrimitive` payload, all eight fields are scalars:

| Offset | Field |
| --- | --- |
| `+0x10` | IndexBuffer (a pointer, record as an opaque value, do not dereference) |
| `+0x18` | PrimitiveType |
| `+0x1C` | BaseVertexIndex |
| `+0x20` | FirstInstance |
| `+0x24` | NumVertices |
| `+0x28` | StartIndex |
| `+0x2C` | NumPrimitives |
| `+0x30` | NumInstances |

The `IndexBuffer` pointer is safe to record as a value. It is not safe to dereference later,
because the buffer's lifetime is not governed by the command list.

### 5. Render-target and depth-stencil binding

From the `SetRenderTargets` payload:

| Offset | Field | Safe as |
| --- | --- | --- |
| `+0x10` | count | scalar |
| `+0x18 + i*0x18` | RT[i] resource pointer | opaque value |
| `+0x18 + i*0x18 + 0x8` | RT[i] second pointer | opaque value |
| `+0xD8` | depth resource pointer | opaque value |
| `+0xE8` | UAV count | scalar |
| `+0xF0 + i*8` | UAV[i] pointer | opaque value |

All safe as values. None safe to dereference.

### 6. Viewport state

From the `SetViewport` payload, six scalars at `+0x10` through `+0x24`, with `MinZ` and `MaxZ`
as floats. Fully safe.

### 7. Vertex-buffer state

From the `SetStreamSource` payload: `StreamIndex` at `+0x10`, `VertexBuffer` at `+0x18` (opaque),
`Offset` at `+0x20`. Safe as values.

### 8. Shader-resource identity, where independently supported

This is the weakest item and should be marked as such in any capture.

The shader binding does not go through a command object. It happens inside the recorder at
`0x0CB5130`, reached from the context vtable slot `+0x340`. So a command-list capture will not
see it.

What can be recorded, if the recorder is instrumented separately: the shader resource pointer
from `shader-state + 0x200`, and the `shader_modifier` from `shader-state + 0x1C0`. Both are
opaque values. The program data at `shader-resource + 0x1D8` is a borrowed pointer and must not
be retained.

## What must not be captured

- Any pointer dereferenced after the walk. The allocator reset invalidates payloads.
- Any assumption that a resource pointer is still live at capture time.
- Thread identity inferred from a function name. Use `scePthreadGetthreadid`.
- Command type inferred from a payload shape. Use the vtable.

## Command execution versus state change

This distinction matters and is easy to get wrong.

The PS4 RHI receivers cache state and suppress redundant PM4 writes:

- `SetRenderTargets` compares a bitmask at `+0x518` against `+0x510` and emits nothing when
  unchanged.
- `SetViewport` compares six cached floats at `+0x80`..`+0xA4` and emits nothing when unchanged.

So a capture that counts commands will **overcount** actual state changes, and a capture that
counts PM4 packets will **undercount** them. The two must be recorded separately and labelled.

A correct capture records:

1. The command was executed (from the executor walk).
2. Whether the receiver emitted a packet (from the receiver, if instrumented).

Conflating them produces a stream that looks like more state churn than the GPU ever saw.

## Minimal record schema

```
record:
  sequence        u32   per-execution counter
  list_id         u8    0 = primary (0x47C07D0), 1 = secondary (0x47C07D8)
  thread_id       u32   scePthreadGetthreadid at the dispatch
  vtable          u64   the command's vptr, exact
  command         enum  derived from vtable, or unknown
  payload         union per command, scalars only
  emitted_packet  bool  only if the receiver is instrumented; else absent
```

Everything in that record is a scalar or an opaque pointer value. Nothing requires the payload to
outlive the walk.

## What this contract does not cover

- GNM submission correlation. The executor's tail calls `0x47B3B08`'s `vtable[0x3C0]`, but that
  was not traced to `sceGnmSubmitCommandBuffers`.
- The two recorded thread ids' owners. The gate compares against them; their roles are inferred.
- Whether `Execute` can append to the same list during the walk.
- Resource lifetime. It is governed outside the command list and is not observable here.

## Implementation note

If this is ever implemented, it should be a passive observer at the dispatch site `0x1376954`
that copies scalars into a bounded ring buffer, with the same default-off opt-in and bounded
record count as the existing A3 probe. It must not dereference, must not allocate during the
walk, and must not alter the command or the list.
