# A4 Phase 3: the shader/pipeline-state method at 0x0CB5130

## Result

The method at `0x0CB5130` is **not** a thin shader-setter wrapper. It is a large
command-recording function that writes through a separate command-buffer writer object, and
only falls back to direct GNM calls when the writer declines.

That distinction matters: the semantic contract is the writer interface, not the GNM setters.

## Membership in the draw vtable

The method is slot `+0x340` (`0x03EDFFE0`) of the same table that holds the draw method at
`+0x4F0`. So it is a member of the same interface, by table position.

But membership by table position is not the same as being on the draw path. The draw method is
reached by a direct call from a wrapper; this method has three direct callers
(`0x0CB438C`, `0x13D6B06`, `0x18ABB99`) plus its vtable slot. It is a general-purpose
recording entry point, not a draw-specific one.

## The writer-object pattern

The dominant structure in this function is an indirect call through `+0x18` of a pointer held
in a callee-saved register:

```
cb7018: 41 ff 55 18   callq  *0x18(%r13)
cb701c: 84 c0         testb  %al, %al
cb701e: 74 1c         je     0xcb703c
```

The return value is tested as a boolean. If the writer returns non-zero, the function skips the
direct GNM call and continues. If it returns zero, the function falls through to the GNM setter.

The same shape repeats throughout the function:

| Call form | Count |
| --- | --- |
| `callq *0x18(%r14)` | 26 |
| `callq *0x18(%r13)` | 23 |
| `callq *0x18(%r12)` | 22 |
| `callq *0x18(%rbx)` | 17 |
| `callq *0x18(%r15)` | 2 |
| `callq *0x18(%rax)` | 2 |

That is 92 indirect calls through offset `+0x18` of a writer object. The register varies because
the function reloads the writer pointer into different registers across basic blocks, but the
offset is constant.

## The fallback contract

The pixel-shader path shows the full sequence:

```
cb700c: 49 8b 55 20   movq   0x20(%r13), %rdx     ; argument 3: register array
cb7010: be 28 00 00 00 movl   $0x28, %esi          ; argument 2: size = 40
cb7015: 4c 89 ef      movq   %r13, %rdi           ; argument 1: writer object
cb7018: 41 ff 55 18   callq  *0x18(%r13)          ; writer->method(...)
cb701c: 84 c0         testb  %al, %al
cb701e: 74 1c         je     0xcb703c             ; writer declined -> fall through
cb7020: 49 8b 3c 24   movq   (%r12), %rdi         ; cmdbuf
cb7024: be 28 00 00 00 movl   $0x28, %esi          ; size = 40
cb7029: 31 d2         xorl   %edx, %edx           ; null register array
cb702b: e8 10 13 71 02 callq  0x33c8340            ; sceGnmSetPsShader350
cb7030: 85 c0         testl  %eax, %eax
cb7032: 75 08         jne    0xcb703c
```

The vertex-shader path is identical in shape, with size `0x1D` (29) and target `0x33C81B0`
(`sceGnmSetVsShader`):

```
cb704f: 49 8b 55 20   movq   0x20(%r13), %rdx
cb7053: be 1d 00 00 00 movl   $0x1d, %esi
cb7058: 4c 89 ef      movq   %r13, %rdi
cb705b: 41 ff 55 18   callq  *0x18(%r13)
cb705f: 84 c0         testb  %al, %al
cb7061: 74 20         je     0xcb7083
```

So the contract is:

- The writer object is the first argument (`%rdi`).
- The register-array size is the second argument (`%esi`): 40 bytes for PS, 29 for VS.
- The register array is the third argument (`%rdx`), loaded from `+0x20` of the writer.
- The writer returns a boolean in `%al`.
- A non-zero return means "handled"; the GNM fallback is skipped.

The GNM calls are the *fallback*, not the primary path. That inverts the natural assumption.

## What the GNM calls actually are

The function reaches these GNM import stubs:

| Stub | Calls |
| --- | --- |
| `0x33C8380` | 3 |
| `0x33C8340` (`sceGnmSetPsShader350`) | 3 |
| `0x33C81B0` (`sceGnmSetVsShader`) | 2 |
| `0x33C8270` | 3 |
| `0x33C82C0` | 3 |
| `0x33C81D0` | 1 |
| `0x33C88A0` | 1 |

Only two of these are the shader setters named in the A2 atlas. The rest are unnamed here
because the import-name resolution was not completed in this phase (see Uncertainties).

## The interception target

The concrete next target is the **writer object's `+0x18` method**, not the GNM setters and not
this function.

Reasons:

- It is the single choke point for 92 call sites in this function alone.
- It receives the semantic arguments: a size and a register array, which is the shader-state
  payload before PM4 encoding.
- It returns a boolean, so it is a decision point the caller already branches on.
- Intercepting it captures intent across many operations, not one setter.

The open question is what object type sits at `+0x18` and whether it is the same object across
all 92 sites. The register varies, which suggests the pointer is reloaded, but it may be the
same object.

## Uncertainties

- The writer object's type is not identified. Its vtable is not located.
- Whether all 92 `+0x18` calls target the same object is not proven.
- The import names for `0x33C8380`, `0x33C8270`, `0x33C82C0`, `0x33C81D0` and `0x33C88A0` were
  not resolved. The PLT relocation table in the unwrapped ELF uses SCE-relative offsets that
  the current script does not decode, so `tools/a4/imports.py` does not yet produce names.
- The function is large (`0x0CB5130` to `0x0CB8660`, about 13.6 KB) and only the shader paths
  were examined in detail.
- Membership in the draw vtable is established by slot position only. It does not prove this
  method runs on the draw path.

## Reproducible checks

```
llvm-objdump -d --start-address=0xCB5130 --stop-address=0xCB8660 v100.elf
llvm-objdump -d --start-address=0xCB7000 --stop-address=0xCB7090 v100.elf
python tools/a4/relocs.py v100.elf slot 0x3EDFFE0
```

The last command confirms slot `0x03EDFFE0` holds `0x0CB5130`.
