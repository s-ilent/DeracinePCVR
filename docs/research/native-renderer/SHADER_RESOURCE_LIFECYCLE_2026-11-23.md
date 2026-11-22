# Native PCVR Renderer — UE4 Shader-Resource Lifecycle and Interception Boundary

**Date:** 2026-11-23
**Branch:** `research/native-shader-resource-lifecycle-20261123`
**Base:** `origin/research/native-rhi-foundation-20261123` @ `6b359f52`
**Status:** Static analysis only. No executable hook, no runtime capture, no native GPU prototype.
**Target:** Déraciné 1.00 SELF, inner ELF `v100.elf`
**ELF SHA-256:** `3F70499588A9A51781DE87E2999433B85A6D23A4307DC44827134C8B71A938BF`
**SELF SHA-256 (pinned):** `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`

This report is independent of the shadPS4 renderer. shadPS4 is used only as a read-only
execution oracle for cross-checking addresses already published in SC-09 and A-028R.

---

## 1. Executive findings

### 1.1 The headline result

**The UE4 shader-resource object is real, refcounted, and reachable above the GNM layer.**
The chain published in A6 is confirmed byte-for-byte, and this pass extends it in both
directions: down to the object's constructor and refcount template, and up to the
context method that binds it.

The single most important new finding is that **the shader-state binding is a direct
context method, not a deferred command**, and it is the first point in the recovered
pipeline where a complete, platform-independent shader identity is available together
with its per-draw parameter state.

### 1.2 Confidence classification

| # | Finding | Confidence | Basis |
|---|---|---|---|
| F1 | A6 field chain `context+0x468 → +0x200 → +0x1D8 → +0x40+idx*4` | **Verified** | Byte-exact disassembly at `0x0CCD0D0`–`0x0CCD139` |
| F2 | `0x0CBE210` is the shader-state binder; it refcount-assigns `context+0x468` | **Verified** | `leaq 0x468(%r14)` + call to refcount-assign template |
| F3 | `0x0CBF920`/`0x0CBF A40`/`0x0CC7500`/`0x0CBF700` are one refcount-assign template | **Verified** | Mnemonic-identical bodies, 4 instantiations |
| F4 | Refcount layout: `+0x08` count, `+0x0C` delete-pending, `+0x10` deferred-delete flag, `vtable[1]` destructor | **Verified** | `0x0CBF949`–`0x0CBF968`, `0x0CBF970`–`0x0CBF97C` |
| F5 | Shader-resource object is 472 bytes (`0x1D8`), allocated by one shared allocator | **Verified** | 5,031 allocation sites with `movl $0x1d8,%edi` |
| F6 | Shader-resource vtable `0x3F20180`, secondary base vtable `0x3F203D8` at `+0x28` | **Verified** | Constructor `0x135B9B0`, destructor `0x135BBC0` |
| F7 | Program data at `shader_resource+0x1D8` is a **separate refcounted object**, not inline | **Verified** | `lock incl 0x8(%rax)` after the store at `0x1209D63`, `0x12244F4` |
| F8 | Shader-map slot indirection: `u16` array, `0xFFFF` unmapped, bit 15 = direct PM4 | **Verified** | `0x0CAB43F`–`0x0CAB464` |
| F9 | `writer+0xC468` holds `shader_resource+0xB0` (the map base) | **Verified** | `0x0CB12FE`–`0x0CB132A` |
| F10 | `0xB0 + 0xC2 = 0x172`, reconciling A-023's `shader_resource+0x172` | **Verified** | Arithmetic + both read sites |
| F11 | Shader-state object is a **pointer** at `context+0x468`, not an embedded member | **Verified** | Refcount-assign at `0x0CBEE2D`, `0x0CD9D67` |
| F12 | `0x0CB5130` is `primary_vtable+0x340` **and** `secondary_vtable+0xC8` | **Verified** | `0x3EDFCA0+0x340 = 0x3EDFFE0 = 0x3EDFF18+0xC8` |
| F13 | The two "shader" setters at `0x2F61D50`/`0x2F61FE0` are **not** GNM import stubs | **Verified** | No `callq 0x33C8xxx` in either body |
| F14 | `writer+0xC490` is a second map pointer, distinct from `+0xC468` | **Verified** | Both read, only `+0xC468` has a located writer |
| F15 | The shader-state binder runs on the RHI thread, not the render thread | **Inferred** | A9's two-thread gate; not observed for this function |
| F16 | `+0x1E0` and `+0x1E8` on the shader-state are the PS and VS resources | **Inferred** | Symmetric use in `0x0CBE3AD`/`0x0CBE55D`; roles not proven |
| F17 | The 472-byte object is `FRHIShader`-equivalent | **Inferred** | Size, vtable shape, refcount, and program-data ownership |

