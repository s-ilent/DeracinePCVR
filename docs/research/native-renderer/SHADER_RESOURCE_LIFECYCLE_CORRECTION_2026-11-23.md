# Shader-Resource Lifecycle — Correction and Binder Validation

**Date:** 2026-11-23
**Branch:** `research/native-shader-resource-correction-20261123`
**Base:** `origin/research/native-shader-resource-lifecycle-20261123` @ `58379164`
**Supersedes:** `SHADER_RESOURCE_LIFECYCLE_2026-11-23.md` (partially — see §1.3)
**Status:** Static analysis only. No runtime hook, no capture, no native GPU prototype.
**Target:** Déraciné 1.00 inner ELF `v100.elf`
**ELF SHA-256:** `3F70499588A9A51781DE87E2999433B85A6D23A4307DC44827134C8B71A938BF`

---

## 1. Executive verdict

### 1.1 What survives

| Claim | Status |
|---|---|
| The A6 field chain `context+0x468 → +0x200 → +0x1D8 → +0x40+idx*4` | **Confirmed** byte-for-byte |
| `0x0CBE210` is a direct context method that refcount-assigns `context+0x468` | **Confirmed** |
| The refcount template (count `+0x08`, delete-pending `+0x0C`, deferred flag `+0x10`) | **Confirmed** |
| Program data at `resource+0x1D8` is a separate refcounted object | **Confirmed** |
| The shader map slot indirection (`u16`, `0xFFFF`, bit 15) | **Confirmed** |
| `0xB0 + 0xC2 = 0x172` reconciles A-023 | **Confirmed** |
| `0x0CB5130` is `primary+0x340` and `secondary+0xC8` | **Confirmed** |

### 1.2 What changes

**The 472-byte shader-instance claim is withdrawn.** It was wrong, and the error was
structural rather than arithmetic: 472 bytes is the size of the **class-descriptor object**
allocated by the factory at `0x135B4F0`, not the shader-resource instance. The instance is
**504 bytes (`0x1F8`)**, and that number is stated by the descriptor itself.

**The Rank 1 runtime hypothesis was wrong in its premise.** It required
`shader_state+0x200` to be non-null. SC-02 observed 156 of 156 draws resolving through
`+0x1E0` with `+0x200` null. The hypothesis would have failed on the first real draw.

**The three slots are not three stages.** They are two independent stage slots plus one
alternate vertex-shader slot, selected by null-test, not by stage identity.

### 1.3 What remains unknown

- The exact predicate that decides *which* of `+0x200` and `+0x1E0` is populated. The
  receiver's selection is a null-test, but the *producer* that chooses to populate one or the
  other was not located.
- Whether `+0x1E0` and `+0x200` ever hold different program data for the same draw.
- The thread affinity of `0x0CBE210` (inherited from A9, not observed).
- No portable shader program exists anywhere in the chain. This is unchanged and still the
  hard blocker for execution.

---

## 2. The allocation contradiction, resolved

### 2.1 The contradiction

The previous report claimed all of:

1. Shader-resource allocation size `0x1D8` (472 bytes).
2. Shader-resource program-data pointer at `+0x1D8`.
3. Factory `0x135B4F0` allocates `0x1D8` bytes.
4. Constructor `0x135B9B0` stored at factory offset `+0x98`.

(1) and (2) cannot both hold: an eight-byte pointer at `+0x1D8` needs at least 480 bytes.

### 2.2 The resolution

There are **two distinct objects**, and the previous report conflated them.

**Object A — the class descriptor.** Allocated by `0x135B4F0` at `0x135B51E`:

```
135b51e:  movl $0x1d8, %edi        ; 472 bytes
135b523:  movl $0x1, %esi
135b528:  callq 0x103c0c0          ; allocator
135b534:  movq %rax, %rbx          ; rbx = descriptor
...
135b5c8:  leaq 0x3e1(%rip), %rdx   ; = 0x135b9b0  (constructor)
135b5e0:  movq %rdx, 0x98(%rbx)    ; descriptor->+0x98 = ctor
135b5e7:  movq %rcx, 0xa0(%rbx)    ; descriptor->+0xA0 = 0x135bbb0
135b5ee:  movq %rax, 0xa8(%rbx)    ; descriptor->+0xA8 = 0xf65930
135b56f:  movabsq $0x1000001f8, %rdx
135b5a2:  movq %rdx, 0x40(%rbx)    ; descriptor->+0x40 = 0x1000001f8
```

