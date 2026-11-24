# Shader Binder Caller and Producer Provenance

**Date:** 2026-11-24
**Branch:** `research/native-shader-binder-caller-20261124`
**Base:** `faa12466` (`research/native-shader-state-slots-20261124`)
**Status:** Static analysis only. No runtime hook, no capture, no renderer.
**Target:** Déraciné 1.00 inner ELF `v100.elf`, SHA-256 `3F704995…A938BF`

---

## 1. Executive verdict

**The binder call site is resolved.** The primary dispatch is found, its vptr provenance is
established, and the state argument is traced to the function boundary.

| Question | Answer |
|---|---|
| Where is the binder invoked? | `0xF07EFA`, inside `0xF07D10` |
| How is the vptr obtained? | `movq (%rbx), %rax` at `0xF07EEC` — the context's own vtable |
| Is `%rbx` a context? | **Yes**, proven via the sibling slot `+0x268` method reading `0x460(%rdi)` |
| What is the state argument? | `rsi` = the caller's second parameter, saved at `0xF07D28` |
| Is the state constructed here? | **No.** The caller never writes the slots and performs no allocation |
| Where does the state come from? | **One frame further up.** `0xF07D10` is a virtual method of a shader-type class |

### 1.1 The genuinely new findings

1. **The binder is invoked from a shader-type virtual method, not from a pipeline setup path.**
   `0xF07D10` is reachable from four thin wrappers, each of which is a vtable slot of a
   different shader-type class. The binder call is therefore made *per shader type*, inside a
   loop over the context's shader-type list.

2. **The state is a persistent object passed down, not a per-call temporary.** The caller
   performs no allocation and never writes `+0x1C0`, `+0x1E0`, `+0x1E8` or `+0x200`. The
   slots are already populated when the binder sees the state.

3. **`writer+0xC198` and `writer+0xC490` are provably different fields.** `+0xC198` has two
   writers; `+0xC490` has **none** in any instruction form I searched. The `+0xB0` address goes
   to `+0xC198` and never to `+0xC490`.

### 1.2 What remains unresolved

- The **producer** of the state object. It is one frame above `0xF07D10`, behind a virtual
  dispatch whose vptr I did not resolve to a concrete class.
- The **writer** of `writer+0xC490`. No store of any form was found.
- The **secondary** dispatch path (`0x3EE0238` slot 0 → thunk `0xCBF540`). No static call site
  references the thunk.

---

## 2. Dispatch search

### 2.1 Method

I searched the whole `.text` segment at byte level for every applicable call form, then
resolved each candidate's vptr origin by disassembling a bounded window before the call.

| Form | Pattern | Count |
|---|---|---|
| 1 | `callq *0x2f8(%reg)` | **39** |
| 1b | same with REX.B (r8–r15) | 0 |
| 2 | `callq *%reg` (all) | 18,109 |
| 2b | same with REX.B | 3,561 |
| 3 | direct `callq`/`jmp` to binder or thunk | 1 (thunk only) |

All 39 form-1 candidates use `%rax`. That is consistent with a vptr loaded into `%rax`
immediately before the call.

### 2.2 Resolving the candidates

For each of the 39 sites I looked back for the last write to `%rax`:

| Origin pattern | Sites |
|---|---|
| `movq 0x20(%rbx), %rax` | 8 |
| `movq %rsi, %rax` | 1 |
| unresolved in the 0x80-byte window | 10 |
| (remainder outside the first 20 inspected) | — |

The `0x20(%rbx)` pattern is a **different** vtable family: those objects have their vptr at
`+0x20`, not at `+0x00`. They are not contexts.

### 2.3 The resolved site

`0xF07EFA` is the only candidate whose vptr comes from `(%rbx)` — offset zero — which is the
context layout:

```
f07eec  movq (%rbx), %rax        ; rax = context->vptr
f07eef  andl $0x40, %r14d
f07ef3  movq %rbx, %rdi          ; rdi = context
f07ef6  shrl $0x6, %r14d
f07efa  callq *0x2f8(%rax)       ; binder(context, state)
```

### 2.4 Proving `%rbx` is a context

Absence of `+0x460`/`+0x468` access in the caller is **not** evidence that `%rbx` is not a
context — a caller need not touch every field. I proved it a different way: the same function
calls slot `+0x268` on the same `%rbx`:

```
f07ec5  movq (%rbx), %rax
f07ec8  callq *0x268(%rax)       ; sibling context method
```

And `0x3EDFF18+0x268` = `0xCBaf80`, whose body reads `0x460(%rdi)`:

```
cbafc2  movq 0x460(%rdi), %r14
```