### 1.3 What this changes about the plan

The FOUNDATION report proposed recovering "resource creation and lifetime, shader/descriptor
binding" as open work. This pass closes the shader half of that: creation, ownership,
refcounting, destruction, map indirection and binding are all located and byte-verified.

It also **corrects one inherited assumption**: the shader binding is not a deferred command
object. A6 already said this, and this pass confirms it structurally — `0x0CBE210` is a
direct method on the context, reached through a secondary vtable, and it mutates the context
in place. There is no command payload to intercept.

---

## 2. Verified address map

All addresses are inner-ELF virtual addresses. Function bounds are from `.eh_frame_hdr`
(180,806 entries), not from heuristics.

### 2.1 Functions

| RVA | Extent | Role | Confidence |
|---|---|---|---|
| `0x0CBE210` | `0x0CBE210`–`0x0CBEE90` | **Shader-state binder.** Refcount-assigns `context+0x468`, then emits VS/PS state | Verified |
| `0x0CBEE90` | `0x0CBEE90`–`0x0CBF030` | Adjacent state emitter (same vtable family) | Verified bounds, role inferred |
| `0x0CBF540` | 8 bytes | `this-8` thunk → `0x0CBE210` | Verified |
| `0x0CBF920` | `0x0CBF920`–`0x0CBFA40` | Refcount-assign template, instantiation A | Verified |
| `0x0CBFA40` | `0x0CBFA40`–`0x0CBFB60` | Refcount-assign template, instantiation B | Verified |
| `0x0CBF700` | `0x0CBF700`–`0x0CBF820` | Refcount-assign template, instantiation C | Verified |
| `0x0CC7500` | `0x0CC7500`–`0x0CC7620` | Refcount-assign template, instantiation D | Verified |
| `0x0CBFB80` | `0x0CBFB80`–`0x0CBFC80` | Refcount-release (decrement-only) | Verified |
| `0x0CB5130` | `0x0CB5130`–`0x0CB8660` | Shader/pipeline-state recorder (A4/A6) | Verified |
| `0x0CCC2F0` | `0x0CCC2F0`–`0x0CCD480` | GNM shader receiver (A6) | Verified |
| `0x135B9B0` | `0x135B9B0`–`0x135BBB0` | **Shader-resource constructor** | Verified |
| `0x135BBC0` | `0x135BBC0`–`0x135BC40` | Shader-resource destructor | Verified |
| `0x135B4F0` | `0x135B4F0`–`0x135B740` | Shader-resource singleton factory | Verified |
| `0x103C0C0` | `0x103C0C0`–`0x103C210` | Shared allocator (`size, flag`) | Verified |
| `0x0CBB9C0` | `0x0CBB9C0`–`0x0CBBEF0` | Indexed draw implementation | Verified |
| `0x13A68B0` | `0x13A68B0`–`0x13A68F0` | Draw command wrapper (A8) | Verified |
| `0x13764D0` | — | Command executor (A8/A9) | Verified (A9) |

### 2.2 Data

| RVA | Contents | Confidence |
|---|---|---|
| `0x3F20180` | Shader-resource primary vtable | Verified |
| `0x3F203D8` | Shader-resource secondary base vtable (at `+0x28`) | Verified |
| `0x3EF8CE0` | Shader-resource base-class vtable (installed by ctor at `+0x00`) | Verified |
| `0x3EDFCA0` | Context primary vtable | Verified |
| `0x3EDFF18` | Context secondary vtable (at `+0x08`) | Verified |
| `0x3EDFFE0` | `0x0CB5130` — shader recorder slot | Verified |
| `0x3EE01C0` | Shader-state primary vtable | Verified |
| `0x3EE0238` | Shader-state secondary vtable (at `+0x08`) | Verified |
| `0x3EE0210` | `0x0CBE210` — binder slot (`0x3EE01C0+0x50`) | Verified |
| `0x47B2968` | Shader-resource singleton pointer | Verified |
| `0x44A0F70` | Deferred-delete queue base | Verified |
| `0x47B4220` | Deferred-delete queue head (lock-free) | Verified |