The highest byte the factory writes through `%rbx` is `+0x1D7` (a 32-byte `vmovups` at
`+0x1C8`). So **472 bytes is exactly right for the descriptor** and it is fully used.

**Object B — the shader-resource instance.** Its size is not a literal in the factory. It is
the **low dword of the descriptor's `+0x40` field**: `0x1000001F8` → `0x1F8` = **504 bytes**.

The instance is allocated by the generic instantiation helper `0x1038500`, which reads the
size from the descriptor:

```
103865f:  movq %rax, -0x40(%rbp)
1038663:  movl 0x40(%rbx), %edi    ; size = descriptor->+0x40 (low dword) = 0x1F8
1038666:  movzbl 0x34a1a9b(%rip), %esi
1038676:  callq 0x103c0c0          ; allocate 504 bytes
103867b:  movq %rax, %r12          ; r12 = instance
```

### 2.3 The construction graph, verified

```
 0x135B4F0  factory (singleton, guarded by 0x47B2968)
   │
   ├─ alloc(0x1D8) ──────────────────────► Object A: class descriptor (472 bytes)
   │                                          +0x00 = vtable 0x3EECC90
   │                                          +0x40 = 0x1000001F8   (size | flags)
   │                                          +0x98 = 0x135B9B0     (constructor)
   │                                          +0xA0 = 0x135BBB0     (no-op)
   │                                          +0xA8 = 0xF65930
   │                                          +0xB4 = 0x10000080    (capability flags)
   │                                          0x47B2968 = descriptor
   │
   └─ 0xF0D4A0(descriptor, ...)  ─────────► registration

 0xF10910  instantiation helper (r12 = descriptor)
   │
   ├─ 0x1038500(descriptor, ...) ─────────► alloc(descriptor->+0x40 & 0xFFFFFFFF)
   │                                          = alloc(0x1F8) = 504 bytes
   │                                          returns instance in %rax
   ├─ movq %rbx, 0xF8(%r12)  ─────────────► descriptor caches the instance
   ├─ testl $0x10000080, 0xB4(%r12) ──────► confirms r12 is the descriptor
   ├─ movq 0x98(%r12), %r15  ─────────────► load constructor from descriptor
   └─ callq *%r15  ───────────────────────► 0x135B9B0(instance)
                                                +0x00 = vtable 0x3F20180
                                                +0x28 = vtable 0x3F203D8
                                                writes up to +0x1F0
```

The `testl $0x10000080, 0xB4(%r12)` at `0xF10CA0` is the decisive identity check: that
exact constant is written by the factory at `0x135B5FF`. It proves `%r12` in the
instantiation helper is the same descriptor object the factory built.

### 2.4 Why the previous report got it wrong

It inferred the instance size from an allocation histogram. `0x1D8` is by far the most
common allocation size in this binary (5,031 sites), because it is the size of a widely used
descriptor base class. The histogram could not distinguish the descriptor from the instance,
and the report did not check whether the constructor's write extent fit the allocation it had
attributed to it. It did not: the constructor writes to `+0x1F0`, which is 497 bytes.

**The corrected rule:** instance size comes from the descriptor's `+0x40` field, and is
cross-checked against the constructor's maximum write offset. Both give a consistent answer
here; the histogram gave neither.

### 2.5 Vtable and destructor association — still valid

The vtable association survives the correction, because it was never derived from the size:

| Item | Value | Evidence |
|---|---|---|
| Instance vtable | `0x3F20180` | Written at `0x135B9E5` in the constructor |
| Instance secondary vtable | `0x3F203D8` at `+0x28` | Written at `0x135B9FF` |
| Destructor | `0x135BBC0` | `vtable[1]` of `0x3F20180`; installs `0x3EF8CB0` then frees |
| Descriptor vtable | `0x3EECC90` | Written at `0x135B5C5` |

The destructor at `0x135BBC0` is reached through `vtable[1]` from the refcount template, and
it operates on the same `+0x28` secondary base. That is consistent with a 504-byte instance
carrying a `+0x28` base subobject.

---

## 3. The three slots, reconciled with SC-02

### 3.1 What SC-02 observed

156 of 156 indexed draws resolved their shader through `shader_state+0x1E0`, because
`shader_state+0x200` was null on every one. Zero resolved through `+0x200`.

### 3.2 The receiver's actual selection flow

