# Shader-State Slot Provenance — Correction and Backward Slice

**Date:** 2026-11-24
**Branch:** `research/native-shader-state-slots-20261124`
**Base:** `5fbe1b73` (`research/native-shader-creation-provenance-20261123`)
**Status:** Static analysis only. No runtime hook, no capture, no renderer.
**Target:** Déraciné 1.00 inner ELF `v100.elf`, SHA-256 `3F704995…A938BF`

---

## 1. Executive verdict

**Two of my previous conclusions were wrong, and both are corrected here.**

1. **The `+0xB0` "third type" argument is withdrawn.** `leaq 0xb0(%rbx), %rdx` computes an
   address inside the object. It is not a pointer load. Requiring a *store* to `+0xB0` was an
   invalid exclusion criterion, and the conclusion that rested on it does not survive.

2. **`0x3EE01C0` is not a shader-state vtable.** It is a slot **inside the context vtable**
   `0x3EDFF18`, at offset `+0x2A8`. The binder at `0x0CBE210` is context vtable slot
   **`+0x2F8`**, not a method of a separate shader-state type. My earlier reports treated
   `0x3EE01C0` as a vtable base and derived a "shader-state object" from it. That object does
   not exist as a distinct type in the evidence.

### 1.1 What this means

The binder is a **context method**. It receives a new state pointer in `rsi` and refcount-assigns
it into `context+0x468`. There is no separate "shader-state vtable" to identify, and therefore
no "third object type" was ever missing. The slot objects are whatever the caller passes.

### 1.2 What remains true

| Finding | Status |
|---|---|
| Three slots `+0x1E0`, `+0x1E8`, `+0x200` with the established stage roles | **Confirmed** |
| Slot→GNM setter mapping (PS at `+0x1E8`, VS at `+0x200`/`+0x1E0`) | **Confirmed** |
| Program-data layout (`+0x03`, `+0x24`..`+0x34`, `+0x40`, `+0x5E`) | **Confirmed by reads** |
| Binder/receiver predicate asymmetry (`+0x260`) | **Confirmed** |
| `context+0x468` is written only by the binder and the clear path | **Confirmed** |

### 1.3 What remains unknown

The **producer** of the slot objects. The backward slice from slot assignment did not close,
because the slots are populated inside an object that is constructed elsewhere and passed in
whole. §5 states the precise missing edge.

---

## 2. Correction of the `+0xB0` assumption

### 2.1 The instruction

```
cbe441  leaq 0xb0(%rbx), %rdx     ; rdx = &object->field_at_0xB0
cbe448  movq 0x1d8(%rbx), %rsi    ; rsi = object->program_data
cbe44f  callq 0x2f61d50           ; setter(writer, pd, &object->0xB0)
```

`leaq` computes an effective address. It reads no memory. The previous report treated the
absence of a *store* to `+0xB0` as proof that the object could not be a slot object. That was
wrong: an inline subobject needs no store to its own base address.

### 2.2 Exhaustive check

I scanned the binder and receiver for every access to `+0xB0`:

| Function | Accesses to `+0xB0` |
|---|---|
| Binder `0x0CBE210` | `leaq 0xb0(%rbx)` at `0xCBE441`, `leaq 0xb0(%rax)` at `0xCBE589`, `leaq 0xb0(%rbx)` at `0xCBE730` |
| Receiver `0x0CCC2F0` | **none** (only stack-relative `-0xb0(%rbp)` spills) |

**Every access is `leaq`.** There is no load and no store of a pointer at `+0xB0` in either
function. The region is addressed as an inline subobject.

### 2.3 Where the address goes

The setter `0x2F61D50` consumes the address:

```
2f61e00  movq %rdx, 0xc198(%rbx)   ; writer->+0xC198 = &object->0xB0
```

I then searched for reads of `writer+0xC198`: **zero**. The field is written and never read
in the recovered code. So the `+0xB0` address is stored but its consumer was not located.

### 2.4 The separate map pointer

The receiver uses a **different** field, `writer+0xC490`, as a loaded pointer:

```
cab438  movq 0xc490(%rdx), %rax    ; rax = writer->map
cab43f  movzwl 0xc2(%rax,%r14,2), %eax   ; map->slots[slot]
```

That is a genuine pointer load. But `+0xC490` has **no located writer** anywhere in the binary
(a full `movq`-store scan found only reads). This is the same unresolved item recorded in the
lifecycle correction, and it remains unresolved.

### 2.5 Is "shader map" justified?

**No, not as a name.** What is verified is:

- a `u16` array indexed at `+0xC2` from a base address,
- with `0xFFFF` meaning unmapped,
- and bit 15 selecting a direct-emission path.

