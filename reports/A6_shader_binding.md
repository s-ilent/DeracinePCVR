# A6: shader binding and the command stream

## Result

`FRHICommandSetBoundShaderState` is **not** a separate deferred command in this binary.

The shader binding happens inside the recording routine at `0x0CB5130`, which is itself vtable
slot `+0x340` of the context vtable. There is no command object whose payload is a
`FBoundShaderStateRHIParamRef`. The public UE4.18.3 header declares such a command; this build
does not use it on the path that reaches the draw.

That is a negative result for the primary target, and it is the more useful finding: the chain
I proposed in A5 was wrong about this link.

## What was searched

1. The command vtable cluster around `0x03F26A00`. It contains the five A5 commands plus
   destructors, but no shader-binding command.
2. Every call site of `sceGnmSetVsShader` (`0x033C81B0`). There are five:
   `0x0CB7075`, `0x0CB7DE2`, `0x0CCD139`, `0x0CCD201`, `0x2F60CD7`.
3. Every call site of `sceGnmSetPsShader350` (`0x033C8340`).
4. Pointer tables for the recording routine and for the shader receiver.

The recording routine `0x0CB5130` is stored at exactly one pointer slot: `0x03EDFFE0`, which is
`+0x340` of the context vtable `0x03EDFCA0`. The shader receiver `0x0CCC2F0` is stored in no
pointer table at all, so it is reached only by direct call.

## The actual shader binding path

The receiver at `0x0CCC2F0` (FDE `[0x0CCC2F0, 0x0CCD480)`) is called from three sites, all
inside the recording routine:

| Call site | Containing function |
| --- | --- |
| `0x0CB6502` | `0x0CB5130` |
| `0x0CB7453` | `0x0CB5130` |
| `0x0CB804F` | `0x0CB5130` |

So the shader state is bound by the same routine that emits the GNM setters, not by a deferred
command object.

## The binding edge

This is the definite ownership edge the task asked for. The chain is:

```
context + 0x468  ->  shader-state object
shader-state + 0x200  ->  shader resource (r13)
shader-state + 0x1C0  ->  a 32-bit value passed as ecx (r15d)
shader resource + 0x1D8  ->  program data (r12)
program data + 0x3  ->  a byte index (ebx)
program data + 0x40 + index*4  ->  register array passed as rdx
```

The code:

```
ccd0d4: 48 8b 80 68 04 00 00   movq   0x468(%rax), %rax    ; shader-state object
ccd0db: 4c 8b a8 00 02 00 00   movq   0x200(%rax), %r13    ; shader resource
ccd0e2: 4d 85 ed              testq  %r13, %r13
ccd0e5: 0f 84 bf 00 00 00     je     0xccd1aa             ; null -> skip
ccd0eb: 44 8b b8 c0 01 00 00  movl   0x1c0(%rax), %r15d   ; a 32-bit field
ccd0f6: 4d 8b a5 d8 01 00 00  movq   0x1d8(%r13), %r12    ; program data
ccd103: 41 0f b6 5c 24 03     movzbl 0x3(%r12), %ebx      ; index byte
ccd12c: 49 8d 54 9c 40        leaq   0x40(%r12,%rbx,4), %rdx  ; register array
ccd131: be 1d 00 00 00        movl   $0x1d, %esi          ; size = 29
ccd136: 44 89 f9              movl   %r15d, %ecx          ; the 32-bit field
ccd139: e8 72 b0 6f 02        callq  0x033C81B0           ; sceGnmSetVsShader
```

So a UE4 shader resource object owns a program-data block at `+0x1D8`, and that block holds a
register array at `+0x40` indexed by a byte at `+0x3`. The GNM setter receives a pointer into
that array, not a copy.

That is a real ownership edge: the register array lives inside the shader resource, and the
shader resource is referenced from the context's shader-state object at `+0x200`.

## UE4 semantic objects versus PS4 register arrays

The distinction the task asked for, now grounded:

| Layer | Where | What it is |
| --- | --- | --- |
| UE4 shader resource | `shader-state + 0x200` | a reference-counted object |
| Program data | `shader resource + 0x1D8` | the PS4-side program block |
| Register array | `program data + 0x40 + index*4` | raw GCN register values |
| GNM call | `sceGnmSetVsShader` | writes those registers into PM4 |

The UE4 object is the shader resource. The register array is a field inside its program data.
They are not the same thing, and the register array is not a separate allocation.

The byte at `program data + 0x3` selects which register array to use, and the code at
`0x0CCD14C` reads a per-index flag byte at `program data + 0x5E + index*4` to decide whether a
variant path applies. So one shader resource can carry multiple register arrays.

## The pixel-shader path

The second call site uses the same program data with a different offset:

```
ccd1f0: 49 8b 3e              movq   (%r14), %rdi        ; cmdbuf
ccd1f3: 49 83 c4 08           addq   $0x8, %r12          ; program data + 8
ccd1f7: be 1d 00 00 00        movl   $0x1d, %esi         ; size = 29
ccd1fc: 89 d9                 movl   %ebx, %ecx
ccd1fe: 4c 89 e2              movq   %r12, %rdx          ; register array
ccd201: e8 aa af 6f 02        callq  0x033C81B0          ; sceGnmSetVsShader
```

Note this is still `sceGnmSetVsShader`, with `%r12` advanced by 8. So the same program block
carries both stages' register arrays at fixed offsets, and the "pixel" path here is a second
vertex-shader variant, not the pixel shader.

The pixel shader is set at `0x0CCD0C0`, and it passes a **null** register array:

```
ccd0b9: be 28 00 00 00        movl   $0x28, %esi         ; size = 40
ccd0be: 31 d2                 xorl   %edx, %edx          ; register array = null
ccd0c0: e8 7b b2 6f 02        callq  0x033C8340          ; sceGnmSetPsShader350
```

So the pixel-shader registers are written by a preceding `SetShReg` sequence, and the setter
call itself carries no array. That matches the A4 correction: the null argument is not a
disable, it is a consequence of the registers already being in the command buffer.

## Render-pass boundary

`FRHICommandBeginRenderPass` and `FRHICommandEndRenderPass` were **not found**.

The command vtable cluster contains five commands, all of which are state setters or draws. No
command carries a `FRHIRenderPassInfo`-shaped payload. The public header declares these
commands, but this build does not appear to use them on the observed path.

What the binary does instead is not yet established. The render-target and viewport commands
are state-cached and emit PM4 context registers directly, which is consistent with a backend
that treats a render pass as an implicit consequence of binding targets rather than an explicit
command. That is a hypothesis, not a finding.

## Command ordering

Not established. The command list is a singly linked list, and the execute loop walks it, but
the head and tail pointers were not traced to a producer. A recovered set of command types is
not an observed sequence, and this report does not claim one.

## Confirmed versus inferred

Confirmed by bytes:

- The recording routine `0x0CB5130` is vtable slot `+0x340` of `0x03EDFCA0`.
- The shader receiver `0x0CCC2F0` is called only from inside that routine.
- The field chain `context+0x468` → `+0x200` → `+0x1D8` → `+0x40`.
- The register-array pointer arithmetic and the `0x1D` size.
- The absence of a shader-binding command in the vtable cluster.

Inferred:

- That `+0x200` is a "shader resource" and `+0x1D8` is "program data". The names come from
  behaviour, not symbols.
- That the byte at `+0x3` is a register-array selector. It is used as an index, but its full
  meaning is not established.
- That the absence of `BeginRenderPass` means the backend uses implicit passes.

## Next targets

1. Trace the command list head and tail to a producer. Without that, ordering is unproven.
2. Decode `0x0CCD0C0`, the pixel-shader setter, to complete the shader picture.
3. Determine what `shader-state + 0x1C0` is. It is passed as the GNM `flags` argument.
4. Look for the mechanism that delimits a pass, given that `BeginRenderPass` is absent.

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x0CCC2F0 0x0CB5130
llvm-objdump -d --start-address=0xCCD0C0 --stop-address=0xCCD240 v100.elf
llvm-objdump -d --start-address=0xCCC2F0 --stop-address=0xCCC3A0 v100.elf
```