`0x0CCC2F0` reads the slots in a fixed order with explicit null-tests:

```
ccd02d:  movq 0x468(%r12), %rax     ; rax = shader_state
ccd035:  testq %rax, %rax
ccd038:  je    0xccd418             ; no state at all -> skip

; --- slot +0x1E8 : PIXEL stage ---
ccd03e:  movq 0x1e8(%rax), %rax
ccd045:  testq %rax, %rax
ccd048:  je    0xccd08c             ; null -> emit PS with a null array
ccd04a:  movq 0x1d8(%rax), %r12     ; program data
...
ccd08a:  jmp   0xccd0c0
ccd0c0:  callq 0x33c8340            ; sceGnmSetPsShader350(cmd, 0x28, array)

; --- slot +0x200 : VERTEX stage, primary ---
ccd0d0:  movq -0x48(%rbp), %rax
ccd0d4:  movq 0x468(%rax), %rax
ccd0db:  movq 0x200(%rax), %r13
ccd0e2:  testq %r13, %r13
ccd0e5:  je    0xccd1aa             ; null -> alternate path
ccd0eb:  movl 0x1c0(%rax), %r15d    ; modifier
ccd0f6:  movq 0x1d8(%r13), %r12     ; program data
ccd103:  movzbl 0x3(%r12), %ebx     ; register-array selector
ccd12c:  leaq 0x40(%r12,%rbx,4), %rdx
ccd139:  callq 0x33c81b0            ; sceGnmSetVsShader(cmd, 0x1D, regs, modifier)

; --- slot +0x1E0 : VERTEX stage, alternate ---
ccd1aa:  movq 0x1e0(%rax), %rcx
ccd1b1:  movl 0x1c0(%rax), %ebx     ; same modifier field
ccd1c5:  movq 0x1d8(%rcx), %r12     ; program data
ccd1f3:  addq $0x8, %r12            ; program data + 8
ccd201:  callq 0x33c81b0            ; sceGnmSetVsShader(cmd, 0x1D, pd+8, modifier)
```

### 3.3 The verified slot model

| Slot | GNM setter | Size arg | Array argument | Role |
|---|---|---|---|---|
| `+0x1E8` | `sceGnmSetPsShader350` (`0x33C8340`) | `0x28` (40) | `pd+8` | **Pixel stage** |
| `+0x200` | `sceGnmSetVsShader` (`0x33C81B0`) | `0x1D` (29) | `pd+0x40+idx*4` | **Vertex stage, primary** |
| `+0x1E0` | `sceGnmSetVsShader` (`0x33C81B0`) | `0x1D` (29) | `pd+8` | **Vertex stage, alternate** |

This is direct evidence, not inference: each slot's load is followed within a few instructions
by a specific GNM import call with a specific size argument.

**The stage roles are now evidence-backed for `+0x1E8` (PS) and `+0x200`/`+0x1E0` (both VS).**
The previous report's guess that `+0x1E0`/`+0x1E8` were "PS and VS" was half right: `+0x1E8`
is PS, but `+0x1E0` is a *second vertex* slot, not PS.

### 3.4 What the two vertex slots differ in

They are not interchangeable. The array argument differs:

- `+0x200` passes `pd + 0x40 + idx*4` — the indexed register array, selected by the byte at
  `pd+0x03` (`0xCCD103`, `0xCCD12C`).
- `+0x1E0` passes `pd + 8` — a fixed offset, with no index selection (`0xCCD1C5`, `0xCCD1F3`).

The pixel path at `+0x1E8` also passes `pd + 8` (`0xCCD04A`, `0xCCD07E`), so the fixed-offset
form is shared between the pixel stage and the alternate vertex path, while the indexed form is
unique to the primary vertex slot.

So `+0x200` is the **indexed/variant** vertex path and `+0x1E0` is the **fixed** vertex path.
The binder corroborates this: at `0xCBE3C5` it reads `pd+0x03` and tests bit 1 of
`pd+0x5E+idx*4` to choose between two sub-paths for the `+0x200` slot, while the `+0x1E0`
path at `0xCBE56D` has no such index logic.

**This is a hypothesis about intent, not a proven semantic.** What is proven is that the two
paths compute different array pointers from the same program data.

### 3.5 The binder agrees with the receiver

`0x0CBE210` uses the same three slots and the same setters:

| Slot | Binder setter | Call site |
|---|---|---|
| `+0x200` | `0x2F61D50` | `0xCBE44F` |
| `+0x1E0` | `0x2F61D50` | `0xCBE44F` (shared path) |
| `+0x1E8` | `0x2F62350` | `0xCBE21` |

The binder's `+0x200` path additionally requires `state+0x1E0` to be non-null and to have a
non-null `+0x260` sub-object (`0xCBE3AD`–`0xCBE3BF`); if either is null it falls through to
the alternate path at `0xCBE459`. That is a **second, independent** reason the primary slot
may not be used, beyond the slot itself being null.

### 3.6 Reconciling SC-02

SC-02's observation is fully consistent with this model:

- `+0x200` was null on every captured draw, so the receiver took the `0xCCD1AA` branch.
- That branch reads `+0x1E0`, which was populated, and emitted a vertex shader.
- SC-02 correctly labelled its association "the vertex shader slot" — it is.
- SC-02's note that "the pixel shader is set through a different path" is also correct: the
  PS path is the `+0x1E8` block at `0xCCD03E`, which SC-02 did not capture.

**No contradiction remains.** The previous report's error was assuming `+0x200` was the
canonical slot; it is the *primary* slot, and the engine may legitimately leave it null.

---

## 4. Re-evaluating `0x0CBE210` as the interception boundary

### 4.1 Calling convention, verified

```
cbe210:  pushq %rbp
cbe221:  movq %rdi, %r14          ; r14 = context (this)
cbe224:  movq 0x460(%r14), %rbx   ; rbx = context->writer
cbe28d:  movq %rsi, -0x50(%rbp)   ; new_state saved to stack
cbe291:  movq 0x1c8(%rsi), %rbx   ; new_state->+0x1C8 (a refcounted object)
cbe2a1:  leaq 0xac8(%r14), %rdi
cbe2ab:  callq 0xcbf700           ; refcount-assign context->+0xAC8
...
cbee2d:  leaq 0x468(%r14), %rdi
cbee34:  movq %r15, %rsi          ; r15 = new_state
cbee55:  callq 0xcbfa40           ; refcount-assign context->+0x468
```

**Signature:** `void __thiscall binder(Context* this /*rdi*/, ShaderState* new_state /*rsi*/)`.

The new state is **retained by refcount** into `context+0x468`, replacing the previous state.
That is a real ownership transfer, not a borrowed pointer.

### 4.2 What is available at entry versus after binding

| Information | At entry | After binding |
|---|---|---|
| New shader state pointer | Yes (`rsi`) | — |
| Context pointer | Yes (`rdi`) | — |
| Previous state | Yes (`context+0x468`) | Replaced |
| Slot contents (`+0x1E0/1E8/200`) | Yes, via `rsi` | Unchanged |
| Program data pointers | Yes, via slot `+0x1D8` | Unchanged |
| Modifier (`+0x1C0`) | Yes, via `rsi` | Unchanged |
| Which slot the *receiver* will later pick | **No** | **No** |

The last row is the important limitation. The binder does not decide slot selection; the
receiver does, at draw time, by null-test. The binder's own branch structure
(`0xCBE3A7`, `0xCBE3BF`) mirrors the receiver's, but it is a *separate* decision made at
binding time, and the two can disagree if the state is mutated between binding and draw.

### 4.3 Resources referenced versus resources selected

This distinction was missing from the previous report and matters for any observer:

- **Referenced:** all three slots are read by the binder and by the receiver.
- **Selected for GNM emission:** exactly one vertex slot (`+0x200` if non-null, else `+0x1E0`)
  and the pixel slot (`+0x1E8` if non-null, else a null-array PS emission).

An observer that records "the shader resource" without recording *which slot was selected*
records an ambiguous fact. This is the single most important correction to the observer design.

### 4.4 Is `0x0CBE210` sufficient to associate state with subsequent draws?

**Partially, and only with the selection predicate applied.**

The binder gives a semantic state object and its refcounted retention. It does **not** give:

- the draw that will consume it;
- which vertex slot the receiver will choose;
- whether the state is replaced again before the draw.

So the binder is a *producer* event, and the receiver is the *consumer* event. A correct
association needs both, joined on the context pointer and ordered by time.

### 4.5 Revised ranking