### 2.3 Object layouts

**Shader resource** (472 bytes, `0x1D8`):

| Offset | Field | Confidence |
|---|---|---|
| `+0x00` | vtable `0x3F20180` | Verified |
| `+0x08` | refcount (atomic) | Verified |
| `+0x0C` | delete-pending flag | Verified |
| `+0x10` | deferred-delete flag | Verified |
| `+0x28` | secondary vtable `0x3F203D8` | Verified |
| `+0xB0` | shader map base (slot array at `+0xC2`) | Verified |
| `+0x1D8` | **program data pointer** (separate refcounted object) | Verified |

**Program data** (separate object, refcounted):

| Offset | Field | Confidence |
|---|---|---|
| `+0x03` | register-array selector byte | Verified |
| `+0x06` | `u16`, emitted as PM4 at `0x0CC374B` | Verified |
| `+0x24` | `u8`, low 2 bits → `writer+0xC9C1` | Verified |
| `+0x40 + idx*4` | GCN register array (29 DWORDs for VS, 40 for PS) | Verified |
| `+0x5E + idx*4` | per-array flag byte; bit 1 selects a variant path | Verified |

**Shader state** (pointer at `context+0x468`):

| Offset | Field | Confidence |
|---|---|---|
| `+0x00` | vtable `0x3EE01C0` | Verified |
| `+0x08` | secondary vtable `0x3EE0238` | Verified |
| `+0x1C0` | `u32` GNM flags argument | Verified |
| `+0x1E0` | resource A (has `+0x260` sub-object) | Verified |
| `+0x1E8` | resource B | Verified |
| `+0x200` | resource C (the A6 chain entry) | Verified |

---

## 3. Shader-resource lifecycle

```
                        ┌─────────────────────────────────────────┐
                        │ 0x135B4F0  singleton factory            │
                        │  if (0x47B2968) return it;              │
                        │  p = alloc(0x1D8, 1)                    │
                        │  p->vptr = 0x3EF8CE0                    │
                        │  p->+0x98 = 0x135B9B0  (ctor)           │
                        │  p->+0xA0 = 0x135BBB0  (no-op)          │
                        │  p->+0xA8 = 0xF65930                    │
                        │  0x47B2968 = p                          │
                        └───────────────────┬─────────────────────┘
                                            │
                    ┌───────────────────────▼───────────────────────┐
                    │ 0x135B9B0  constructor                        │
                    │  vptr  = 0x3F20180                            │
                    │  +0x28 = 0x3F203D8                            │
                    │  +0x1D8 = 0  (program data, set later)        │
                    │  +0x1E0 = 0                                   │
                    └───────────────────────┬───────────────────────┘
                                            │
        ┌───────────────────────────────────▼───────────────────────────────────┐
        │ PROGRAM DATA ATTACH                                                   │
        │  0x1209D63 / 0x12244F4:                                               │
        │     res->+0x1D8 = pd;  lock incl pd->+0x08   ← separate refcounted obj │
        │  0x1092DD9 (loader):                                                  │
        │     pd->+0x04/+0x06/+0x08/+0x0A = u16 fields from the cooked record    │
        └───────────────────────────────────┬───────────────────────────────────┘
                                            │
        ┌───────────────────────────────────▼───────────────────────────────────┐
        │ MAP BINDING                                                           │
        │  0x0CB12FE / 0x0CC3706:                                               │
        │     writer->+0xC468 = res->+0xB0        ← slot array at res->+0x172    │
        │     writer->+0xC388 = res->+0x1D8                                     │
        │     writer->+0xC9C1 = pd->+0x24 & 3                                   │
        └───────────────────────────────────┬───────────────────────────────────┘
                                            │
        ┌───────────────────────────────────▼───────────────────────────────────┐
        │ STATE BINDING  (0x0CBE210, direct context method)                     │
        │  refcount_assign(&context->+0x468, new_state)   ← 0x0CBFA40           │
        │  then, per stage:                                                     │
        │    st = context->+0x468                                               │
        │    res = st->+0x200                                                   │
        │    pd  = res->+0x1D8                                                  │
        │    idx = pd->+0x03                                                    │
        │    regs = pd->+0x40 + idx*4                                           │
        │    flags = st->+0x1C0                                                 │
        └───────────────────────────────────┬───────────────────────────────────┘
                                            │
        ┌───────────────────────────────────▼───────────────────────────────────┐
        │ GNM EMISSION  (0x0CCC2F0)                                             │
        │  sceGnmSetVsShader(cmd, 0x1D, regs, flags)                            │
        │  sceGnmSetPsShader350(cmd, 0x28, NULL)                                │
        └───────────────────────────────────┬───────────────────────────────────┘
                                            │
        ┌───────────────────────────────────▼───────────────────────────────────┐
        │ DESTRUCTION                                                           │
        │  refcount -> 0:                                                       │
        │    if (+0x10) vtable[1](this)          ← immediate                    │
        │    else if CAS(+0x0C, 0->1):                                          │
        │      queue into 0x44A0F70, push 0x47B4220  ← deferred                 │
        │  0x135BBC0: vptr = 0x3EF8CB0, free                                    │
        └───────────────────────────────────────────────────────────────────────┘
```