That is a **slot-indirection table**. Calling it a "shader map" imports UE4 terminology that
the evidence does not support. This report uses "slot-indirection table" and treats "shader map"
as an unverified label inherited from A-023.

---

## 3. Correction of the vtable attribution

### 3.1 The error

Previous reports stated:

> Shader-state primary vtable `0x3EE01C0`, secondary `0x3EE0238` at `+0x08`.

### 3.2 The evidence

`0x3EE01C0` is **not** a vtable base. It sits inside the context primary vtable:

```
0x3EDFF18   context primary vtable base (installed by 0xCB2780 at object+0x00)
   +0x000  0xCB9F80
   ...
   +0x2A8  0xCBCB40   <- this is 0x3EE01C0 (slot 85)
   +0x2F8  0xCBE210   <- the binder (slot 95)
```

Arithmetic:

| Expression | Value |
|---|---|
| `0x3EE01C0 - 0x3EDFF18` | `0x2A8` (slot 85) |
| `0x3EE0210 - 0x3EDFF18` | `0x2F8` (slot 95) |

So the binder is **context primary vtable slot `+0x2F8`**.

### 3.2.1 The secondary vtable is real, but starts at 0x3EE0230

A genuine Itanium secondary-vtable header sits at `0x3EE0228`:

```
0x3EE0228  0xFFFFFFFFFFFFFFF8   ; offset_to_top = -8
0x3EE0230  0x0000000000000000   ; typeinfo = null (stripped)
0x3EE0238  0x0000000000CBF540   ; slot 0 = binder thunk
```

So the secondary vtable base is **`0x3EE0230`**, not `0x3EE0238`. The previous report used
`0x3EE0238` as the base, which is slot 0 of that vtable rather than its header.

The `-8` offset_to_top means this secondary vtable belongs to a base subobject at `+0x08` of the
complete object. That is consistent with `0xCB2780` installing `0x3EDFF18` at `+0x00` and
`0x3EE0238` at `+0x08`.

**Correction to §3.3:** the value installed at `+0x08` is `0x3EE0238`, which is the first
*slot* of the secondary vtable whose header begins at `0x3EE0230`. Both statements are true; the
header is at `0x3EE0230` and the installed pointer is `0x3EE0238`.

### 3.3 Confirmation

`0xCB2780` installs `0x3EDFF18` at `+0x00` and `0x3EE0238` at `+0x08`:

```
cb278e  leaq 0x3ee0238(%rip), %rax
cb2795  leaq 0x3edff18(%rip), %rcx
cb27b4  vmovdqu %xmm0, (%r12)      ; [0]=0x3EDFF18, [8]=0x3EE0238
cb27ba  movq %rax, 0xb20(%r12)
```

That is a **context** constructor: it installs the context vtable pair and a third vtable at
`+0xB20`. The previous report called `0xCB2780` a "shader-state constructor". It is not.

### 3.4 Consequence

The "shader-state object" that earlier reports described as having a vtable at `+0x00` and
`+0x08` is actually the **context**. The object at `context+0x468` is a separate structure whose
vtable was never identified — because it may not have one.

Note that the context itself carries a secondary vtable at `+0x08` with `offset_to_top = -8`,
which is the normal Itanium layout for a class with a base subobject at `+0x08`. The binder and
its thunk are methods of that secondary base, reached through `0x3EE0238` (slot 0) and the
primary vtable slot `+0x2F8` respectively.

---

## 4. Backward slice from slot assignment

### 4.1 Method

I enumerated every instruction form that can write to displacements `0x1E0`, `0x1E8`, `0x200`:

- `movq`/`movl`/`movw`/`movb` register-to-memory with `mod=10`
- SIB-addressed forms
- vector stores

Each hit was attributed to its enclosing function via `.eh_frame_hdr`.

### 4.2 Result

The raw counts are large (340 / 68 / 400 hits) because `0x1E0`, `0x1E8` and `0x200` are
**common offsets across many unrelated types**. Offset matching alone cannot identify a
shader-state writer, which is exactly the failure mode the task warned about.

### 4.3 The decisive narrowing

`context+0x468` is written in exactly two places:

| Site | Function | Value |
|---|---|---|
| `0xCBEE2D` | binder `0x0CBE210` | refcount-assign of `rsi` |
| `0xCD9D67` | clear path in `0xCD8FC0` | NULL |

**Therefore the state object is fully constructed by the binder's caller and passed in `rsi`.**
The binder never sees individual slot writes. This confirms the task's hypothesis in §3.3.

### 4.4 The missing edge

The binder is reached only through context vtable slot `+0x2F8`. I searched for:

