# A4 Phase 1: dispatch mechanism of the indexed-draw method

## Result

The call to the draw method at `0x0CBB9C0` is **direct**. It is not a virtual dispatch.

The vtable at `0x03EDFCA0` is real, populated, and contains the draw method at slot `+0x4F0`.
But the runtime caller does not use it. The caller is a small wrapper that calls the method
with `E8 rel32`.

So the original hypothesis was half right: the table exists and holds the method, but the
observed runtime path bypasses it. Both facts are reported below.

## Evidence: the call is direct

Disassembly of the caller in the 01.01 image:

```
13a68b0: 55                    pushq  %rbp
13a68b1: 48 89 e5              movq   %rsp, %rbp
13a68b4: 53                    pushq  %rbx
13a68b5: 50                    pushq  %rax
13a68b6: 48 8b 46 20           movq   0x20(%rsi), %rax
13a68ba: 48 8b 77 10           movq   0x10(%rdi), %rsi
13a68be: 8b 57 18              movl   0x18(%rdi), %edx
13a68c1: 8b 4f 1c              movl   0x1c(%rdi), %ecx
13a68c4: 44 8b 47 20           movl   0x20(%rdi), %r8d
13a68c8: 44 8b 4f 24           movl   0x24(%rdi), %r9d
13a68cc: 44 8b 57 28           movl   0x28(%rdi), %r10d
13a68d0: 44 8b 5f 2c           movl   0x2c(%rdi), %r11d
13a68d4: 8b 5f 30              movl   0x30(%rdi), %ebx
13a68d7: 48 83 ec 08           subq   $0x8, %rsp
13a68db: 48 89 c7              movq   %rax, %rdi
13a68de: 53                    pushq  %rbx
13a68df: 41 53                 pushq  %r11
13a68e1: 41 52                 pushq  %r10
13a68e3: e8 d8 50 91 ff        callq  0xcbb9c0
13a68e8: 48 83 c4 28           addq   $0x28, %rsp
13a68ec: 5b                    popq   %rbx
13a68ed: 5d                    popq   %rbp
13a68ee: c3                    retq
```

`0x13A68E3` is `E8` with rel32 `0xFF9150D8`, resolving to `0x0CBB9C0`. There is no register
indirection and no memory operand.

A full scan of both images finds exactly one reference to `0x0CBB9C0`:

| Image | Direct E8/E9 sites | 8-byte pointers |
| --- | --- | --- |
| 1.00 | `0x13A68E3` | none |
| 01.01 | `0x13A68E3` | none |

The pointer scan reports none because the vtable slot is populated by a relocation, not by a
literal. That is covered next.

## Evidence: the vtable is real

The table base `0x03EDFCA0` sits in the RELRO segment (`PT_SCE_RELRO`, file offset
`0x03E8C000`, vaddr `0x03E88000`), which is file-backed but not executable. That is exactly
where a relocated vtable belongs.

The vptr store site:

```
cace4b: 48 8d 05 4e 2e 23 03   leaq  0x3232e4e(%rip), %rax   # 0x3edfca0
cace52: 48 89 03               movq  %rax, (%rbx)
```

So `0x0CACE4B` loads the table base and `0x0CACE52` stores it as an object vptr. A2's
description of this site is correct.

The table is populated with plain 8-byte pointers. The first entries:

| Slot | Offset | Value |
| --- | --- | --- |
| `0x03EDFCA0` | `+0x000` | `0x0CACDA0` |
| `0x03EDFCA8` | `+0x008` | `0x0CACE70` |
| `0x03EDFCB0` | `+0x010` | `0x0` |
| `0x03EDFCB8` | `+0x018` | `0x0` |
| `0x03EDFCC0` | `+0x020` | `0x0CACF80` |
| `0x03EDFCC8` | `+0x028` | `0x0CAD1E0` |

The pattern is pairs of function pointers separated by two zero slots. That is consistent with
a vtable carrying two entries per interface method, most likely a thunk plus an adjustor, or a
method plus a scalar-deleting variant.

The claimed slots all resolve as A2 reported:

| Slot | Offset | Value | Matches |
| --- | --- | --- | --- |
| `0x03EE0190` | `+0x4F0` | `0x0CBB9C0` | draw method |
| `0x03EDFDB8` | `+0x118` | `0x0CADC00` | compute dispatch |
| `0x03EDFFE0` | `+0x340` | `0x0CB5130` | shader/pipeline |

The arithmetic `0x03EDFCA0 + 0x4F0 = 0x03EE0190` is confirmed, and the value at that slot is
exactly `0x0CBB9C0`.

The table extends to at least `+0xC78` (slot `0x03EE0918`), so it is a large interface.

## The two-table structure

There are two distinct tables, and the distinction matters.

