# A5 Phase 1: render-target and viewport commands

Both candidates are confirmed. `0x03F26A28` is `FRHICommandSetRenderTargets` and `0x03F26A08`
is `FRHICommandSetViewport`. The payload layouts are recovered from the constructors, not
guessed from the public header.

All addresses are inner-ELF VAs from the pinned 1.00 image
(`8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`). Function bounds come
from the `.eh_frame_hdr` binary search table, so they are independently confirmed rather than
inferred from padding.

## Command 1: `FRHICommandSetRenderTargets`

### Vtable and bounds

| Item | Value |
| --- | --- |
| Vtable | `0x03F26A28` |
| `Execute` | `0x13A6A30` |
| Destructor | `0x13A6A70` |
| `Execute` bounds | `[0x13A6A30, 0x13A6A70)` |
| Receiver | `0x0CC2420` |
| Receiver bounds | `[0x0CC2420, 0x0CC2C80)` |

### Execute

```
13a6a30: 55                    pushq  %rbp
13a6a31: 48 89 e5              movq   %rsp, %rbp
13a6a34: 48 83 ec 10           subq   $0x10, %rsp
13a6a38: 48 8b 46 20           movq   0x20(%rsi), %rax   ; context from CmdList
13a6a3c: 8b 77 10              movl   0x10(%rdi), %esi   ; arg1 = count
13a6a3f: 44 8b 87 e8 00 00 00  movl   0xe8(%rdi), %r8d   ; arg4 = numUAVs
13a6a46: 48 8d 57 18           leaq   0x18(%rdi), %rdx   ; arg2 = &RT[0]
13a6a4a: 48 8d 8f d8 00 00 00  leaq   0xd8(%rdi), %rcx   ; arg3 = &depth
13a6a51: 4c 8d 8f f0 00 00 00  leaq   0xf0(%rdi), %r9    ; arg5 = &UAV[0]
13a6a58: c7 04 24 00 00 00 00  movl   $0x0, (%rsp)       ; stack arg = 0
13a6a5f: 48 89 c7              movq   %rax, %rdi         ; this = context
13a6a62: e8 b9 b9 91 ff        callq  0x0CC2420
13a6a67: 48 83 c4 10           addq   $0x10, %rsp
13a6a6b: 5d                    popq   %rbp
13a6a6c: c3                    retq
```

Five register arguments plus one stack argument. The stack argument is always zero at this call
site, so its meaning is not established here.

### Payload layout

Recovered from the constructor at `0x13D69C2`, which allocates `0x130` bytes and writes:

| Offset | Size | Field | Evidence |
| --- | --- | --- | --- |
| `+0x00` | 8 | vptr = `0x03F26A28` | `movq %rsi, (%rax)` |
| `+0x08` | 8 | `Next` = 0 | `movq $0x0, 0x8(%rax)` |
| `+0x10` | 4 | `NewNumSimultaneousRenderTargets` | `movl $0x1, 0x10(%rax)` |
| `+0x14` | 4 | padding | not written |
| `+0x18` | `0xC0` | `NewRenderTargetsRHI[8]`, stride `0x18` | eight 24-byte entries initialised |
| `+0xD8` | `0x18` | `NewDepthStencilTarget` | `leaq 0xd8(%rdi), %rcx` |
| `+0xF0` | `0x38` | `UAVs[]` | `leaq 0xf0(%rdi), %r9` |
| `+0xE8` | 4 | `NewNumUAVs` | `movl $0x0, 0xe8(%rax)` |

The render-target array is eight entries of 24 bytes, which matches
`MaxSimultaneousRenderTargets == 8` in the public header. The constructor initialises each
entry with a repeating pattern:

```
+0x00  movq $0x0        ; resource pointer
+0x08  movq %r14        ; (a second pointer, r14)
+0x10  movw $0x0        ; two 16-bit fields
```

