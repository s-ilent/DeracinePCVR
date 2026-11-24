# Observer Specification — Binder/Receiver Predicate Correction

**Date:** 2026-11-23
**Branch:** `research/native-shader-creation-provenance-20261123`
**Corrects:** `SHADER_RESOURCE_LIFECYCLE_CORRECTION_2026-11-23.md` §5 (observer specification)
**Preserves:** the original report unchanged; this is a companion correction, not a rewrite.
**Target:** Déraciné 1.00 inner ELF `v100.elf`, SHA-256 `3F704995…A938BF`

---

## 1. Why this correction exists

The previous observer specification conflated two independent decisions:

- the **binder's** choice of which slot to process, and
- the **receiver's** choice of which slot a draw consumes.

It then proposed an acceptance criterion that required `shader_state+0x200` to be non-null.
SC-02 had already measured 156 of 156 draws resolving through `+0x1E0` with `+0x200` null.
The criterion would have failed on the first real draw.

This document separates the two decisions and withdraws the combined criterion.

---

## 2. Verified predicates

Both functions were re-disassembled against the pinned ELF. All claims below are byte-verified.

### 2.1 Receiver — `0x0CCC2F0`

```
ccd02d  movq 0x468(%r12), %rax     ; rax = shader_state
ccd035  testq %rax, %rax
ccd038  je    0xccd418             ; no state -> skip entirely

; --- PIXEL slot +0x1E8 ---
ccd03e  movq 0x1e8(%rax), %rax
ccd045  testq %rax, %rax
ccd048  je    0xccd08c             ; null -> emit PS with null array
ccd04a  movq 0x1d8(%rax), %r12     ; program data
ccd07e  addq $0x8, %r12            ; array = pd + 8
ccd0c0  callq 0x33c8340            ; sceGnmSetPsShader350(cmd, 0x28, array)

; --- VERTEX slot +0x200 (primary) ---
ccd0db  movq 0x200(%rax), %r13
ccd0e2  testq %r13, %r13
ccd0e5  je    0xccd1aa             ; null -> alternate
ccd0f6  movq 0x1d8(%r13), %r12
ccd103  movzbl 0x3(%r12), %ebx      ; register-array selector
ccd12c  leaq 0x40(%r12,%rbx,4), %rdx
ccd139  callq 0x33c81b0            ; sceGnmSetVsShader(cmd, 0x1D, regs, modifier)

; --- VERTEX slot +0x1E0 (alternate) ---
ccd1aa  movq 0x1e0(%rax), %rcx      ; NO null test
ccd1c5  movq 0x1d8(%rcx), %r12
ccd1f3  addq $0x8, %r12             ; array = pd + 8
ccd201  callq 0x33c81b0            ; sceGnmSetVsShader(cmd, 0x1D, pd+8, modifier)
```

**Receiver predicates, exactly:**

| Decision | Predicate |
|---|---|
| Process any shader at all | `state != NULL` |
| Pixel path | `state+0x1E8 != NULL` → use it; else emit PS with a null array |
| Vertex path | `state+0x200 != NULL` → use it; **else use `state+0x1E0` unconditionally** |

The receiver does **not** null-test `+0x1E0`. It dereferences `[+0x1E0]+0x1D8` directly.
If `+0x200` is null and `+0x1E0` is also null, the receiver faults. That is a real
precondition the engine must maintain, not a defensive branch.

### 2.2 Binder — `0x0CBE210`

```
cbe39d  movq 0x200(%r15), %rbx      ; r15 = new_state
cbe3a4  testq %rbx, %rbx
cbe3a7  je    0xcbe459              ; primary null -> alternate path
cbe3ad  movq 0x1e0(%r15), %r12      ; NO null test on +0x1E0 itself
cbe3b4  movq 0x260(%r12), %rcx      ; dereference +0x1E0 -> +0x260
cbe3bc  testq %rcx, %rcx
cbe3bf  je    0xcbe459              ; +0x260 null -> alternate path
cbe3c5  movq 0x1d8(%rbx), %rax
cbe3cc  movzbl 0x3(%rax), %edx
cbe3d0  testb $0x2, 0x5e(%rax,%rdx,4)
cbe3d5  jne   0xcbe64c              ; bit 1 set -> variant sub-path
```

**Binder predicates, exactly:**

| Decision | Predicate |
|---|---|
| Primary vertex path | `state+0x200 != NULL` **AND** `[state+0x1E0]+0x260 != NULL` |
| Variant sub-path | bit 1 of `[pd+0x5E + idx*4]` where `idx = pd+0x03` |
| Alternate vertex path | either of the above fails |
| Pixel path | `state+0x1E8 != NULL` (checked separately at `0xCBEBFC`) |

### 2.3 The asymmetry, stated precisely

| Aspect | Binder | Receiver |
|---|---|---|
| Null-tests `+0x200` | Yes | Yes |
| Null-tests `+0x1E0` itself | **No** | **No** |
| Requires `[+0x1E0]+0x260 != NULL` | **Yes** | **No** |
| Tests bit 1 of the per-array flag | **Yes** | **No** (receiver reads the byte but only for a size heuristic at `0xCCD14C`) |

**This is the correction.** The binder can decline the primary path for a reason the receiver
does not share. A state with `+0x200` non-null but `[+0x1E0]+0x260` null will make the binder
take its alternate path while the receiver still takes the primary path. The two functions can
legitimately disagree, and that disagreement is information, not an error.