- direct `callq` to `0x0CBE210` — one, from the thunk `0xCBF540`
- direct `callq` to the thunk — none
- RIP-relative references to `0x3EE0210` — none
- RELATIVE relocations targeting `0x3EE0210` — none

The invoker is therefore reached through a **computed vtable dispatch** whose base register I did
not resolve. That is the precise missing edge: **the call site that invokes context vtable slot
`+0x2F8`**.

---

## 5. Candidate family reclassification

The flawed `+0xB0` criterion is withdrawn, so the families are reconsidered on construction and
vtable evidence alone.

| Candidate | vtable | extent | program data | verdict |
|---|---|---|---|---|
| Family A | `0x3F102D8` | 784 B (`0x310` alloc) | `+0x1D8` from param struct | **Not excluded.** Cannot be confirmed or denied as a slot object. |
| Family B | `0x3F11580` | ≥682 B | `+0x1D8` from param struct | **Not excluded.** Same. |
| RHI instance | `0x3F20180` | 504 B (`0x1F8`) | `+0x1D8` | **Not excluded.** Same. |

**None of the three is confirmed as the slot object**, because the slot assignment itself was not
located. What changed is that none is *excluded* either. The previous report excluded all three on
an invalid basis.

### 5.1 What would discriminate them

The slot object must satisfy the receiver's read pattern:

```
pd = [slot + 0x1D8]
idx = [pd + 0x03]
regs = pd + 0x40 + idx*4
flags = [pd + 0x5E + idx*4]
```

All three candidates have `+0x1D8`. The discriminator is therefore **not** the object's own
layout but **which constructor's output is stored into a slot** — the missing edge in §4.4.

---

## 6. Producer-side VS decision

**Not recovered.** The receiver's rule is verified (use `+0x200` if non-null, else `+0x1E0`),
but the producer that decides which to populate was not located, because the producer is the
unresolved caller in §4.4.

### 6.1 What can be said

- The two slots are **not** interchangeable: `+0x200` uses the indexed array
  (`pd+0x40+idx*4`), `+0x1E0` uses the fixed array (`pd+8`).
- The binder additionally requires `[+0x1E0]+0x260 != NULL` before taking the primary path.
  That means `+0x1E0` must be populated **even when `+0x200` is used**, because the binder
  dereferences it unconditionally.
- Therefore the two slots are **not alternatives at the producer level**. `+0x1E0` is always
  present; `+0x200` is optional.

That is a structural inference from the binder's unconditional dereference, not an observation of
the producer.

### 6.2 SC-02 scope

SC-02's 156/156 `+0x1E0` selection describes one title-screen capture. It is not generalised.

---

## 7. Longest verified chain

```
[caller of context vtable slot +0x2F8]        <- MISSING EDGE
        │ rsi = new shader state
        ▼
0x0CBE210  binder (context vtable +0x2F8)
        │ refcount-assign into context+0x468
        ▼
context+0x468  shader state
        │ slots +0x1E0 / +0x1E8 / +0x200
        ▼
0x0CCC2F0  receiver
        │ selects +0x200 if non-null else +0x1E0; +0x1E8 separately
        ▼
pd = [slot+0x1D8]  program data (refcounted)
        │ idx = [pd+0x03]; regs = pd+0x40+idx*4
        ▼
0x33C81B0 / 0x33C8340  GNM shader setters
        ▼
0x0CBB9C0  indexed draw
```

**Verified from the binder downward.** The edge above the binder is missing.

---

## 8. Correction table

| # | Previous claim | Verdict | Corrected |
|---|---|---|---|
| 1 | Slot object must be a third type because no ctor writes both `+0xB0` and `+0x1D8` | **Withdrawn** | `+0xB0` is `leaq`-addressed inline; no store required |
| 2 | `0x3EE01C0` is the shader-state vtable | **Corrected** | It is context primary vtable slot `+0x2A8` (slot 85) |
| 3 | `0x3EE0238` is the shader-state secondary vtable | **Corrected** | It is slot 0 of the context secondary vtable, whose header is at `0x3EE0230` |
| 4 | Binder is a shader-state method | **Corrected** | Binder is context vtable slot `+0x2F8` |
| 5 | `0xCB2780` is the shader-state constructor | **Corrected** | It is the **context** constructor |
| 6 | `+0xB0` is a "shader map" | **Unsupported** | It is a slot-indirection table; the name is inherited, not verified |
| 7 | Families A/B/RHI excluded as slot objects | **Withdrawn** | None is excluded; none is confirmed |
| 8 | Three slots with PS/VS roles | **Confirmed** | — |
| 9 | Program-data layout | **Confirmed** | — |
| 10 | Binder/receiver `+0x260` asymmetry | **Confirmed** | — |
| 11 | `context+0x468` written only by binder and clear | **Confirmed** | — |
| 12 | `writer+0xC490` has no located writer | **Confirmed unresolved** | — |
| 13 | `+0x158` constants 4/6 are frequencies | **Unresolved** | No read-side evidence |