So each `FRHIRenderTargetView` entry is `{Texture*, ?, uint16, uint16}` at 24 bytes. The
`Execute` loop reads `(%r15)` as the resource pointer, `0x8(%r15)` and `0xc(%r15)` as two
32-bit values, and advances by `0x18`.

The constructor's final writes confirm the tail fields:

```
13d6aa8: movq %r8, 0x18(%rax)      ; RT[0].resource
13d6aac: movq %r14, 0x20(%rax)     ; RT[0].second pointer
13d6ab0: movw $0x101, 0x28(%rax)   ; RT[0].two 16-bit fields = 0x01, 0x01
13d6ab6: movq %r15, 0xd8(%rax)     ; depth target resource
13d6abd: movq %rdx, 0xe0(%rax)     ; depth target second pointer
```

So the first render target is populated with `0x101` in its two 16-bit fields, and the depth
target is a two-pointer pair at `+0xD8`/`+0xE0`.

### Receiver behaviour

The receiver at `0x0CC2420` is a real RHI method, not a thunk. It:

1. Saves all five arguments and the stack byte (`movb 0x10(%rbp), %r12b`).
2. Stores the count to `0x474(%r13)`.
3. Loops over the render targets, calling `0x0CC7E00` per slot with the index.
4. Accumulates a bitmask in `0x518(%r13)` via `orl %eax, 0x518(%r13)` with `0xf << index`.
5. Compares the new mask against the cached one at `0x510(%r13)` and only emits a PM4 packet
   when it changed.
6. Emits `movabsq $0x8EC0016900` followed by the mask, advancing the cursor by `0xc`.
7. Handles the depth-stencil target separately, emitting `0x3531A20` (16 bytes) plus
   `movq $0x11` and advancing by `0x18`.

The bitmask comparison is a real state-cache optimisation: an unchanged render-target set
produces no packet. That matters for any future instrumentation, because a render-target change
is not one packet per call.

### Confirmed versus inferred

Confirmed by bytes: the vtable, the `Execute` bounds, the five argument registers, the payload
offsets `+0x10`, `+0x18`, `+0xD8`, `+0xE8`, `+0xF0`, the 24-byte RT stride, and the eight-entry
array.

Inferred from the public header: the field names. The offsets and sizes agree with
`FRHICommandSetRenderTargets`, but the executable is stripped, so the names are a mapping, not a
symbol.

## Command 2: `FRHICommandSetViewport`

### Vtable and bounds

| Item | Value |
| --- | --- |
| Vtable | `0x03F26A08` |
| `Execute` | `0x13A6BB0` |
| Destructor | `0x13A6BE0` |
| `Execute` bounds | `[0x13A6BB0, 0x13A6BE0)` |
| Receiver | `0x0CB8AE0` |
| Receiver bounds | `[0x0CB8AE0, 0x0CB9150)` |

### Execute

```
13a6bb0: 55                    pushq  %rbp
13a6bb1: 48 89 e5              movq   %rsp, %rbp
13a6bb4: 48 8b 46 20           movq   0x20(%rsi), %rax   ; context
13a6bb8: 8b 77 10              movl   0x10(%rdi), %esi   ; MinX
13a6bbb: 8b 57 14              movl   0x14(%rdi), %edx   ; MinY
13a6bbe: c5 fa 10 47 18        vmovss 0x18(%rdi), %xmm0   ; MinZ (float)
13a6bc3: 8b 4f 1c              movl   0x1c(%rdi), %ecx   ; MaxX
13a6bc6: 44 8b 47 20           movl   0x20(%rdi), %r8d   ; MaxY
13a6bca: c5 fa 10 4f 24        vmovss 0x24(%rdi), %xmm1   ; MaxZ (float)
13a6bcf: 48 89 c7              movq   %rax, %rdi
13a6bd2: 5d                    popq   %rbp
13a6bd3: e9 08 1f 91 ff        jmp    0x0CB8AE0
```