### 3.1 The refcount template, verified

Four instantiations share one body. Normalising the `E8`/`E9` rel32 operands makes them
mnemonic-identical, which is what a template instantiation looks like:

```
movq (%r14), %rbx          ; old = *slot
movq %rsi, (%r14)          ; *slot = new
testq %rsi, %rsi
je   1f
lock incl 0x8(%rsi)        ; new->refcount++
1:
testq %rbx, %rbx
je   done
lock xaddl %eax, 0x8(%rbx) ; old->refcount--
cmpl $1, %eax
jne  done
cmpb $0, 0x10(%rbx)        ; deferred-delete flag
je   2f
movq (%rbx), %rax
callq *0x8(%rax)           ; vtable[1] = destructor
jmp  done
2:
movl $1, %ecx
lock cmpxchgl %ecx, 0xc(%rbx)   ; claim delete-pending
jne  done
... queue into 0x44A0F70 ...
```

This is UE4's `FRHIResource` reference-counting contract: an intrusive count at `+0x08`,
a deferred-delete flag at `+0x10`, and a delete-pending CAS at `+0x0C`. The presence of
the deferred-delete queue is the strongest single piece of evidence that this is the
original UE4 abstraction and not a FromSoftware rewrite.

### 3.2 What is original UE4, what is FromSoftware, what is PS4 RHI

| Layer | Evidence | Attribution |
|---|---|---|
| Intrusive refcount + deferred delete | Template shape, `+0x08`/`+0x0C`/`+0x10` layout | **Original UE4** `FRHIResource` |
| Shader resource owning program data | `+0x1D8` pointer, separate refcount | **Original UE4** shape, PS4 payload |
| Shader map slot indirection | `u16` array, `0xFFFF`, bit 15 | **PS4 RHI** (`FGnmShaderMap`-equivalent) |
| GCN register arrays in program data | `+0x40+idx*4`, 29/40 DWORDs | **PS4 RHI** |
| `sceGnmSetVsShader` / `SetPsShader350` | Import calls | **PS4 RHI** |
| Direct context method for state binding | No command object | **FromSoftware/PS4 RHI deviation** from the public 4.18 header |
| `0x0CB5130` writer-object fallback pattern | 92 `callq *0x18` sites | **PS4 RHI** |

The deviation that matters: public UE4.18.3 declares `FRHICommandSetBoundShaderState` as a
deferred command. This build does not use it on the observed path. A6 found this; this pass
confirms it structurally — the binder is a vtable method on the context, and it mutates the
context's `+0x468` slot in place.

---

## 4. Candidate interception points, ranked

Ranking criteria: (a) is a complete shader description available, (b) is it above the PS4
layer, (c) is the call site stable and singular, (d) does it survive to a native backend.

