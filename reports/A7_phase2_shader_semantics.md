# A7 Phase 2: shader-stage semantics

## Result

Three corrections to the A6 shader interpretation, and one new finding.

The `shader_modifier` argument is not a flag. It is **OR'd into the RSRC1 register** of the
vertex shader, which means it changes the shader's resource configuration. That is a real
semantic consequence, not a hint.

## Correction 1: the null PS array is the null-shader branch

A6 said the pixel-shader setter passes a null array "because the registers were already written
by a preceding SetShReg sequence". That was wrong.

The target code has two branches:

```
ccd045: 48 85 c0              testq  %rax, %rax
ccd048: 74 42                 je     0xccd08c            ; null -> null-shader branch
ccd04a: 4c 8b a0 d8 01 00 00  movq   0x1d8(%rax), %r12   ; non-null -> program data
...
ccd07e: 49 83 c4 08           addq   $0x8, %r12          ; array = program_data + 8
ccd087: 4c 89 e2              movq   %r12, %rdx
ccd08a: eb 34                 jmp    0xccd0c0            ; call the setter with an array
...
ccd08c: (null path)
ccd0be: 31 d2                 xorl   %edx, %edx          ; array = null
ccd0c0: e8 7b b2 6f 02        callq  sceGnmSetPsShader350
```

So the null array is passed when the shader pointer itself is null. It is a **null-shader
binding**, not a redundant re-set.

## Correction 2: what the null path actually does

The shadPS4 implementation makes this concrete:

```cpp
if (!ps_regs) {
    cmdbuf = PM4CmdSetData::SetShReg(cmdbuf, 8u, 0u, 0u);   // SPI_SHADER_PGM_LO_PS/HI_PS = 0
    cmdbuf = PM4CmdData::SetContextReg(cmdbuf, 0x203u, 0u); // DB_SHADER_CONTROL = 0
    cmdbuf = PM4CmdData::SetContextReg(cmdbuf, 0x8fu, 0xfu);// CB_SHADER_MASK = 0xf
    WriteTrailingNop<0x1d>(cmdbuf);
}
```

So the null path **clears the PS program address** and sets the colour mask to all-ones. That is
a deliberate "no pixel shader" state, not a no-op.

The PM4 consequence is concrete: `SPI_SHADER_PGM_LO_PS` and `SPI_SHADER_PGM_HI_PS` become zero,
`DB_SHADER_CONTROL` becomes zero, and `CB_SHADER_MASK` becomes `0xF`.

That is the answer to the task's question 2 and 3: the semantics come from the PM4 state, and
the API name alone would have misled.

## Correction 3: the `shader_modifier` argument

The fourth argument to `sceGnmSetVsShader` is `shader_modifier`, and it is not a flag. The
implementation shows exactly what it does:

```cpp
if (shader_modifier & 0xfcfffc3f) {
    LOG_ERROR(Lib_GnmDriver, "Invalid modifier mask");
    return -1;
}
const u32 var = shader_modifier == 0
    ? vs_regs[2]
    : (vs_regs[2] & 0xfcfffc3f) | shader_modifier;
cmdbuf = PM4CmdSetData::SetShReg(cmdbuf, 0x4au, var, vs_regs[3]); // SPI_SHADER_PGM_RSRC1_VS
```

So the modifier is masked into `SPI_SHADER_PGM_RSRC1_VS`. The valid bits are the complement of
`0xfcfffc3f`, which is `0x030003c0`. Those are RSRC1 fields: the VGPRS/SGPRS counts and the
priority/float-mode bits.

**This means `shader-state + 0x1C0` changes the shader's resource configuration.** It is not
metadata. A different value produces a different `RSRC1`, which changes how the GPU allocates
registers for that shader.

That is the new architectural finding: the shader-state object carries a per-binding modifier
that rewrites the vertex shader's resource register.

## The selector and the array layout

A6 described `program-data + 0x3` as a "register-array selector" and `+0x40 + selector*4` as the
array. The task correctly warns against assuming these are complete register arrays. They are
not.

The evidence:

```
ccd103: 41 0f b6 5c 24 03     movzbl 0x3(%r12), %ebx       ; selector byte
ccd12c: 49 8d 54 9c 40        leaq   0x40(%r12,%rbx,4), %rdx  ; base + selector*4
ccd131: be 1d 00 00 00        movl   $0x1d, %esi          ; size = 29 dwords
```

The pointer is `program_data + 0x40 + selector*4`, and the size is a fixed 29 dwords. So the
array is **not** at a fixed offset per selector; the selector shifts the *start* by 4 bytes per
step, and the same 29-dword window is read from there.

That is a different structure from "N separate arrays". It is one contiguous register block, and
the selector picks a starting offset into it. A selector of 0 reads `+0x40`, a selector of 1
reads `+0x44`, and so on.

The per-index byte at `+0x5E + selector*4` is a separate flag table, read at `0x0CCD14C`:

```
ccd146: 41 0f b6 44 24 03     movzbl 0x3(%r12), %eax       ; selector
ccd14c: 41 8a 44 84 5e        movb   0x5e(%r12,%rax,4), %al ; flag byte
ccd151: a8 02                 testb   $0x2, %al
ccd153: 0f 85 be 00 00 00     jne     0xccd217             ; variant path
```

So `+0x5E` is a parallel table of flag bytes, one per selector, and bit 1 selects a variant
path. The two tables are adjacent and both indexed by the same selector.

## Confirmed versus inferred

Confirmed by bytes:

- The null-shader branch and its condition.
- The `program_data + 8` array for the non-null PS path.
- The `program_data + 0x40 + selector*4` pointer and the fixed 29-dword size.
- The `+0x5E + selector*4` flag table and the bit-1 test.
- The `shader_modifier` argument position.

Confirmed by the emulator implementation (not the target binary):

- That the null PS path clears the program address and sets the colour mask.
- That `shader_modifier` is OR'd into `RSRC1`.

Inferred:

- That the selector picks a starting offset into one contiguous block rather than selecting
  among separate arrays. The arithmetic supports it; the block's full extent was not measured.
- That the `+0x5E` flag's bit 1 means "variant". The branch is visible; its purpose is not.

## What this does not establish

- Where the shader-state object is populated. The task asked for this and it was not traced.
- Whether the selector ever exceeds 0 in practice. Only the arithmetic is established.
- The full extent of the register block at `+0x40`.

## Reproducible checks

```
llvm-objdump -d --start-address=0xCCD040 --stop-address=0xCCD0D0 v100.elf
llvm-objdump -d --start-address=0xCCD0F6 --stop-address=0xCCD160 v100.elf
```