---

## 3. Revised observer specification

**Not implemented. Not executed. Requires separate authorization.**

### 3.1 Two independent probes

**Probe A — binder, at `0x0CBE210` entry.** Records what the binder *received* and what it
*decided*.

| Field | Source |
|---|---|
| `seq` | shared monotonic counter |
| `thread_id` | `scePthreadGetthreadid` at the probe site |
| `context` | `rdi` |
| `new_state` | `rsi` |
| `prev_state` | `[rdi+0x468]` read before the assign |
| `slot_1e0`, `slot_1e8`, `slot_200` | `[rsi+0x1E0]`, `[rsi+0x1E8]`, `[rsi+0x200]` |
| `pd_1e0`, `pd_1e8`, `pd_200` | `[[slot]+0x1D8]`, only when the slot is non-null |
| `sub260_1e0` | `[[rsi+0x1E0]+0x260]`, only when `+0x1E0` is non-null |
| `modifier` | `[rsi+0x1C0]` |
| `binder_path` | derived: `primary` / `variant` / `alternate` / `no_state` |

**Probe B — receiver, at `0x0CCC2F0` after slot selection.** Records what a draw *consumed*.

| Field | Source |
|---|---|
| `seq` | same counter |
| `thread_id` | `scePthreadGetthreadid` |
| `context` | the receiver's `this` |
| `state` | `[context+0x468]` |
| `selected_vs_slot` | `0x200` or `0x1E0` — **the critical field** |
| `selected_vs_pd` | the `+0x1D8` actually used |
| `selected_ps_slot` | `0x1E8` or `none` |
| `selected_ps_pd` | the `+0x1D8` actually used |
| `vs_array_form` | `indexed` (`pd+0x40+idx*4`) or `fixed` (`pd+8`) |
| `modifier` | `[state+0x1C0]` |

### 3.2 Classification rules

Each receiver event is classified against the most recent preceding binder event on the same
`context`:

| Outcome | Condition |
|---|---|
| **Positive** | `selected_vs_pd` equals `pd_200` or `pd_1e0` of that binder event, **and** the slot choice matches the receiver predicate (`+0x200` non-null → primary) |
| **Explained divergence** | the slot choice differs from the binder's `binder_path`, **and** the difference is fully accounted for by the `+0x260` predicate. This is **not a failure**; it is the asymmetry in §2.3 being observed |
| **Negative** | `selected_vs_pd` matches neither `pd_200` nor `pd_1e0`, or the slot choice contradicts the receiver predicate while `+0x200` is non-null |
| **Inconclusive** | no preceding binder event on that context; or `state` changed between binder and receiver; or the receiver ran on a thread with no observed binder; or `+0x200` was null and `+0x1E0` was also null (the fault precondition) |

### 3.3 Constraints

1. Scalars and opaque identities only. No shader bytecode is read, hashed or exported.
2. No retained guest pointers. Every value is copied at the probe site.
3. Bounded ring buffer, compile-time capacity, no allocation in the probe.
4. Every slot read is null-guarded. A null slot records as null, never as zero.
5. **Populated ≠ selected.** Probe A records population; Probe B records selection. Separate fields.
6. Shared monotonic counter so binder and receiver events interleave correctly.
7. Thread identity from `scePthreadGetthreadid`, never inferred.
8. Default off.

### 3.4 Acceptance criterion

**Positive (H1 confirmed):** for at least 100 consecutive receiver events, `selected_vs_pd`
equals `pd_200` or `pd_1e0` of the most recent preceding binder event on the same context, and
every slot choice is either consistent with the receiver predicate or explained by the `+0x260`
predicate.

**Negative (H1 falsified):** any receiver event whose `selected_vs_pd` matches neither slot of
the preceding binder event, or whose slot choice contradicts the receiver predicate.

**Inconclusive:** any of the §3.2 inconclusive conditions.

### 3.5 What this replaces

The previous criterion — "match `program_data+0x06` against a PM4 field" — is **withdrawn**.
It tested one `u16` on a slot SC-02 showed is usually null, and it did not test selection at
all. The replacement tests a structural prediction about which resource a draw consumes.

---

## 4. Verifier changes

`tools/research/verify_shader_resource_correction.py` was strengthened:

| Change | Reason |
|---|---|
| Rejects an unexpected SHA-256 by default | Every address is specific to the pinned binary; a silent mismatch turns a wrong-binary run into a false pass |
| `--allow-hash-mismatch` override | Deliberate inspection of a different build stays possible |
| `DisasmError` on empty or failed disassembly | An empty result previously meant "no instructions matched"; it now means the tool failed or the range is unmapped |
| Branch-target extent assertions | Confirms each function's branches stay inside its own FDE extent |
| Explicit `+0x1E0` no-null-test assertion | Pins the asymmetry as a finding rather than leaving it implicit |

Ten check groups, all passing. Verified that a wrong binary is rejected with exit code 1.

---

## 5. Summary

**Corrected:** the binder and receiver predicates are now modelled separately. The combined
acceptance criterion is withdrawn.

**New finding:** the binder requires `[+0x1E0]+0x260 != NULL` before taking the primary vertex
path; the receiver does not. Neither function null-tests `+0x1E0` itself. Binder/receiver
divergence is therefore expected and must be classified as explained, not failed.

**Unchanged:** the three-slot model, the slot→setter mapping, and the program-data layout.