### Rank 1 — `0x0CBE210` shader-state binder (RECOMMENDED)

| Property | Value |
|---|---|
| RVA | `0x0CBE210`–`0x0CBEE90` |
| Reached by | Direct call from `0x0CBF549` (thunk) and vtable slot `0x3EE0210` |
| Calling convention | SysV x86-64: `rdi` = context, `rsi` = new shader state |
| Callers | 1 direct (`0x0CBF549`), plus the vtable slot |
| Direct/virtual/deferred | **Direct method**, not deferred |
| Info available | Full shader-state pointer; `+0x200`/`+0x1E8`/`+0x1E0` resources; `+0x1C0` flags; per-stage program data |
| Sufficient for native? | **Yes for shader identity and stage binding.** No for parameter values (those arrive separately) |

Why it wins: it is the single point where the engine commits a shader state to the context.
Everything downstream is PS4 encoding. Everything upstream is a lookup. It is one function,
one call site, and it already receives the semantic object.

### Rank 2 — `0x0CB5130` shader/pipeline-state recorder

| Property | Value |
|---|---|
| RVA | `0x0CB5130`–`0x0CB8660` |
| Reached by | `primary_vtable+0x340` = `secondary_vtable+0xC8` = `0x3EDFFE0` |
| Callers | 3 direct (`0x0CB438C`, `0x13D6B06`, `0x18ABB99`) + vtable |
| Info available | Everything Rank 1 has, plus the writer-object decision point |
| Sufficient for native? | Yes, but broader and noisier |

Why second: it is a general-purpose recording entry point, not draw-specific (A4 established
this). Intercepting it captures more than shader binding and requires filtering.

### Rank 3 — `0x0CCC2F0` GNM shader receiver

| Property | Value |
|---|---|
| RVA | `0x0CCC2F0`–`0x0CCD480` |
| Callers | 3, all inside `0x0CB5130` |
| Info available | Register arrays and flags, already PS4-shaped |
| Sufficient for native? | **No** — this is the console GPU layer the task excludes |

Why third: it is the boundary the task explicitly says to stay above. It is useful as a
*verification oracle* for a Rank 1 capture, not as an implementation point.

### Rank 4 — `0x135B9B0` shader-resource constructor

| Property | Value |
|---|---|
| RVA | `0x135B9B0`–`0x135BBB0` |
| Reached by | Indirect, through factory `+0x98` |
| Info available | Object identity at creation; no program data yet |
| Sufficient for native? | No — program data is attached later |

Why fourth: it gives resource identity and lifetime, which a native backend needs for
caching, but not the shader description. Pair it with Rank 1.

### Rank 5 — `0x13764D0` command executor

Not recommended. A9 established that it runs no destructors and resets the allocator, so
payloads are valid only during the walk. It also sees no shader binding at all, because
binding is not a command.

### 4.1 The ranking in one line

**Rank 1 is the answer to the task's success criterion**: it is a genuine UE4 RHI resource
interface, above the PS4 layer, receiving a complete shader state.

---

## 5. UE4 semantic information available at each point

| Information | Rank 1 (`0x0CBE210`) | Rank 2 (`0x0CB5130`) | Rank 3 (`0x0CCC2F0`) | Rank 4 (ctor) |
|---|---|---|---|---|
| Shader resource identity | Yes (`+0x200`) | Yes | No (already lowered) | Yes |
| Stage identification (VS/PS) | Yes, by slot | Yes | Yes, by setter | No |
| Program data pointer | Yes (`+0x1D8`) | Yes | No | No (null at ctor) |
| Shader map base | Yes (`+0xB0`) | Yes | No | No |
| Slot → register indirection | Yes (`+0xC2`) | Yes | Resolved already | No |
| GCN register array | Yes (`+0x40+idx*4`) | Yes | Yes | No |
| Register-array selector | Yes (`+0x03`) | Yes | Yes | No |
| Per-array flag byte | Yes (`+0x5E+idx*4`) | Yes | Yes | No |
| GNM flags argument | Yes (`+0x1C0`) | Yes | Yes | No |
| Program-data `u16` fields | Yes (`+0x04`..`+0x0A`) | Yes | No | No |
| Permutation key | **No** | **No** | **No** | **No** |
| Vertex declaration / input layout | **No** | **No** | **No** | **No** |
| Parameter values (uniforms) | **No** — separate path | **No** | **No** | **No** |
| Cooked material metadata | **No** | **No** | **No** | **No** |