| Rank | Point | Why |
|---|---|---|
| **1** | `0x0CBE210` binder **plus** `0x0CCC2F0` receiver | The binder gives semantic identity and lifetime; the receiver gives the actual selection. Neither alone is sufficient. |
| 2 | `0x0CCC2F0` receiver alone | Gives the true selected resource per draw, but loses the semantic state object and its lifetime. |
| 3 | `0x0CB5130` recorder | Broader, noisier, not draw-specific. |
| 4 | `0x135B9B0` constructor | Resource identity only; no program data at construction. |
| 5 | `0x13764D0` executor | Sees no shader binding at all. |

**The previous Rank 1 was too optimistic.** `0x0CBE210` alone cannot predict what a draw
will use, because slot selection happens later and elsewhere. The corrected Rank 1 is a
**pair**, and the report should not have called a single function the boundary.

### 4.6 Why the binder is still worth instrumenting

It is the only point where the engine commits a *semantic* shader state with refcounted
lifetime. The receiver sees only the resolved PS4 payload. For a native backend that needs to
cache translated programs against a stable engine-side identity, the binder is the right place
to mint that identity — provided the observer also records the receiver's selection so the
identity can be tied to an actual draw.

---

## 5. Revised runtime-observer specification

**Not implemented. Not executed. Requires separate authorization.**

### 5.1 Primary hypothesis

> **H1:** The semantic shader state received by `0x0CBE210`, after applying the verified
> resource-selection predicates, predicts the shader-resource identity observed at subsequent
> draws on the same context.

This replaces the previous hypothesis, which assumed `+0x200` was always populated.

### 5.2 Observer design

Two passive probes, both default-off, both scalar-only.

**Probe A — binder event, at `0x0CBE210` entry.**

Record, per call:

| Field | Source | Notes |
|---|---|---|
| `seq` | monotonic counter | Global ordering |
| `thread_id` | `scePthreadGetthreadid` | Do not infer from function name |
| `context` | `rdi` | Opaque value |
| `new_state` | `rsi` | Opaque value |
| `prev_state` | `[rdi+0x468]` | Opaque value, read before the assign |
| `slot_1e0` | `[rsi+0x1E0]` | Opaque, may be null |
| `slot_1e8` | `[rsi+0x1E8]` | Opaque, may be null |
| `slot_200` | `[rsi+0x200]` | Opaque, may be null |
| `pd_1e0` | `[[rsi+0x1E0]+0x1D8]` | Only if slot non-null |
| `pd_1e8` | `[[rsi+0x1E8]+0x1D8]` | Only if slot non-null |
| `pd_200` | `[[rsi+0x200]+0x1D8]` | Only if slot non-null |
| `modifier` | `[rsi+0x1C0]` | `u32` |
| `sub260_present` | `[[rsi+0x1E0]+0x260] != 0` | The binder's second predicate |
| `pd_200_idx` | `[[[rsi+0x200]+0x1D8]+0x03]` | `u8` register-array selector |
| `pd_200_flag` | `[[[rsi+0x200]+0x1D8]+0x5E+idx*4]` | `u8` per-array flags |

**Probe B — receiver event, at `0x0CCC2F0` after slot selection.**

Record, per call:

| Field | Source | Notes |
|---|---|---|
| `seq` | same counter | Shares ordering with Probe A |
| `thread_id` | `scePthreadGetthreadid` | |
| `context` | `[rbx]` where `rbx` is the receiver's `this` | |
| `state` | `[context+0x468]` | The state actually consumed |
| `selected_vs_slot` | `0x200` or `0x1E0` | **The critical field** |
| `selected_vs_pd` | the `+0x1D8` actually used | |
| `selected_ps_slot` | `0x1E8` or `none` | |
| `selected_ps_pd` | the `+0x1D8` actually used | |
| `modifier` | `[state+0x1C0]` | |
| `vs_array_offset` | `0x40+idx*4` or `+8` | Distinguishes the two VS paths |

### 5.3 Constraints the observer must honour

1. **Scalars and opaque identities only.** No dereferencing of program data beyond the
   specific scalar fields listed. No shader bytecode is read, hashed or exported.
2. **No retained guest pointers.** Every value is copied at the probe site. Nothing is
   dereferenced after the call returns.
3. **Bounded.** Fixed-size ring buffer, compile-time capacity, no allocation in the probe.
4. **Null-safe.** Every slot read is guarded. A null slot records as null, not as zero.
5. **Populated ≠ selected.** Probe A records population; Probe B records selection. They are
   separate fields and must never be conflated.
6. **Ordering.** Both probes share one monotonic counter so binder and receiver events
   interleave correctly.