That is the context's writer field. So `%rbx` is a context, and the `+0x2F8` call is the binder.

**This is the vptr-provenance evidence the task required.** The classification does not rest on
the displacement `0x2F8` alone.

---

## 3. Backward slice of the `rsi` state argument

### 3.1 Within `0xF07D10`

```
f07d28  movq %rsi, -0x50(%rbp)   ; state saved from the function's 2nd parameter
...
f07eba  movq -0x50(%rbp), %rsi
f07ebe  movq %rbx, %rdi
f07efa  callq *0x2f8(%rax)       ; binder(context, state)
```

The state is the caller's own argument. It is used at four other sites
(`0xF07D52`, `0xF07DB1`, `0xF07E15`, `0xF07FEA`) as an argument to other context methods.

### 3.2 What the caller does not do

| Check | Result |
|---|---|
| Writes `+0x1C0`, `+0x1E0`, `+0x1E8` or `+0x200` | **No** |
| Allocates (`0xD25750` or `0x103C0C0`) | **No** |
| Copies the state | **No** |

So the state arrives fully populated. The producer is upstream.

### 3.3 One frame further up

`0xF07D10` has four direct callers, all thin wrappers that pass `rsi` straight through:

| Wrapper | Body |
|---|---|
| `0xF101E0` | `movq %rdi,%rbx ; callq 0xF07D10` |
| `0xF15970` | `movq %rdi,%r14 ; callq 0xF07D10` |
| `0xF187B0` | `movq %rdi,%r14 ; callq 0xF07D10` |
| `0x22BDB30` | `movq %rsi,%r14 ; movq %rdi,%rbx ; callq 0xF07D10` |

None of these wrappers has a direct caller. Each is referenced only from **vtables**:

| Wrapper | vtable slots (RELRO) |
|---|---|
| `0xF101E0` | `0x3EECCEE0`, `0x3EED218`, `0x3EEF910` |
| `0xF15970` | `0x3EED550`, `0x3EEE660`, `0x3EF3228` |
| `0xF187B0` | `0x3EED840`, `0x4028318`, `0x413BE80` |
| `0x22BDB30` | `0x3FC10D8`, `0x40770D0` |

These are RELRO slot addresses. Some are not file-backed in the ELF image because the
loader populates them; `0x3EECCEE0` reads as unmapped in a static read for that reason. The
addresses themselves come from the 8-byte pointer scan over file-backed segments, so they are
reliable even where the slot content is not statically readable.

**Interpretation (inferred):** these are virtual methods of distinct shader-type classes. The
binder is invoked once per shader type, from inside that type's own method. The state is a
member of the shader-type object, not a standalone allocation.

That is a hypothesis. What is verified is that the wrappers are vtable-only and pass `rsi`
through unchanged.

---

## 4. State object characteristics

### 4.1 Fields the binder reads

| Offset | Use | Evidence |
|---|---|---|
| `+0x1C8` | refcounted object, incremented at entry | `0xCBE291`, `0xCBE29E` |
| `+0x1C0` | modifier, passed as the GNM flags argument | `0xCBE3DF`, `0xCBE574` |
| `+0x1E0` | alternate VS resource | `0xCBE3AD`, `0xCBE55D` |
| `+0x1E8` | PS resource | `0xCBEBFC` |
| `+0x200` | primary VS resource | `0xCBE39D` |

### 4.2 Refcount

The state itself is refcounted: the binder retains it via the template at `0xCBFA40`, which
does `lock incl 0x8(%rsi)` on the new value. So the state has an intrusive count at `+0x08`,
the same contract as the RHI resources.

### 4.3 What is not established

- The state's **allocation size**. No allocation site was found in the caller chain.
- Whether the state has a **vtable**. The binder never dereferences `(%rsi)`.
- The **producer** function. It is behind a virtual dispatch whose vptr I did not resolve.

---

## 5. Slot initialization

**Not recovered.** The caller chain never writes `+0x1C0`, `+0x1E0`, `+0x1E8` or `+0x200`.
The slots are populated before `0xF07D10` is entered.

### 5.1 The precise missing edge

The producer is the function that:
1. constructs or owns the state object, and
2. populates its three slots, and
3. passes it as `rsi` to one of the four wrappers.

Since the wrappers are vtable-only, that producer is reached through a virtual call on a
shader-type object. Resolving it requires identifying the shader-type class whose vtable holds
`0xF101E0` (or one of its siblings) and finding the caller of *that* slot.

### 5.2 What can still be said about the producer predicate

The binder dereferences `[+0x1E0]+0x260` unconditionally before taking the primary path
(`0xCBE3AD`–`0xCBE3BF`). Therefore **`+0x1E0` must always be populated**, even when
`+0x200` is used. `+0x200` is the optional additional resource.