### 5.1 What is genuinely platform-independent

Only three things at Rank 1 are platform-independent in the sense a PC backend can consume
directly:

1. **Resource identity and lifetime.** The refcount contract is engine-level, not PS4-level.
2. **Stage association.** Which resource is bound as VS versus PS is engine semantics.
3. **The slot indirection.** `api_slot → register` is a mapping the engine owns, and A-028R
   proved it is a function (248/248 shaders, zero violations) that is neither identity nor
   monotonic.

Everything else at Rank 1 is PS4-shaped: the register arrays are GCN, the flags are GNM, and
the program data is a PS4 program block.

### 5.2 What requires translation or reconstruction

| Requirement | Status |
|---|---|
| Shader bytecode for PC | **Requires translation or reconstruction.** The program data holds GCN register arrays and a PS4 program block. It does **not** contain HLSL, DXIL or SPIR-V. |
| Parameter map | **Requires reconstruction.** The slot indirection gives register placement, not a typed parameter map. |
| Input layout | **Requires separate recovery.** Not present at any ranked point. |
| Permutation key | **Requires separate recovery.** Not present at any ranked point. |

**Explicit non-assumption:** the PS4 shader binaries do not contain HLSL, DXIL or SPIR-V.
A-028R's validated GNM metadata describes *declarations* (usage type, api_slot,
start_register), not source or portable IR. Nothing in this pass found portable shader IR.

---

## 6. Missing information for native rendering

Ordered by how directly each blocks the first real draw.

| # | Missing | Why it blocks | Where it might be |
|---|---|---|---|
| M1 | **Portable shader program** | No PC-executable code exists for any shader | Requires translation from GCN, or reconstruction from cooked metadata |
| M2 | **Parameter values per draw** | Rank 1 gives binding, not values | SC-09 captured effective values at draw time; the UE4 producer is still open |
| M3 | **Vertex declaration / input layout** | Cannot build a Vulkan pipeline | SC-08 captured vertex inputs; the declaration object is not located |
| M4 | **Permutation key** | Cannot select or cache a translated program | Not found at any ranked point |
| M5 | **Render-target and viewport state at draw** | Needed for a complete pipeline | A5 recovered the commands; not joined to Rank 1 |
| M6 | **`writer+0xC490` writer** | A second map pointer with no located writer | Unresolved; see §7.2 |
| M7 | **Stage roles of `+0x1E0`/`+0x1E8`** | Needed to name VS vs PS confidently | Inferred only |

### 6.1 Honest statement of the gap

The success criterion asks for "an evidence-backed path from an original UE4 shader resource
to the information a native PC RHI implementation would need to bind and execute it."

**Binding: established.** Rank 1 gives resource identity, stage, and lifetime.

**Executing: not established.** M1 is the hard blocker. No portable shader program exists
anywhere in the recovered chain, and this pass found no evidence that one is recoverable
without translation or reconstruction.

---

## 7. Blockers and unresolved items

### 7.1 No runtime verification

Every claim here is static. No hook was installed, no capture was taken. The task permitted
a standalone test if safe; none was attempted, because the only meaningful test would be a
live hook into the game process, which the scope forbids without a separate authorization.

### 7.2 `writer+0xC490` has no located writer

`0x0CAB438` reads `writer+0xC490` as a map base and indexes it at `+0xC2`. A full
`movq`-store scan across all readable segments found **zero** writers to `+0xC490`,
`+0xC498` or `+0xC4A0`. Either the field is written by an instruction form the scan does
not cover (a SIB-addressed store, or a wide struct copy), or it aliases a field written
through a different base register.

This is recorded as an open item, not resolved by assumption. `+0xC468` **does** have a
located writer (`0x0CB132A`, `0x0CC372B`) and holds `shader_resource+0xB0`.

### 7.3 The two "shader setters" are not GNM stubs