7. **Thread identity.** Recorded from `scePthreadGetthreadid` at the probe site, never
   inferred.
8. **Default off.** With the environment variable unset, no probe installs and no guest
   memory is read.

### 5.4 Correlation method

For each receiver event, find the most recent binder event on the same `context` with a
lower `seq`. That pair is the association under test. If no such binder event exists, the
receiver event is **inconclusive**, not negative.

### 5.5 Acceptance criteria

**Positive (H1 confirmed):** for at least 100 consecutive receiver events, the
`selected_vs_pd` equals the `pd_200` or `pd_1e0` of the most recent preceding binder event on
the same context, **and** the choice between them matches the predicate
"use `+0x200` iff non-null and its `+0x260` sub-object is non-null, else `+0x1E0`", **and**
the same holds for the PS slot.

**Negative (H1 falsified):** any receiver event whose `selected_vs_pd` matches neither
`pd_200` nor `pd_1e0` of the preceding binder event on that context, or whose slot choice
contradicts the predicate while both slots are non-null.

**Inconclusive:** no preceding binder event on that context; or the state pointer changed
between binder and receiver; or the receiver ran on a thread with no observed binder.

### 5.6 Why this is stronger than matching `program_data+0x06`

The previous criterion compared one `u16` against a PM4 field. That is weak for three
reasons:

1. A `u16` match is not identity. The field could be a register value, a mode, or a
   coincidence. The previous report itself flagged this risk and then used it anyway.
2. It tested one slot, on a path SC-02 showed is usually null.
3. It did not test the *selection* at all, which is the actual open question.

The new criterion tests a **structural prediction**: that a specific state object, retained by
a specific refcounted assignment, produces a specific resource selection at a later draw,
under a predicate derived from the disassembly. That is falsifiable in a way a scalar match
is not.

---

## 6. Correction table

| # | Original claim | Verdict | Corrected value |
|---|---|---|---|
| 1 | Shader-resource instance is 472 bytes (`0x1D8`) | **Corrected** | 504 bytes (`0x1F8`), from descriptor `+0x40` |
| 2 | Factory `0x135B4F0` allocates the instance | **Corrected** | It allocates the 472-byte **class descriptor** |
| 3 | Constructor stored at factory `+0x98` | **Confirmed** | Data field, not vtable slot; called via `0xF10CAC` |
| 4 | Program-data pointer at `+0x1D8` | **Confirmed** | Fits inside 504 bytes |
| 5 | Instance vtable `0x3F20180`, destructor `0x135BBC0` | **Confirmed** | Unaffected by the size correction |
| 6 | `+0x1E0`/`+0x1E8` are "PS and VS" (inferred) | **Corrected** | `+0x1E8` = PS; `+0x1E0` = alternate **VS** |
| 7 | `+0x200` is the canonical shader slot | **Corrected** | It is the *primary* VS slot; may be null |
| 8 | Runtime test requires `+0x200` non-null | **Corrected** | Must handle all three slots and record selection |
| 9 | `0x0CBE210` alone is the Rank 1 boundary | **Corrected** | Rank 1 is the binder **plus** receiver pair |
| 10 | `program_data+0x06` match is sufficient proof | **Unsupported** | Replaced by a structural selection test |
| 11 | Refcount template identity | **Confirmed** | Four mnemonic-identical instantiations |
| 12 | Program data is a separate refcounted object | **Confirmed** | `lock incl 0x8(%rax)` after the store |
| 13 | Map indirection `0xB0+0xC2=0x172` | **Confirmed** | |
| 14 | `0x0CB5130` is `+0x340` and `+0xC8` | **Confirmed** | |
| 15 | Binder runs on the RHI thread | **Unresolved** | Inherited from A9; not observed |
| 16 | Portable shader program exists in the chain | **Unsupported** | No HLSL/DXIL/SPIR-V found; unchanged blocker |
| 17 | `writer+0xC490` has no located writer | **Unresolved** | Still unresolved |
| 18 | The two "shader setters" are not GNM stubs | **Confirmed** | No `callq 0x33C8xxx` in either body |

---

## 7. Reproduction

### 7.1 Automated checks

```
python tools/research/verify_shader_resource_correction.py <path-to-v100.elf>
```