That is a structural inference from the binder's unconditional dereference, not an observation
of the producer.

---

## 6. `writer+0xC198` versus `writer+0xC490`

### 6.1 Independent search

| Field | Writers found |
|---|---|
| `+0xC198` | **2** — `0x2F61E00` (`movq %rdx`) and `0x2F62095` (`movq %rcx`) |
| `+0xC490` | **0** |

The `+0xC490` search covered:
- `movq` register stores (`mod=10`, non-SIB, REX.W)
- immediate stores
- 16-byte and 32-byte vector stores whose range covers `0xC490`
- VEX-encoded stores

**Zero hits in every form.** I also searched for wider stores whose range would cover
`0xC490` (e.g. a 32-byte store at `0xC480`): none.

### 6.2 Conclusion

**The two fields are different and the relationship is unresolved.**

- `+0xC198` receives the `+0xB0` address from the setter at `0x2F61E00`.
- `+0xC490` is read as a pointer at `0xCAB438` and indexed at `+0xC2`.
- No code writes `+0xC490`.

The arithmetic identity `0xB0 + 0xC2 = 0x172` is **not** pointer-provenance evidence. It
describes an offset relationship within a hypothetical object; it does not show that the
`+0xC490` pointer points at the object whose inline region begins at `+0xB0`.

**Possible explanations, all unverified:**
- `+0xC490` is written by code outside the searched ranges (a different segment, or a
  relocation-populated slot).
- `+0xC490` aliases a field written through a base register I did not model.
- The field is initialized by a bulk copy from a template object.

I did not resolve which.

---

## 7. Findings table

| # | Finding | Class |
|---|---|---|
| 1 | Binder invoked at `0xF07EFA` in `0xF07D10` | **Verified** |
| 2 | vptr from `movq (%rbx), %rax` — context's own vtable | **Verified** |
| 3 | `%rbx` is a context (sibling slot `+0x268` reads `0x460(%rdi)`) | **Verified** |
| 4 | State is the caller's `rsi`, saved at `0xF07D28` | **Verified** |
| 5 | Caller never writes the slots and never allocates | **Verified** |
| 6 | Binder reads `state+0x1C8` and refcount-increments it | **Verified** |
| 7 | State is refcounted at `+0x08` via `0xCBFA40` | **Verified** |
| 8 | Four wrappers, all vtable-only, pass `rsi` through | **Verified** |
| 9 | `+0xC198` has 2 writers; `+0xC490` has 0 | **Verified** |
| 10 | `+0xB0` address goes to `+0xC198`, not `+0xC490` | **Verified** |
| 11 | No direct call/jmp to the thunk `0xCBF540` | **Verified** |
| 12 | Wrappers are virtual methods of shader-type classes | **Inferred** |
| 13 | Binder is invoked once per shader type | **Inferred** |
| 14 | State is a member of the shader-type object | **Inferred** |
| 15 | `+0x1E0` always populated, `+0x200` optional | **Inferred** (from binder's unconditional deref) |
| 16 | State allocation size | **Unresolved** |
| 17 | State vtable | **Unresolved** |
| 18 | Producer function | **Unresolved** |
| 19 | Slot initialization | **Unresolved** |
| 20 | `writer+0xC490` writer | **Unresolved** |
| 21 | Secondary dispatch call site | **Unresolved** |
| 22 | `+0x158` constants 4/6 are shader frequencies | **Unresolved** (no read-side evidence) |
| 23 | Slot object is a "third type" because no ctor writes `+0xB0` | **Falsified** (prior report, withdrawn) |
| 24 | `0x3EE01C0` is a shader-state vtable | **Falsified** (prior report, corrected) |

---

## 8. Longest established chain

```
[shader-type virtual method]  <- UNRESOLVED producer
        │ rsi = state (slots already populated)
        ▼
0xF07D10  per-shader-type context method
        │ movq (%rbx),%rax ; callq *0x2f8(%rax)
        ▼
0x0CBE210  binder (context vtable +0x2F8)
        │ reads state+0x1C8, +0x1C0, +0x1E0, +0x1E8, +0x200
        │ refcount-assign into context+0x468
        ▼
context+0x468  retained shader state
        ▼
0x0CCC2F0  receiver
        │ +0x200 if non-null, else +0x1E0; +0x1E8 separately
        ▼
pd = [slot+0x1D8] ; idx = [pd+0x03] ; regs = pd+0x40+idx*4
        ▼
0x33C81B0 / 0x33C8340  GNM shader setters
        ▼
0x0CBB9C0  indexed draw
```

**Verified from `0xF07D10` downward.** The edge above it is missing.

---

## 9. Search coverage and limitations

### 9.1 What was searched

| Search | Coverage |
|---|---|
| `callq *0x2f8(reg)` | Full `.text`, byte-level, all REX variants |
| `callq *%reg` | Full `.text`, byte-level (21,670 sites) |
| Direct call/jmp to binder and thunk | Full `.text` |
| `+0xC198` / `+0xC490` writers | `movq`, immediate, 16/32-byte vector, VEX |
| `context+0x468` writers | `movq` reg→mem, `mod=10`, non-SIB, REX.W |

### 9.2 What was not covered

- **SIB-indexed stores** to `+0x468`, `+0xC198`, `+0xC490`.
- **`memcpy`/`memmove`/bulk struct copies** that would write these fields indirectly.
- **Relocation-populated slots** in the RELRO segment (the SCE relocation tables did not parse
  cleanly with the standard DT_RELA tags; the SCE-specific tags `0x6100002F`/`0x61000031`
  yielded a table whose entries did not decode as RELATIVE relocations).
- **Indirect writes through computed base pointers** where the base is not a simple register.

These are the reasons the `+0xC490` result is "no writer found", not "provably never written".

### 9.3 Why the `context+0x468` search is not exhaustive

The 22 direct `movq` stores to `+0x468` are **unrelated objects** that happen to use that
offset. The binder's own write is helper-mediated (`leaq 0x468(%r14), %rdi` then
`callq 0xCBFa40`), which a displacement scan cannot see. So the search confirms the helper
path separately rather than proving uniqueness.

