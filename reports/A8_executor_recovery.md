# A8: the actual deferred-command executor

## Result

`0x14B00` is **not** the RHI command executor. The real executor is at `0x13764D0`, with the
dispatch at `0x1376954`.

A7's executor identification was wrong on three counts, all of which the audit identified
correctly. This report supersedes it.

## Why `0x14B00` is not the executor

### The offset contradiction

The caller at `0x1486A` passes `object + 0x38`:

```
1485e: 49 8d 7f 38           leaq   0x38(%r15), %rdi
1486a: e8 91 02 00 00        callq  0x14b00
```

So `0x14B00`'s `0x18(%r13)` is the original object's `+0x50`, and its `0x20(%r13)` is `+0x58`.
The teardown at `0x14810` reads `+0x08` and `+0x10` of the *original* object. Those are
different fields, so A7's claim that both functions process the same array was unfounded.

### The extent contradiction

`0x14B00` reads `0x48(%rdi)` from each dispatched object:

```
14b4f: 8b 47 48              movl   0x48(%rdi), %eax
```

The recovered `FRHICommandDrawIndexedPrimitive` object is about `0x38` bytes. Reading `+0x48`
would be past its end. So the dispatched objects are not RHI commands.

### The calling-convention contradiction

At the dispatch, `%rsi` is never prepared:

```
14b9f: 48 8b 07              movq   (%rdi), %rax
14ba2: ff 10                 callq  *(%rax)
```

`FRHICommandDrawIndexedPrimitive::Execute` requires `%rsi` to be the command list, and the
wrapper at `0x13A68B0` reads `0x20(%rsi)` immediately. `0x14B00` does not set it.

### What `0x14B00` actually is

It walks an array at `+0x50` with a count at `+0x58`, reads a 32-bit key at `+0x48` of each
element, compares it against a value at `(%r13)`, and dispatches `vtable[0]`. That is a
task/job dispatch loop over an unrelated object type, not a deferred RHI command list.

## The real executor

### Location

| Item | Value |
| --- | --- |
| Function | `0x13764D0` |
| FDE | `[0x13764D0, 0x1377660)` |
| Dispatch | `0x1376954` |

### The loop

```
137692f: 41 c6 47 10 01        movb   $0x1, 0x10(%r15)     ; list->bExecuting = true
1376934: 49 8b 3f              movq   (%r15), %rdi        ; cmd = list->head
1376937: 48 85 ff              testq  %rdi, %rdi
137693a: 74 22                 je     0x137695e            ; empty -> done
137693c: 4c 8d 75 a0           leaq   -0x60(%rbp), %r14
1376940: 48 8b 5f 08           movq   0x8(%rdi), %rbx      ; next = cmd->Next
1376944: 48 89 3d 7d 9a 44 03  movq   %rdi, 0x47c03c8      ; current command global
137694b: 4c 89 fe              movq   %r15, %rsi           ; arg2 = command list
137694e: 4c 89 f2              movq   %r14, %rdx           ; arg3 = scratch
1376951: 48 8b 07              movq   (%rdi), %rax        ; rax = cmd->vptr
1376954: ff 10                 callq  *(%rax)              ; vtable[0] = Execute
1376956: 48 89 df              movq   %rbx, %rdi           ; cmd = next
1376959: 48 85 db              testq  %rbx, %rbx
137695c: 75 e2                 jne    0x1376940            ; loop
137695e: 49 8d 7f 30           leaq   0x30(%r15), %rdi     ; reset the allocator
1376962: 31 f6                 xorl   %esi, %esi
1376964: 41 c6 47 10 00        movb   $0x0, 0x10(%r15)     ; bExecuting = false
1376969: e8 62 89 ac ff        callq  0x0E3F2D0
137696e: 41 c7 47 14 00 00 00 00  movl $0x0, 0x14(%r15)  ; NumCommands = 0
1376976: 49 c7 07 00 00 00 00  movq   $0x0, (%r15)         ; head = null
137697d: 4d 89 7f 08           movq   %r15, 0x8(%r15)      ; tail slot = &head
```

### The calling convention matches

| Register | Value | Matches |
| --- | --- | --- |
| `%rdi` | `cmd` | command object |
| `%rsi` | `%r15` = command list | `Execute(FRHICommandListBase&)` |
| `%rdx` | `-0x60(%rbp)` | a third argument the wrapper ignores |
| `vtable[0]` | `cmd->vptr[0]` | `Execute` |

