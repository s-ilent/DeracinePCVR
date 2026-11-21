# A4 correction: command identity and shader-path semantics

This supersedes `A4_phase3_shader_state.md`. Three findings there were wrong. The static facts
(addresses, call sites, table contents) stand; the semantics attached to them did not.

## Correction 1: the `+0x18` callback is a space reservation

The previous report read `testb %al,%al` / `je` as "non-zero means handled, skip the GNM
fallback". That is backwards. The surrounding code shows why.

The full pixel-shader block, verbatim from the 1.00 image:

```
cb6ffd: 49 8b 07              movq   (%r15), %rax        ; buffer end
cb7000: 48 29 f8              subq   %rdi, %rax
cb7003: 48 c1 e8 02           shrq   $0x2, %rax          ; free dwords
cb7007: 83 f8 27              cmpl   $0x27, %eax         ; 39 dwords
cb700a: 77 18                 ja     0xcb7024            ; enough -> skip callback
cb700c: 49 8b 55 20           movq   0x20(%r13), %rdx
cb7010: be 28 00 00 00        movl   $0x28, %esi         ; need 40 dwords
cb7015: 4c 89 ef              movq   %r13, %rdi
cb7018: 41 ff 55 18           callq  *0x18(%r13)         ; grow buffer
cb701c: 84 c0                 testb  %al, %al
cb701e: 74 1c                 je     0xcb703c            ; grow failed -> skip write
cb7020: 49 8b 3c 24           movq   (%r12), %rdi        ; reload cursor
cb7024: be 28 00 00 00        movl   $0x28, %esi
cb7029: 31 d2                 xorl   %edx, %edx
cb702b: e8 10 13 71 02        callq  sceGnmSetPsShader350
```

`cmpl $0x27` / `ja` is a capacity check. With 39 or more dwords free the callback is skipped.
Otherwise it is asked to make room for 40. Non-zero means the buffer grew, so the write
proceeds. Zero means it could not grow, so the write is skipped.

The callback is a **buffer-growth** function. It is not a shader setter and not a semantic
interception point.

The same shape repeats at every GNM write, with the threshold always one less than the request:

| Call site | Threshold | Requested |
| --- | --- | --- |
| `0xCB5820` | `0x11` | `0x12` |
| `0xCB5971` | `0x0D` | `0x0E` |
| `0xCB60FF` | `0x27` | `0x28` |
| `0xCB6144` | `0x1C` | `0x1D` |
| `0xCB6260` | `0x06` | `0x07` |

That one-less relationship is the signature of "is there room for N dwords".

The null register-array argument is consistent with this. `%edx = 0` because the registers were
already written into the command buffer by the preceding `SetShReg` sequence, not because
shader state is being disabled.

## Correction 2: the GNM stubs are now named

Using the existing SELF-aware resolver (`tools/ps4elf.mjs imports`), not a new parser:

| Stub | Name |
| --- | --- |
| `0x33C81A0` | `sceGnmDrawIndex` |
| `0x33C81B0` | `sceGnmSetVsShader` |
| `0x33C81D0` | `sceGnmSetEmbeddedVsShader` |
| `0x33C8270` | `sceGnmDrawIndexAuto` |
| `0x33C82C0` | `sceGnmInsertPopMarker` |
| `0x33C8340` | `sceGnmSetPsShader350` |
| `0x33C8380` | `sceGnmInsertPushMarker` |
| `0x33C88A0` | `__stack_chk_fail` |

This changes the character of `0x0CB5130`. It is not a shader-state function. It emits push and
pop markers, auto draws, embedded vertex shaders, and both shader setters. It is a general
command-buffer recording routine with mixed responsibilities.

## Correction 3: `0x13A68B0` is `FRHICommandDrawIndexedPrimitive::Execute`

The field offsets match the pinned UE4.18.3 struct exactly:

| Offset | UE4.18.3 field | Wrapper read |
| --- | --- | --- |
| `+0x10` | `IndexBuffer` | `movq 0x10(%rdi), %rsi` |
| `+0x18` | `PrimitiveType` | `movl 0x18(%rdi), %edx` |
| `+0x1C` | `BaseVertexIndex` | `movl 0x1c(%rdi), %ecx` |
| `+0x20` | `FirstInstance` | `movl 0x20(%rdi), %r8d` |
| `+0x24` | `NumVertices` | `movl 0x24(%rdi), %r9d` |
| `+0x28` | `StartIndex` | `movl 0x28(%rdi), %r10d` |
| `+0x2C` | `NumPrimitives` | `movl 0x2c(%rdi), %r11d` |
| `+0x30` | `NumInstances` | `movl 0x30(%rdi), %ebx` |

The forwarding call matches the public `Execute` contract: command list in `%rsi`, context
loaded from `0x20(%rsi)`, eight fields forwarded to `RHIDrawIndexedPrimitive`.

This is a strong identification, but still an inference. The executable is stripped, so the
name comes from layout and contract agreement with the pinned public source.

## Correction 4: the header is a vtable, but not the one I described

This is where I must partly disagree with the review premise, and the construction site is the
evidence.

The public `FRHICommandBase` is `{Next, ExecuteAndDestructPtr}`. The target object is not that
shape. The constructor writes:

```
141015c: 48 8d 50 08           leaq   0x8(%rax), %rdx      ; rdx = obj + 8
1410160: 48 8d 35 ...          leaq   0x3f26a88(%rip), %rsi ; rsi = vtable
1410173: 48 8b 4b 08           movq   0x8(%rbx), %rcx      ; rcx = list tail slot
1410177: 48 89 01              movq   %rax, (%rcx)         ; *tail = obj
141017a: 48 89 53 08           movq   %rdx, 0x8(%rbx)      ; tail = &obj->Next
1410188: 48 c7 40 08 00 00 00 00  movq $0x0, 0x8(%rax)     ; obj->Next = null
1410190: 48 89 30              movq   %rsi, (%rax)         ; obj->vptr = 0x3F26A88
1410193: 4c 89 70 10           movq   %r14, 0x10(%rax)     ; IndexBuffer
1410197: c5 f8 11 40 18        vmovups %xmm0, 0x18(%rax)   ; PrimitiveType..NumVertices
141019f: 48 89 50 28           movq   %rdx, 0x28(%rax)     ; StartIndex/NumPrimitives
14101a3: c7 40 30 01 00 00 00  movl   $0x1, 0x30(%rax)     ; NumInstances
```

So the layout is:

| Offset | Field |
| --- | --- |
| `+0x00` | vptr = `0x3F26A88` |
| `+0x08` | `Next` (link) |
| `+0x10` | `IndexBuffer` |
| `+0x18` | `PrimitiveType` |
| `+0x1C` | `BaseVertexIndex` |
| `+0x20` | `FirstInstance` |
| `+0x24` | `NumVertices` |
| `+0x28` | `StartIndex` |
| `+0x2C` | `NumPrimitives` |
| `+0x30` | `NumInstances` |

`0x3F26A88` is a **vtable**, and it is a two-entry command vtable:

| Slot | Value | Role |
| --- | --- | --- |
| `[0]` | `0x13A68B0` | `Execute` |
| `[1]` | `0x13A68F0` | destructor (`push rbp; mov rsp,rbp; pop rbp; ret`) |

The two dispatch loops confirm the roles:

```
; execute: call vtable[0]
14b9f: 48 8b 07              movq   (%rdi), %rax        ; rax = obj->vptr
14ba2: ff 10                 callq  *(%rax)             ; call vtable[0]

; teardown: call vtable[1]
14848: 48 8b 07              movq   (%rdi), %rax        ; rax = obj->vptr
1484b: ff 50 08              callq  *0x8(%rax)          ; call vtable[1]
```