**Table A — the context vtable, `0x03EDFCA0`.** Holds the draw, compute and shader methods.
Reached by the vptr store at `0x0CACE52`. Referenced by RIP-relative `lea` at `0x0CACE4B` and
`0x197588A`.

**Table B — the wrapper table, `0x03F26A88`.** Holds the wrapper `0x13A68B0` at that exact
slot. Referenced by 28 RIP-relative `lea` sites. One of them, `0x157F2A7`, stores it as a vptr:

```
157f298: 48 8d 15 e9 77 9a 02   leaq  0x29a77e9(%rip), %rdx   # 0x3f26a88
157f2a7: 48 89 10               movq  %rdx, (%rax)
157f2aa: 4c 89 78 10            movq  %r15, 0x10(%rax)
157f2ae: 89 58 18               movl  %ebx, 0x18(%rax)
157f2b1: 48 c7 40 1c 00 00 00 00 movq  $0x0, 0x1c(%rax)
157f2b9: 44 89 68 24            movl  %r13d, 0x24(%rax)
157f2bd: 44 89 40 28            movl  %r8d, 0x28(%rax)
157f2c1: 89 70 2c               movl  %esi, 0x2c(%rax)
157f2c4: 44 89 60 30            movl  %r12d, 0x30(%rax)
```

Those field offsets `0x10`, `0x18`, `0x1c`, `0x24`, `0x28`, `0x2c`, `0x30` are exactly the
fields the wrapper reads at `0x13A68BA` through `0x13A68D4`. The wrapper is a virtual method of
the object whose vptr is Table B.

Neither the wrapper nor the compute method has any direct caller. Both are reached only through
their tables.

## Runtime corroboration

The B3 trace already contains the confirmation, and it was not recognised as such at the time.

Every captured event carries `stack_args[4] = 0x803F26A88`. With `module_base = 0x800000000`,
that is `module_base + 0x3F26A88` — the address of the slot in Table B that holds the wrapper.

So the runtime stack frame at the draw method contains a pointer to the wrapper's own vtable
slot. That is consistent with the wrapper being invoked virtually through Table B, and it
independently confirms Table B is live at runtime.

The trace also confirms the caller: `caller=0x8013A68E8`, which is `module_base + 0x13A68E8`,
the instruction after the direct call.

## Classification

| Question | Answer | Basis |
| --- | --- | --- |
| Is the draw call direct? | Yes | `E8 rel32` at `0x13A68E3` |
| Is there a vtable holding the draw method? | Yes | slot `0x03EE0190` = `0x0CBB9C0` |
| Does the runtime caller use that vtable? | No | the caller is a direct wrapper |
| Is the wrapper itself virtual? | Yes, through Table B | vptr store at `0x157F2A7`, runtime stack arg |
| Is Table A live at runtime? | Not demonstrated | no runtime evidence yet |

The honest summary: the interface exists and is populated, but the observed draw path reaches
the method through a direct call inside a virtual wrapper. The wrapper is the virtual boundary,
not the draw method.

## What this means for interception

The practical target is the wrapper at `0x13A68B0`, not the draw method.

- The wrapper is a virtual method, so its slot in Table B is a stable interception point.
- It is the boundary that receives the semantic arguments: it reads the object fields and
  forwards them to the draw method.
- It has no direct callers, so a detour at its entry cannot be bypassed by a direct call.

The draw method at `0x0CBB9C0` remains a valid secondary observation point, and the existing
A3 probe already covers it.

## Uncertainties

- The object type behind Table B is not identified. RTTI and type names are not recovered.
- The two-entries-per-method pattern in Table A is inferred from spacing, not proven.
- Table A's liveness at runtime is not demonstrated. Only Table B is corroborated.
- The wrapper's own slot index in Table B is `0x3F26A88 - base`, but the Table B base is not
  established; only that one slot address is known.
- The 1.00 and 01.01 images agree on every address checked here, so no version divergence was
  found in these regions.

## Reproducible checks

Scripts are in `_build/a4/` (not committed; they are analysis scratch):

- `refs.py <elf> <va>` — direct E8/E9 call sites and 8-byte pointers to a VA.
- `relocs.py <elf> target <va...>` — RELATIVE relocations whose addend is a VA.
- `relocs.py <elf> slot <va...>` — relocation and raw bytes at a slot.
- `rip.py <elf> <va...>` — RIP-relative `lea` references to a VA.

Images: `v100.elf` and `v101.elf`, unwrapped from the pinned `eboot.bin` files with
`tools/ps4elf.mjs unwrap`.

Key commands:

```
python refs.py v101.elf 0xCBB9C0
python relocs.py v100.elf slot 0x3EE0190 0x3EDFCA0
python rip.py v100.elf 0x3EDFCA0 0x3F26A88
```