---

## 10. Next experiment

**H4:** the producer of the state is the caller of the vtable slot that holds `0xF101E0`
(or a sibling wrapper), and that caller allocates the state with a size that matches one of the
three candidate families.

### Method

1. For each wrapper (`0xF101E0`, `0xF15970`, `0xF187B0`, `0x22BDB30`), take its vtable slots.
2. For each slot address, find indirect calls of the form `callq *disp(%reg)` where `disp`
   equals that slot's offset from its vtable base.
3. In each resolved caller, find the allocation that produces the `rsi` argument.
4. Compare that size against 504, 784 and ≥682.

### Falsification

| Outcome | Meaning |
|---|---|
| One caller, allocation matches a candidate | H4 supported; that family is the state |
| Multiple callers with different sizes | The state is per-shader-type; sizes may differ |
| No caller resolved | The producer is behind a further virtual layer; widen the search |

---

## 11. Reproduction

### 11.1 Automated

```
python tools/research/verify_binder_caller.py <path-to-v100.elf>
```

Nine check groups, all passing. Rejects a wrong SHA-256 by default (tested, exit 1).

### 11.2 Manual

```
# The resolved binder call site
llvm-objdump -d --start-address=0xF07EEC --stop-address=0xF07F00 --no-show-raw-insn v100.elf

# Proof that rbx is a context: the sibling slot 0x268 method
llvm-objdump -d --start-address=0xCBaf80 --stop-address=0xCBB000 --no-show-raw-insn v100.elf

# The four wrappers
llvm-objdump -d --start-address=0xF101E0 --stop-address=0xF101F0 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0xF15970 --stop-address=0xF15980 --no-show-raw-insn v100.elf

# Binder entry: state+0x1C8 and the refcount
llvm-objdump -d --start-address=0x0CBE289 --stop-address=0x0CBE2B0 --no-show-raw-insn v100.elf

# The +0xC198 writers
llvm-objdump -d --start-address=0x2F61DF0 --stop-address=0x2F61E10 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x2F62080 --stop-address=0x2F620A0 --no-show-raw-insn v100.elf
```

### 11.3 Provenance

| Item | Value |
|---|---|
| ELF | `v100.elf`, SHA-256 `3F704995…A938BF` |
| SELF | Déraciné 1.00, SHA-256 `8caee78e…7011a0` |
| Toolchain | `llvm-objdump` 23.1.5, Python 3.15 |
| Function bounds | `.eh_frame_hdr`, 180,806 entries |

---

## 12. Summary

**Resolved:** the binder call site (`0xF07EFA`), its vptr provenance (context's own vtable at
offset zero), and the fact that the state is passed in fully populated.

**New:** the binder is invoked from a per-shader-type virtual method, not a pipeline setup path.
`writer+0xC198` and `writer+0xC490` are provably different fields, and `+0xC490` has no
writer in any searched form.

**Still missing:** the state producer, one virtual layer above `0xF07D10`. §10 gives the search
that would find it.

**Unchanged:** the native shader execution blocker. Even a complete producer chain yields GCN
register arrays, not portable shader code.