So this build uses a vtable-based command object, not the `ExecuteAndDestructPtr` function
pointer that the public 4.18.3 header declares. The `Next` link is at `+0x08`, not `+0x00`.

That is a real divergence from the public source, and it is worth stating plainly: the public
header is a guide, not a specification of this binary.

The vtable is not isolated. The same region holds a cluster of command vtables:

| Vtable | Execute | Dtor |
| --- | --- | --- |
| `0x3F26A08` | `0x13A6BB0` | `0x13A6BE0` |
| `0x3F26A28` | `0x13A6A30` | `0x13A6A70` |
| `0x3F26A48` | `0x13A6930` | `0x13A6A20` |
| `0x3F26A68` | `0x13A6900` | `0x13A6920` |
| `0x3F26A88` | `0x13A68B0` | `0x13A68F0` |

Five adjacent command vtables, each with an `Execute` and a trivial destructor. That is the
deferred command list.

## Correction 5: the runtime stack argument

The previous report claimed `stack_args[4] = 0x803F26A88` proved virtual dispatch. It does not
prove it on its own.

At the draw method's entry, `stack_args[4]` is the word at `entry_rsp + 0x28`. The wrapper
pushes `%rbx`, `%r11`, `%r10` after `subq $0x8`, so that slot holds the wrapper's saved `%rax`,
loaded from `0x20(%rsi)` — the command list's context pointer, not a declared draw argument.

The value matching `0x803F26A88` is a useful clue: it says the context pointer and the command
vtable address coincide in that frame. But the dispatch is now established directly from the
execute loop, so the stack argument is corroboration, not proof.

## What the command stream actually is

- `FRHICommandListBase` holds a singly linked list of command objects.
- Each object is `{vptr, Next}` followed by its payload.
- `FRHICommandDrawIndexedPrimitive` payload is the eight fields from `+0x10`.
- Execution walks the list and calls `vtable[0]`.
- Teardown calls `vtable[1]`.
- `Execute` forwards to `IRHICommandContext::RHIDrawIndexedPrimitive`, the PS4 implementation at
  `0x0CBB9C0`.

The semantic layer is the command list. The interception point is the command object's
`Execute`, reached through `vtable[0]`.

## Adjacent command types

The five adjacent vtables are not anonymous. Disassembling each `Execute` shows a distinct
payload shape, and two match public UE4.18 command structs exactly.

### `0x3F26A88` — `FRHICommandDrawIndexedPrimitive`

Eight fields from `+0x10`, as tabulated above. Forwards to `0x0CBB9C0`.

### `0x3F26A08` — `FRHICommandSetViewport`

```
13a6bb4: 48 8b 46 20        movq   0x20(%rsi), %rax     ; context
13a6bb8: 8b 77 10           movl   0x10(%rdi), %esi     ; MinX
13a6bbb: 8b 57 14           movl   0x14(%rdi), %edx     ; MinY
13a6bbe: c5 fa 10 47 18     vmovss 0x18(%rdi), %xmm0   ; MinZ (float)
13a6bc3: 8b 4f 1c           movl   0x1c(%rdi), %ecx     ; MaxX
13a6bc6: 44 8b 47 20        movl   0x20(%rdi), %r8d     ; MaxY
13a6bca: c5 fa 10 4f 24     vmovss 0x24(%rdi), %xmm1   ; MaxZ (float)
13a6bd3: e9 08 1f 91 ff     jmp    0x0CB8AE0
```

Six fields, two of them floats, in the exact order of the public `FRHICommandSetViewport`
(`MinX, MinY, MinZ, MaxX, MaxY, MaxZ`). This is a strong match.

### `0x3F26A68` — four-integer command

```
13a6904: 48 8b 46 20        movq   0x20(%rsi), %rax
13a6908: 8b 77 10           movl   0x10(%rdi), %esi
13a690b: 8b 57 14           movl   0x14(%rdi), %edx
13a690e: 8b 4f 18           movl   0x18(%rdi), %ecx
13a6911: 44 8b 47 1c        movl   0x1c(%rdi), %r8d
13a6919: e9 b2 41 91 ff     jmp    0x0CBAAD0
```