F13: `0x2F61D50` and `0x2F61FE0` contain no `callq 0x33C8xxx`. They are state-comparison
and caching wrappers that call through `*0x18(%rXX)` — the same writer-object pattern A4
found in `0x0CB5130`. Their GNM emission happens elsewhere. This corrects a natural
assumption that a function taking a program-data pointer must be the GNM setter.

### 7.4 Stage roles are inferred

`+0x1E0`, `+0x1E8` and `+0x200` are three resource slots used symmetrically in
`0x0CBE3AD` and `0x0CBE55D`. The code paths differ (one uses `+0x260` of the `+0x1E0`
object, the other does not), which is consistent with PS/VS but does not prove it.

### 7.5 Thread attribution is inherited, not observed

A9 established two recorded thread identities and a three-way branch. This pass did not
observe which thread runs `0x0CBE210`. F15 is inferred from A9, not measured.

---

## 8. Next experiment — one concrete, falsifiable test

### Hypothesis

**H1:** At `0x0CBE210` entry, `rsi` points to a shader-state object whose `+0x200`
resource has a non-null `+0x1D8` program data, and the `u16` at `program_data+0x06`
equals the value the GNM path later emits as a PM4 packet at `0x0CC374B`.

If H1 holds, Rank 1 is a valid interception point and the program-data `u16` is a
recoverable shader identity field. If it fails, Rank 1 is not the boundary and the search
must move to the map-binding path.

### Method

A passive, default-off observer at `0x0CBE210` entry, in the style of the existing A3 probe:

1. Read `rsi` (the shader state). If null, record and return.
2. Read `state+0x200`. If null, record and return.
3. Read `resource+0x1D8`. If null, record and return.
4. Copy **scalars only**: `program_data+0x03` (u8), `+0x06` (u16), `+0x24` (u8),
   `state+0x1C0` (u32), and the resource pointer as an opaque value.
5. Do **not** dereference the register array. Do **not** retain any pointer past the call.
6. Emit one bounded record per call into a fixed ring buffer, exported on clean shutdown.

### Falsification criteria

| Outcome | Interpretation |
|---|---|
| `program_data+0x06` matches the PM4 value at `0x0CC374B` for the same draw | **H1 confirmed.** Rank 1 is the boundary. |
| `+0x1D8` is null on most calls | Rank 1 fires before program data is attached; move to Rank 2. |
| `+0x06` never matches the PM4 value | The `u16` is not the identity field; the map indirection is the identity. |
| `rsi` is not a shader state | The calling convention assumption is wrong; re-derive from `0x0CBF549`. |

### Why this is the right next step

It is bounded (one function, scalars only), falsifiable (a specific equality), and it
directly tests the one claim the whole ranking rests on. It also produces the first
runtime evidence in this track, which every prior report has lacked.

### Preconditions

- A separate authorization for a live hook. The current scope forbids modifying the
  executable and does not authorize runtime instrumentation.
- The existing A3 probe infrastructure, reused rather than rebuilt.

---

## 9. Reproducible analysis

All commands run from a directory containing `v100.elf` and the helper scripts below.
Toolchain: `llvm-objdump` 23.1.5, Python 3.15.

### 9.1 Verify the A6 chain

```
llvm-objdump -d --start-address=0x0CCD0D0 --stop-address=0x0CCD240 --no-show-raw-insn v100.elf
```

Expect `movq 0x468(%rax),%rax` → `movq 0x200(%rax),%r13` → `movl 0x1c0(%rax),%r15d`
→ `movq 0x1d8(%r13),%r12` → `movzbl 0x3(%r12),%ebx` → `leaq 0x40(%r12,%rbx,4),%rdx`.

### 9.2 Verify the binder and its refcount assign

```
llvm-objdump -d --start-address=0x0CBE210 --stop-address=0x0CBE3C0 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x0CBEE00 --stop-address=0x0CBEE90 --no-show-raw-insn v100.elf
```

Expect `leaq 0x468(%r14),%rdi` at `0x0CBEE2D` followed by `callq 0x0CBFA40`.

### 9.3 Verify the refcount template identity

```
python norm.py
```

Expect all four instantiations mnemonic-identical after normalising rel32 operands.

### 9.4 Verify the constructor and vtables