---

## 9. Implications for a native renderer

1. **The binder is a context method, not a resource interface.** Intercepting it gives a state
   pointer, not a shader object. A native backend must resolve the state's slots itself.
2. **The slot-indirection table is the real binding metadata.** Its base is passed by address
   (`leaq`), so it is inline in the slot object. A native backend can read it directly from the
   object once the object is identified.
3. **The producer is the missing piece.** Without it, a native backend cannot know when a shader
   becomes available, only that one is currently bound.
4. **No shader identity was recovered.** The execution blocker is unchanged.

---

## 10. Next experiment

**H3:** the call site invoking context vtable slot `+0x2F8` is a direct call in a function that
also constructs the state object, and that function's state allocation size matches one of the
three candidate families.

### Method

1. Find all indirect calls of the form `callq *0x2F8(%reg)` in the text.
2. For each, check whether the base register was loaded from an object whose vtable is
   `0x3EDFF18` (the context).
3. In the enclosing function, find the allocation that produces the `rsi` argument.
4. Compare that allocation size against 504, 784 and ≥682.

### Falsification

| Outcome | Meaning |
|---|---|
| Exactly one call site, allocation matches a candidate | H3 supported; that family is the slot object |
| Multiple call sites | Discriminate by which context the call targets |
| No `*0x2F8` call found | The binder is invoked through a different slot or a copied vtable; search must widen |

---

## 11. Reproduction

### 11.1 Automated verifier

```
python tools/research/verify_shader_state_slots.py <path-to-v100.elf>
```

Ten check groups, all passing against the pinned ELF:

| Group | What it pins |
|---|---|
| S1 | `+0xB0` is `leaq`-addressed; no pointer load in binder or receiver |
| S2 | The setter consumes the address into `writer+0xC198` |
| S3 | `0x3EE01C0` is context primary vtable slot `+0x2A8`; binder is `+0x2F8` |
| S4 | Secondary vtable header at `0x3EE0230` with `offset_to_top = -8` |
| S5 | `0xCB2780` installs the context vtable pair |
| S6 | `context+0x468` written only by binder and clear path |
| S7 | Receiver tests `+0x200`, not `+0x1E0`; array is `pd+0x40+idx*4` |
| S8 | Binder requires `[+0x1E0]+0x260` and tests bit 1 of the flag byte |
| S9 | Family A/B extents fit their constructors |
| S10 | The unresolved binder invoker is reported explicitly |

The verifier rejects a wrong SHA-256 by default (tested against `v101.elf`, exit 1) and raises
`DisasmError` on empty disassembly rather than passing silently.

### 11.2 Manual checks

```
# The leaq that the previous report misread as a pointer load
llvm-objdump -d --start-address=0x0CBE436 --stop-address=0x0CBE458 --no-show-raw-insn v100.elf

# The setter consuming the address
llvm-objdump -d --start-address=0x2F61DF0 --stop-address=0x2F61E10 --no-show-raw-insn v100.elf

# The context vtable and the binder's real slot
python elf.py vt 0x3EDFF18 110
python elf.py q 0x3EE0210 0x3EE0238 0x3EDFFE0

# The context constructor
llvm-objdump -d --start-address=0xCB2780 --stop-address=0xCB27C0 --no-show-raw-insn v100.elf

# context+0x468 writers
llvm-objdump -d --start-address=0x0CBEE20 --stop-address=0x0CBEE60 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x0CD9D60 --stop-address=0x0CD9D78 --no-show-raw-insn v100.elf
```

### Provenance

| Item | Value |
|---|---|
| ELF | `v100.elf`, SHA-256 `3F704995…A938BF` |
| SELF | Déraciné 1.00, SHA-256 `8caee78e…7011a0` |
| Toolchain | `llvm-objdump` 23.1.5, Python 3.15 |
| Function bounds | `.eh_frame_hdr`, 180,806 entries |

---

## 12. Summary

**Corrected:** the `+0xB0` direct-store requirement, the `0x3EE01C0` vtable attribution, the
binder's type, the `0xCB2780` constructor's type, and the exclusion of all three candidate
families.

**Confirmed:** the three slots and their stage roles, the program-data layout, the binder/receiver
asymmetry, and that `context+0x468` is written only by the binder.

**Missing:** the call site that invokes context vtable slot `+0x2F8`. That single edge is what
separates "a shader is bound" from "a shader was created", and §10 gives the search that would
find it.