Six fields in the exact public order `MinX, MinY, MinZ, MaxX, MaxY, MaxZ`, with `MinZ` and
`MaxZ` passed as floats in `%xmm0` and `%xmm1`. The tail `jmp` makes this a tail call, so the
receiver returns directly to the command-list executor.

### Payload layout

| Offset | Size | Field |
| --- | --- | --- |
| `+0x00` | 8 | vptr = `0x03F26A08` |
| `+0x08` | 8 | `Next` |
| `+0x10` | 4 | `MinX` |
| `+0x14` | 4 | `MinY` |
| `+0x18` | 4 | `MinZ` (float) |
| `+0x1C` | 4 | `MaxX` |
| `+0x20` | 4 | `MaxY` |
| `+0x24` | 4 | `MaxZ` (float) |

### Receiver behaviour

The receiver at `0x0CB8AE0` converts the integer bounds to floats and compares them against
cached state at `0x80`, `0x84`, `0x88`, `0x8C`, `0xA0`, `0xA4` of the context. If all six match,
it jumps to `0x0CB8FC9` and emits nothing. Otherwise it:

1. Computes the viewport extent as `MaxX - MinX` and `MaxY - MinY`.
2. Scales by constants at `0x35AF508` and `0x35AF50C`.
3. Emits a 12-dword packet starting with `movabsq $0xB4C0026900` (a `SET_CONTEXT_REG` for
   `PA_CL_VPORT_XSCALE`-class registers) and `movabsq $0x10FC0066900`.
4. Writes the six floats plus `0x3F800000` (1.0f) and advances the cursor by `0x20`.
5. Clamps the scissor to `0x1FC` (508) with `cmovgel`, rounding down to a multiple of 4 via
   `andl $-0x4`.

So the viewport command also updates scissor state, and it is cached the same way as the
render-target set.

### Confirmed versus inferred

Confirmed: the vtable, bounds, six fields and their order, the float/int split, the tail call,
the cached-state offsets, and the PM4 register writes.

Inferred: the field names, from the public header. The receiver's exact RHI method name is not
recovered; it is identified by address and behaviour.

## Ordering and the pass boundary

The two commands are adjacent in the vtable cluster and both are state-cached, but **this does
not establish that they form a render-pass boundary**.

What is established:

- Both are deferred commands in the same list, dispatched through `vtable[0]`.
- Both cache their state and suppress redundant packets.
- Both write PM4 context registers through the same command-buffer writer.

What is not established:

- That they are emitted in a fixed order relative to each other.
- That a render pass is delimited by them rather than by `FRHICommandBeginRenderPass`.
- Whether `FRHICommandBeginRenderPass` exists in this binary at all.

The public header has explicit `FRHICommandBeginRenderPass` and `FRHICommandEndRenderPass`
commands. Those are the actual pass boundary in UE4.18, not the render-target setter. So the
render-target and viewport commands are pass *state*, not pass *delimiters*.

## Next targets

1. Find `FRHICommandBeginRenderPass` / `FRHICommandEndRenderPass` in the same vtable cluster.
   They carry `FRHIRenderPassInfo`, which is the real pass boundary.
2. `FRHICommandSetBoundShaderState`, which binds the shader pair the draw depends on.
3. `0x03F26A48`, the resource-binding candidate (Phase 2).

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x13A6A30 0x13A6BB0 0xCC2420 0xCB8AE0
llvm-objdump -d --start-address=0x13A6A30 --stop-address=0x13A6A70 v100.elf
llvm-objdump -d --start-address=0x13A6BB0 --stop-address=0x13A6BE0 v100.elf
llvm-objdump -d --start-address=0x13D6980 --stop-address=0x13D6AA0 v100.elf
llvm-objdump -d --start-address=0xCC2420 --stop-address=0xCC2620 v100.elf
llvm-objdump -d --start-address=0xCB8AE0 --stop-address=0xCB8D40 v100.elf
```