The wrapper at `0x13A68B0` reads `0x20(%rsi)` as the context, which is exactly what a
`FRHICommandListBase` provides. The convention lines up.

### The list fields

| Offset | Field | Evidence |
| --- | --- | --- |
| `+0x00` | head | `movq (%r15), %rdi` |
| `+0x08` | tail slot | `movq %r15, 0x8(%r15)` on reset |
| `+0x10` | `bExecuting` | set to 1 before, 0 after |
| `+0x14` | `NumCommands` | zeroed on reset |
| `+0x30` | allocator | `leaq 0x30(%r15), %rdi` then `0x0E3F2D0` |

This is `FRHICommandListBase` with `Root`, `CommandLink`, `bExecuting`, `NumCommands` and the
mem-stack allocator. The A7 append analysis remains correct and applies to this list.

### Ordering

The executor walks `Next` from the head, so **execution order is list order**, and list order is
append order. That is now established statically, from the same structure the append writes.

This resolves the A7 open question. The array at `0x14B00` was never the ordering structure.

### Cleanup

After the walk, the executor resets the allocator, zeroes `NumCommands`, clears the head and
points the tail slot back at the head. So the list is emptied in place, and the command objects
are reclaimed by the allocator reset rather than individually destroyed.

That differs from the `0x14810` teardown, which calls `vtable[1]` per slot. The two are
different lifetimes: `0x13764D0` executes and resets, `0x14810` destroys.

## The RSRC1 field correction

A7 claimed the allowed mask `0x030003C0` covers VGPRS, priority and float-mode. That was wrong.

The mask bits are 6, 7, 8, 9, 24 and 25. In `SPI_SHADER_PGM_RSRC1_VS`:

| Bits | Mask | Field |
| --- | --- | --- |
| 0-5 | `0x0000003f` | VGPRS |
| 6-9 | `0x000003c0` | SGPRS |
| 10-12 | `0x00001c00` | priority |
| 13 | `0x00002000` | float mode |
| 14-15 | `0x0000c000` | DX10 clamp |
| 24-25 | `0x03000000` | VGPR component count |

So `0x030003C0` is **SGPRS (bits 6-9) plus bits 24-25**, which are the vertex-component
fields. It is not VGPRS, not priority, and not float mode.

The verified fact stands: `shader-state + 0x1C0` modifies `RSRC1`. What it modifies is the SGPR
allocation and the vertex-component configuration, not the VGPR count.

## What A7 got right and is preserved

- The command-list append mechanism at `0x13D6906`.
- The bump allocator at `+0x30`/`+0x38` and the `0x130`/`0x138` reconciliation.
- The null-pixel-shader branch and its PM4 consequence.
- The shader program pointer chain `context+0x468` → `+0x200` → `+0x1D8` → `+0x40`.
- The selector arithmetic and the `+0x5E` flag table.

## What A7 got wrong and is superseded

- That `0x14B00` is the RHI command executor. It is not.
- That the executor walks an array at `+0x18`/`+0x20` of the command list. It does not.
- That array order versus list order was an open question. The real executor walks the list.
- The RSRC1 field attribution.

## Confirmed versus inferred

Confirmed by bytes:

- `0x14B00` receives `object+0x38` and reads `+0x18`/`+0x20` of that, i.e. `+0x50`/`+0x58`.
- `0x14B00` reads `+0x48` of each dispatched object.
- `0x14B00` does not set `%rsi` before the dispatch.
- `0x13764D0` walks `Next` at `+0x08` from `(%r15)` and sets `%rsi = %r15`.
- The wrapper at `0x13A68B0` reads `0x20(%rsi)`.
- The list fields at `+0x00`, `+0x08`, `0x10`, `0x14`, `0x30`.
- The RSRC1 mask bits.

Inferred:

- That `0x13764D0` is `FRHICommandListExecutor::ExecuteList`. The shape and convention match;
  the executable is stripped.
- That `0x14B00` is a task/job dispatcher. Its object type was not identified.

## Next targets

1. Trace the caller of `0x13764D0` to establish the execution thread.
2. Confirm the `0x14810` teardown's relationship to this executor.
3. Identify the `0x14B00` object type, so the misidentification is fully closed.
4. Then, and only then, resume command ordering and sorting analysis.

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x13764D0 0x14B00 0x14810
llvm-objdump -d --start-address=0x137692F --stop-address=0x1376980 v100.elf
llvm-objdump -d --start-address=0x1485E --stop-address=0x14870 v100.elf
llvm-objdump -d --start-address=0x14B40 --stop-address=0x14BA8 v100.elf
```