Eight check groups, all passing against the pinned ELF. The script verifies the factory
extent, the size descriptor, the constructor extent, the constructor data field, the receiver's
slot reads and null-tests, the slot-to-setter mapping, the binder's convention, and the
descriptor-to-instance construction chain. It exits non-zero on the first failure and warns if
the ELF hash differs from the pinned value.

### 7.2 Manual checks

```
# The factory allocates 472 bytes and stores the instance size at +0x40
llvm-objdump -d --start-address=0x135B51E --stop-address=0x135B5B0 --no-show-raw-insn v100.elf

# The constructor writes past 472 bytes
llvm-objdump -d --start-address=0x135BB90 --stop-address=0x135BBB0 --no-show-raw-insn v100.elf

# The instantiation helper reads the size from the descriptor
llvm-objdump -d --start-address=0x103865F --stop-address=0x1038680 --no-show-raw-insn v100.elf

# The instantiation helper calls the descriptor's +0x98 field
llvm-objdump -d --start-address=0xF10C98 --stop-address=0xF10D40 --no-show-raw-insn v100.elf

# The receiver's three-slot selection
llvm-objdump -d --start-address=0x0CCD02D --stop-address=0x0CCD210 --no-show-raw-insn v100.elf

# The binder's convention and refcount assign
llvm-objdump -d --start-address=0x0CBE210 --stop-address=0x0CBE2B0 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x0CBEE20 --stop-address=0x0CBEE60 --no-show-raw-insn v100.elf
```

### 7.3 Provenance

| Item | Value |
|---|---|
| Target ELF | `v100.elf`, SHA-256 `3F704995…A938BF` |
| SELF | Déraciné 1.00, SHA-256 `8caee78e…7011a0` |
| Toolchain | `llvm-objdump` 23.1.5, Python 3.15 |
| Function bounds | `.eh_frame_hdr`, 180,806 entries |
| Prior report | `SHADER_RESOURCE_LIFECYCLE_2026-11-23.md` @ `58379164` |
| SC-02 source | `research/sc02-draw-shader-20261122:reports/SC02_draw_shader_association.md` |

---

## 8. Implications for the native PC renderer

### 8.1 What this changes architecturally

The native backend cannot treat "the shader resource" as a single per-draw fact. It must model:

- **two stage slots** (PS at `+0x1E8`, VS at `+0x200`/`+0x1E0`), each independently nullable;
- **a selection predicate** for the VS slot that depends on `+0x200` being non-null *and* its
  `+0x1E0` companion having a non-null `+0x260` sub-object;
- **two distinct VS array paths** (indexed at `pd+0x40+idx*4`, fixed at `pd+8`) that produce
  different register arrays from the same program data.

A backend that assumes one shader per draw, or that assumes the primary slot is populated,
will mis-bind on this title. SC-02's 156/156 alternate-slot result is direct evidence.

### 8.2 What does not change

- **No portable shader program exists.** The program data is GCN register arrays and a PS4
  program block. Nothing in this pass found HLSL, DXIL or SPIR-V. Shader translation or
  reconstruction remains the hard blocker for execution.
- **Binding is understood; execution is not.** This correction improves the binding model. It
  does not move the execution blocker.
- **The refcount and lifetime model is engine-level and reusable.** That part of the previous
  report stands and is the most portable finding in the track.

### 8.3 The next honest step

The observer in §5 is the correct next experiment, and it is now specified against the real
slot model rather than an assumed one. It still requires separate authorization for runtime
instrumentation, and it still cannot produce a renderable frame — only a validated association
between a semantic state and a draw.

---

## 9. Summary

**Proven:** the shader-resource instance is 504 bytes, allocated from a size carried by a
472-byte class descriptor; the constructor is invoked indirectly through that descriptor's
`+0x98` data field; the receiver selects among three slots by null-test; `+0x1E8` is the
pixel stage and `+0x200`/`+0x1E0` are two vertex-stage paths with different array
computation; the binder retains the state by refcount into `context+0x468`.

**Corrected:** the 472-byte instance claim, the "three stages" reading, the assumption that
`+0x200` is canonical, and the Rank 1 single-function boundary.

**Unresolved:** which producer populates `+0x200` versus `+0x1E0`; the binder's thread
affinity; `writer+0xC490`'s writer; and the absence of any portable shader program.

**The disproven hypothesis is the useful outcome here.** Building the native renderer around
`0x0CBE210` as a single boundary, with a test that required `+0x200` to be populated, would
have failed on the first real draw — exactly the failure SC-02 had already measured.