```
llvm-objdump -d --start-address=0x135B9B0 --stop-address=0x135BBB0 --no-show-raw-insn v100.elf
python elf.py vt 0x3F20180 12
python elf.py vt 0x3F203D8 8
```

### 9.5 Verify the allocation size

```
python alloc3.py
```

Expect `0x1d8 (472) x5031` as the dominant size.

### 9.6 Verify the map indirection

```
llvm-objdump -d --start-address=0x0CAB400 --stop-address=0x0CAB4E0 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x0CB12B0 --stop-address=0x0CB13C0 --no-show-raw-insn v100.elf
```

Expect `movzwl 0xc2(%rax,%r14,2),%eax` and `cmpl $0xffff,%eax`, and
`leaq 0xb0(%r14),%rcx` → `movq %rcx,0xc468(%rbx)`.

### 9.7 Helper scripts

Written for this pass, in `_build/a4/` (not committed — they are analysis scaffolding):

| Script | Purpose |
|---|---|
| `elf.py` | Segment-aware VA→file mapping; vtable dump |
| `x.py` | Direct-call and pointer search across all readable segments |
| `rip.py` | RIP-relative reference scanner |
| `norm.py` | Refcount-template normalisation and comparison |
| `alloc3.py` | Allocation-size histogram |
| `ctx.py`, `slots.py`, `c490.py` | Field-store scanners |
| `fde.py` | Function bounds from `.eh_frame_hdr` (from `wt-a9-ownership/tools/a5/`) |

---

## 10. Source references

| Report | Branch | Used for |
|---|---|---|
| `FOUNDATION_2026-11-23.md` | `research/native-rhi-foundation-20261123` | Scope, non-goals, library audit |
| `A6_shader_binding.md` | `research/a6-shader-binding-20261122` | The field chain this pass verifies |
| `A4_phase3_shader_state.md` | `research/a4-rhi-dispatch-20261121` | Writer-object pattern, `0x0CB5130` |
| `A9_ownership_lifetime.md` | `research/a9-executor-ownership-20261122` | Thread gate, executor lifetime |
| `SC09_shader_params.md` | `research/sc09-shader-params-20261123` | Effective per-draw values |
| `A028R_gnm_shader_metadata_validation.md` | `research/a028-gnm-shader-metadata-20261123` | Independent GNM oracle, slot indirection |

### 10.1 Reconciliation with prior reports

| Prior claim | Status after this pass |
|---|---|
| A6: chain `+0x468 → +0x200 → +0x1D8 → +0x40` | **Confirmed** byte-for-byte |
| A6: no `FRHICommandSetBoundShaderState` command | **Confirmed** structurally — binder is a direct method |
| A6: `+0x1D8` is "program data" | **Confirmed**, and it is a separate refcounted object |
| A6: `+0x1C0` is the GNM flags argument | **Confirmed** |
| A4: `0x0CB5130` is `+0x340` of `0x3EDFCA0` | **Confirmed**, and also `+0xC8` of `0x3EDFF18` |
| A4: writer object at `+0x18` is the choke point | **Confirmed** in `0x0CBE210` too |
| A-023: map at `shader_resource+0x172` | **Confirmed** as `+0xB0+0xC2` |
| SC-09: `sharp_idx` is a DWORD index | **Confirmed** — `+0x40+idx*4` |
| SC-09: stage labels swapped | **Accepted** per A-028R; not re-tested here |
| A9: `0x14810` is not RHI teardown | **Accepted**; not re-tested here |

---

## 11. Summary

**Proven:** the UE4 shader-resource object exists, is 472 bytes, is refcounted with UE4's
intrusive-count and deferred-delete contract, owns a separate refcounted program-data object,
and is bound to the context through a single direct method at `0x0CBE210` that receives a
complete shader state above the PS4 layer.

**Inferred:** the stage roles of the three resource slots, the thread that runs the binder,
and the `FRHIShader` naming.

**Unknown and blocking:** no portable shader program exists in the recovered chain. The
program data is GCN register arrays and a PS4 program block. Producing PC-executable shaders
requires translation or reconstruction, and this pass found no evidence either is avoidable.

**The experiment that most directly advances us:** the passive scalar observer at
`0x0CBE210` described in §8. It tests the one equality the ranking depends on, and it would
produce the first runtime evidence in this track.