Four consecutive 32-bit fields. That matches `FRHICommandDrawPrimitive`
(`PrimitiveType, BaseVertexIndex, NumPrimitives, NumInstances`) by shape, though the target
`0x0CBAAD0` was not decoded far enough to confirm the semantics.

### `0x3F26A48` — larger command with a resource pointer

```
13a693e: 4c 8b 6e 20        movq   0x20(%rsi), %r13
13a6942: 44 8b 7f 10        movl   0x10(%rdi), %r15d
13a6946: 48 8b 5f 18        movq   0x18(%rdi), %rbx     ; a resource pointer
13a694a: 43 0f b7 8c 7d a8 00 00 00  movzwl 0xa8(%r13,%r15,2), %ecx
13a6958: 48 83 c3 98        addq   $-0x68, %rbx         ; adjust to the resource base
```

An index plus a pointer with a `-0x68` adjustment. That is consistent with a stream-source or
resource-binding command, where the payload holds a reference-counted resource whose interface
pointer sits at a fixed offset.

### `0x3F26A28` — multi-field command with embedded arrays

```
13a6a3c: 8b 77 10           movl   0x10(%rdi), %esi
13a6a3f: 44 8b 87 e8 00 00 00  movl   0xe8(%rdi), %r8d
13a6a46: 48 8d 57 18        leaq   0x18(%rdi), %rdx     ; array at +0x18
13a6a4a: 48 8d 8f d8 00 00 00  leaq   0xd8(%rdi), %rcx     ; array at +0xD8
13a6a51: 4c 8d 8f f0 00 00 00  leaq   0xf0(%rdi), %r9      ; array at +0xF0
```

Three embedded arrays plus a count. That is the shape of `FRHICommandSetRenderTargets`, which
carries `NewRenderTargetsRHI[MaxSimultaneousRenderTargets]`, a depth target, and a UAV array.

So the command list is recoverable. Four of the five adjacent vtables have a plausible public
UE4.18 identity, and the fifth is a resource-binding command.

## Strongest next semantic command candidate

`FRHICommandSetBoundShaderState` remains the best next target, but it is not in this cluster.

- It binds the vertex and pixel shader pair, which is the state the draw depends on.
- Its payload is a single `FBoundShaderStateRHIParamRef`, so the layout is small and checkable.
- The shader setters already observed are the backend consequence of this command.

The cluster found here gives a faster route to the render-target and viewport state, which is
the other half of a pass boundary. `0x3F26A28` (render targets) and `0x3F26A08` (viewport) are
already identified and can be instrumented without further search.

## Uncertainties

- The `FRHICommandDrawIndexedPrimitive` identification is by layout and contract, not symbol.
- The vtable-based header diverges from public 4.18.3. Whether this is a FromSoftware
  modification or a different UE revision is not established.
- `0x3F26A68` and `0x3F26A48` are matched by shape only; their targets were not fully decoded.
- Whether `0x0CB5130` is reached from a command node or only from direct calls is unresolved.
- The `+0x18` buffer-growth callback's owner object is not identified.

## Reproducible checks

```
node tools/ps4elf.mjs imports <eboot.bin>
llvm-objdump -d --start-address=0xCB6FFD --stop-address=0xCB7034 v100.elf
llvm-objdump -d --start-address=0x141015C --stop-address=0x14101A8 v100.elf
llvm-objdump -d --start-address=0x14B97 --stop-address=0x14BA8 v100.elf
llvm-objdump -d --start-address=0x14848 --stop-address=0x14850 v100.elf
python tools/a4/cmdvtables.py v100.elf
```

The import resolver needs `shadps4-arm64-main/scripts/aerolib.inl` relative to `tools/`. That
path is absent in a fresh checkout; copy `src/scripts/aerolib.inl` there.
