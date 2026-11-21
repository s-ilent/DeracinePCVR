# A5 Phase 2: the resource-binding candidate at 0x03F26A48

## Result

`0x03F26A48` is **`FRHICommandSetStreamSource`**, not a texture or resource-binding command.

The `-0x68` adjustment that A4 flagged is not a resource-interface offset. It is the standard
`FVertexBuffer` to `FRHIResource` back-pointer adjustment, and the code that follows computes a
vertex count from a stride. That is stream-source behaviour, not texture binding.

## Vtable and bounds

| Item | Value |
| --- | --- |
| Vtable | `0x03F26A48` |
| `Execute` | `0x13A6930` |
| Destructor | `0x13A6A20` |
| `Execute` bounds | `[0x13A6930, 0x13A6A20)` |
| Constructor | `0x141009F` |

## Payload layout

From the constructor at `0x141009F`, which allocates `0x28` bytes:

```
14100b9: 48 89 10              movq   %rdx, (%rax)        ; vptr = 0x3F26A48
14100bc: c7 40 10 00 00 00 00  movl   $0x0, 0x10(%rax)    ; StreamIndex = 0
14100c3: 4c 89 70 18           movq   %r14, 0x18(%rax)    ; VertexBuffer
14100c7: c7 40 20 00 00 00 00  movl   $0x0, 0x20(%rax)    ; Offset = 0
```

| Offset | Size | Field |
| --- | --- | --- |
| `+0x00` | 8 | vptr = `0x03F26A48` |
| `+0x08` | 8 | `Next` |
| `+0x10` | 4 | `StreamIndex` |
| `+0x14` | 4 | padding |
| `+0x18` | 8 | `VertexBuffer` |
| `+0x20` | 4 | `Offset` |

That is exactly the public `FRHICommandSetStreamSource` payload
(`StreamIndex`, `VertexBuffer`, `Offset`), and it is **not** the deprecated variant, which also
carries a `Stride`. The stride is read from the buffer object instead.

## Execute

```
13a693e: 4c 8b 6e 20           movq   0x20(%rsi), %r13    ; context
13a6942: 44 8b 7f 10           movl   0x10(%rdi), %r15d   ; StreamIndex
13a6946: 48 8b 5f 18           movq   0x18(%rdi), %rbx    ; VertexBuffer
13a694a: 43 0f b7 8c 7d a8 00 00 00  movzwl 0xa8(%r13,%r15,2), %ecx  ; per-stream stride
13a6953: 48 85 db              testq  %rbx, %rbx
13a6956: 74 42                 je     0x13a699a           ; null buffer -> unbind
13a6958: 48 83 c3 98           addq   $-0x68, %rbx        ; FVertexBuffer -> FRHIResource
13a695c: 74 3c                 je     0x13a699a
13a695e: 44 8b 67 20           movl   0x20(%rdi), %r12d   ; Offset
13a6962: 44 8b 73 7c           movl   0x7c(%rbx), %r14d   ; buffer size
13a6966: 45 29 e6              subl   %r12d, %r14d        ; size - offset
```

The `-0x68` adjustment converts the `FVertexBuffer` reference to the underlying `FRHIResource`
base. The field at `0x7c` of that base is the buffer size in bytes.

## The stride computation

The decisive evidence is the division at the end:

```
13a69c7: 4c 03 23              addq   (%rbx), %r12       ; offset += base address
13a69ce: 66 85 c9              testw  %cx, %cx           ; stride == 0?
13a69d1: 4d 89 a4 c5 48 09 00 00  movq %r12, 0x948(%r13,%rax,8)  ; store address
13a69d9: 45 89 b4 c5 50 09 00 00  movl %r14d, 0x950(%r13,%rax,8) ; store size
13a69e1: 41 89 8c c5 54 09 00 00  movl %ecx, 0x954(%r13,%rax,8)  ; store stride
13a69e9: 74 0a                 je     0x13a69f5
13a69eb: 31 d2                 xorl   %edx, %edx
13a69ed: 44 89 f0              movl   %r14d, %eax
13a69f0: f7 f1                 divl   %ecx              ; vertex count = size / stride
13a69f2: 41 89 c6              movl   %eax, %r14d
13a69f5: 4b 8d 04 7f           leaq   (%r15,%r15,2), %rax
13a69f9: 45 89 b4 c5 58 09 00 00  movl %r14d, 0x958(%r13,%rax,8) ; store vertex count
13a6a01: 41 c6 85 d4 0a 00 00 01  movb $0x1, 0xad4(%r13)          ; dirty flag
```

`divl %ecx` divides the byte size by the stride to produce a vertex count. A texture-binding
command has no reason to compute a vertex count. This is stream-source state.

## Per-stream state layout

The context keeps a 24-byte record per stream at `0x948 + index * 24`:

| Offset | Field |
| --- | --- |
| `+0x00` | buffer address (base + offset) |
| `+0x08` | byte size |
| `+0x0C` | stride |
| `+0x10` | vertex count |

The stride itself comes from a separate per-stream table at `0xa8 + index * 2`, which is a
16-bit array. So the stride is registered elsewhere and this command only reads it.

A null buffer takes the unbind path at `0x13A699A`, which zeroes the address, size and stride
and stores the stride into the count slot.

## The debug branch

There is a conditional block at `0x13A6969` that reads two globals and calls
`scePthreadGetthreadid` (`0x33C6770`). It compares the current thread id against a stored value
and, on mismatch, takes a different `addq` (`0x40` instead of `0x20`) before the address
computation.

That is a debug or validation path, not part of the normal binding. It does not change the
identification, but it means the `-0x68` adjustment is not the only pointer arithmetic in this
function, which is worth noting for anyone instrumenting it.

## Confirmed versus inferred

Confirmed by bytes: the vtable, bounds, the three payload fields and their offsets, the
`-0x68` adjustment, the size field at `0x7c`, the stride division, the 24-byte per-stream
record, and the null-unbind path.

Inferred: the field names, from the public header. The `-0x68` offset is inferred to be the
`FVertexBuffer` to `FRHIResource` adjustment because the field it lands on behaves as a byte
size and the code divides by stride. The exact class layout above `FRHIResource` is not
recovered.

## Why this is not a texture-binding command

Three independent reasons:

1. The payload has no texture or sampler field. It has an index, a buffer, and an offset.
2. The code computes a vertex count by dividing size by stride.
3. The per-stream record holds address, size, stride and count, which is vertex-stream state.

A texture-binding command would carry a texture reference and a slot index, and would not
divide by a stride.

## Next targets

The stream-source command completes the vertex-input half of the chain. The remaining gap is
the shader pair:

1. `FRHICommandSetBoundShaderState`, which binds the vertex and pixel shader together.
2. `FRHICommandBeginRenderPass` / `FRHICommandEndRenderPass`, the actual pass boundary.

With stream source recovered, the chain is:

```
SetRenderTargets -> SetViewport -> SetBoundShaderState -> SetStreamSource -> DrawIndexedPrimitive
```

Four of the five are now identified. Only `SetBoundShaderState` is missing.

## Reproducible checks

```
python tools/a5/fde.py v100.elf 0x13A6930
llvm-objdump -d --start-address=0x13A6930 --stop-address=0x13A6A20 v100.elf
llvm-objdump -d --start-address=0x1410060 --stop-address=0x14100D0 v100.elf
```
