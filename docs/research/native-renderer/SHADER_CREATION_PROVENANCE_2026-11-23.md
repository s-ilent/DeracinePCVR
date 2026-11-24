# Shader Creation Provenance — Recovery Attempt

**Date:** 2026-11-23
**Branch:** `research/native-shader-creation-provenance-20261123`
**Base:** `50f13a0e` (`research/native-shader-resource-correction-20261123`)
**Status:** Static analysis only. No runtime hook, no capture, no native GPU prototype.
**Target:** Déraciné 1.00 inner ELF `v100.elf`, SHA-256 `3F704995…A938BF`
**Companion:** `OBSERVER_SPEC_CORRECTION_2026-11-23.md` (Phase A)

---

## 1. Executive verdict

**The upstream UE4 shader-creation interface was NOT recovered.** Two candidate object
families were found and characterised, but neither can be shown to be the object that occupies
the shader-state slots. The creation-to-binding chain is **broken at one specific edge**, and
this report identifies that edge precisely rather than closing it by resemblance.

### 1.1 What was established

| Finding | Confidence |
|---|---|
| Binder and receiver predicates are independent and asymmetric | **Verified** |
| Two distinct shader-family vtables with distinct constructors | **Verified** |
| Family A object: 784-byte allocation, 769-byte write extent, vtable `0x3F102D8` | **Verified** |
| Family B object: 682-byte write extent, vtable `0x3F11580` | **Verified** |
| Family A ctor writes stage-like constant `4` at `+0x158`; family B writes `6` | **Verified** |
| Both families copy a refcounted program-data pointer into `+0x1D8` from a parameter struct | **Verified** |
| Program-data layout (`+0x03` idx, `+0x24`..`+0x34` sizes, `+0x40` regs, `+0x5E` flags) | **Verified by reads** |
| `ShaderCore` / `ShaderCompiler` source-path strings exist in the binary | **Verified** |
| Those strings have zero code references | **Verified** |

### 1.2 What was NOT established

| Question | Status |
|---|---|
| Which object type occupies `shader_state+0x1E0/0x1E8/0x200` | **Unknown** |
| Whether `+0x158` is a shader frequency | **Unresolved** — written, never read in these families |
| Whether family A/B are ShaderCore `FShaderResource` equivalents | **Unresolved** |
| The UE4 output hash | **Not found** |
| The cooked shader storage mechanism | **Not determined** |
| The creation→slot edge | **Missing** |

### 1.3 The honest headline

I did not find the shader creation path. I found two large object families that carry
program-data pointers, and I found that **neither of them is the object the binder and receiver
actually consume**. That negative result is the main deliverable, because it prevents building
the native renderer around the wrong object.

---

## 2. Phase A — observer correction (summary)

Full detail in `OBSERVER_SPEC_CORRECTION_2026-11-23.md`. The essential correction:

| Aspect | Binder `0x0CBE210` | Receiver `0x0CCC2F0` |
|---|---|---|
| Null-tests `+0x200` | Yes | Yes |
| Null-tests `+0x1E0` itself | No | No |
| Requires `[+0x1E0]+0x260 != NULL` | **Yes** | **No** |
| Tests bit 1 of `pd+0x5E+idx*4` | **Yes** | No |

The previous acceptance criterion required `+0x200` to be non-null. SC-02 measured 156/156
draws using `+0x1E0` with `+0x200` null. **That criterion is withdrawn.**

Verifier: 10 check groups, all passing. Rejects a wrong SHA-256 by default (tested, exit 1).
Raises `DisasmError` instead of silently passing on empty disassembly.

---

## 3. Phase B — the two shader-family candidates

### 3.1 Discovery path

Starting from the program-data attach sites found in the previous pass (`0x1209D63`,
`0x12244F4`), I traced the enclosing functions to their vtables:

| Family | vtable | ctor | attach fn | destructor |
|---|---|---|---|---|
| A | `0x3F102D8` | `0x1209580` | `0x12098E0` | `0x1223AE0` |
| B | `0x3F11580` | `0x11E4680` | `0x1223D70` | `0x122EA70` |

Both vtables have **identical slot layouts** (18 populated slots, same positions). That is
consistent with two subclasses of one base, or two instantiations of one template.

### 3.2 Object extents

| Family | allocation | max write offset | implied size |
|---|---|---|---|
| A | `0x310` = 784 bytes (`0x1209B3C`) | `+0x300` (769 bytes) | 784 |
| B | not located | `+0x2A8` (682 bytes) | ≥682 |

For comparison, the RHI shader instance from the corrected report is **504 bytes (`0x1F8`)**.
These families are substantially larger, so they are a different layer.

### 3.3 The stage-like constant

Both constructors write a small constant at `+0x158`:

```
120965c  movl $0x4, 0x158(%rbx)     ; family A
11e4b69  movl $0x6, 0x158(%r12)     ; family B
```

**I am not calling these shader frequencies.** Three reasons:

1. No vtable method of either family reads `+0x158`. I scanned all 18 methods of both
   vtables; zero reads.
2. `+0x158` is a generic offset elsewhere. At `0xFA0390` it is element 4 of a stride-`0x50`
   array (`0x18, 0x68, 0xB8, 0x108, 0x158`), compared against `0x184`. That is a different
   object type entirely.
3. A written-but-never-read field is not evidence of semantics.

The constants `4` and `6` are **recorded as observations**, not interpreted.

### 3.4 Program-data attachment

Both families copy a refcounted pointer into `+0x1D8` from a **parameter struct**:

```
; family A ctor, r14 = parameter struct
1209760  movq (%r14), %rcx          ; param+0x00
120976a  movq %rcx, 0x1d0(%rbx)
1209771  movq 0x8(%r14), %rcx       ; param+0x08
1209778  movq %rcx, 0x1d8(%rbx)     ; program data
1209781  lock incl 0x8(%rcx)        ; refcount++
1209785  movl 0x10(%r14), %ecx      ; param+0x10
1209794  movl %ecx, 0x1e0(%rbx)
```

So the program data is **passed in**, not created here. The creation of the program-data object
happens upstream of this constructor, in whatever builds the parameter struct.

### 3.5 The attach function copies from a source object

`0x12098E0` (family A attach) reads `r15+0x1D8` and `r15+0x1D0` and stores them into a
**newly allocated 784-byte object**:

```
1209ade  movq 0x1d0(%r15), %rax     ; source object's +0x1D0
1209ae5  movq %rax, -0xf0(%rbp)
1209aec  movq 0x1d8(%r15), %rax     ; source object's +0x1D8
1209af6  movq %rax, -0xe8(%rbp)
1209b3c  movl $0x310, %edi          ; allocate 784 bytes
1209b41  callq 0xd25750
...
1209d52  movq %rax, 0x1d0(%r13)     ; into the new object
1209d63  movq %rax, 0x1d8(%r13)
```

This is a **copy/clone** operation between two objects of the same family, not a creation from
cooked data.

---

## 4. The missing edge

### 4.1 The contradiction

The binder and receiver consume slot objects that must have **both** `+0xB0` (shader map base)
and `+0x1D8` (program data):

```
cbe441  leaq 0xb0(%rbx), %rdx       ; map base
cbe448  movq 0x1d8(%rbx), %rsi      ; program data
cbe44f  callq 0x2f61d50
```

But:

| Object | has `+0xB0` written | has `+0x1D8` written |
|---|---|---|
| Family A (784 B) | **No** | Yes |
| Family B (682 B) | **No** | Yes |
| RHI instance (504 B) | **No** | Yes |

**No recovered object type has both fields written.** The slot object is a **third type** that
I did not locate.

### 4.2 Why this matters

The previous report's chain `context+0x468 → +0x200 → +0x1D8 → +0x40+idx*4` is verified as a
*read* chain. What was never established is *which constructor produces the object at
`+0x200`*. That is the missing edge, and it is the edge a native renderer needs.

### 4.3 What `+0xB0` being generic means

`+0xB0` has 169 `movq` store sites in the RHI region across dozens of unrelated functions.
It is a common offset, not a shader-specific field. So "has `+0xB0`" is not a usable type
discriminator on its own. The slot object must be identified by its **vtable**, not by offsets.

---

## 5. Shader identity — classified individually

| Candidate | Status | Evidence |
|---|---|---|
| Shader stage / frequency | **Unknown** | `+0x158` constants 4/6 exist but are never read; not proven to be frequencies |
| UE4 output hash | **Not found** | No 32/64-bit hash field identified with a hashing origin |
| Material shader-map identity | **Unknown** | `+0xB0` map base exists on the slot object, but the object is unlocated |
| Shader type / permutation | **Unknown** | No permutation key found |
| Vertex-factory association | **Not found** | — |
| Program / code-object identity | **Partial** | Program data is a refcounted object at `+0x1D8`; its internal identity fields are `+0x03` (idx), `+0x24`..`+0x34` (sizes), `+0x5E` (flags) — all platform-shaped, none engine-hash-shaped |
| Shader-library lookup key | **Not found** | — |

**Explicit caution:** the program-data fields `+0x24`..`+0x34` are read by the receiver as
sizes (`0xCCD159`–`0xCCD172` sums them). They are PS4 program-block metadata, not a UE4
output hash. I am not treating any of them as a shader identity.

---

## 6. Cooked shader storage — not determined

### 6.1 What was checked

| Mechanism | Evidence found |
|---|---|
| Inline cooked code in assets | No package deserialization path identified |
| Shared `ShaderArchive-*.ushaderbytecode` | **Zero** string occurrences |
| Native PS4 shader archive | No `Orbis` / `sce_shader` strings |
| Title-specific | Possible; not identified |

### 6.2 The `ShaderCore` strings are dead

The binary contains:

```
0x35d387c  "ShaderCore\Private\Shader.cpp"
0x35d38d0  "ShaderCore\Private\ShaderParameters.cpp"
0x35d3923  "ShaderCore\Private\ShaderCore.cpp"
0x35e9119  "ShaderCompiler\ShaderCompiler.cpp"
```

These are **assertion file-path strings**. I scanned for both RIP-relative and absolute
references to each: **zero of either kind**. They are unreferenced leftovers from stripped
assertion macros.

**This is a weak negative, not proof of absence.** Stripping, compile-time folding, or a
different assertion mechanism could hide the references. But it means these strings cannot be
used as evidence that the ShaderCore layer is live.

### 6.3 Blocker

Determining the storage mechanism requires either:
- a private asset inventory (package files), or
- locating the file-open / deserialize path in the executable.

I did not locate the latter. This is recorded as **blocked**, not as "no library".

---

## 7. Comparison against public UE4.18.3

| Public UE4.18.3 fact | Déraciné status |
|---|---|
| `FShaderResource` owns compiled code, `FShaderTarget`, `OutputHash` | **Not recovered.** No object with a hash field was found. |
| `FRHIShader` inherits `FRHIResource` with intrusive refcount + deferred delete | **Consistent** with the 504-byte instance (verified in the prior pass) |
| `FShaderResource::InitRHI` dispatches by `Target.Frequency` | **Not recovered.** No frequency dispatch found. |
| `FShaderCodeLibrary` retrieves from inline/archive/native | **Not determined** (§6) |
| `OutputHash` = hash(compiled code + parameter map) | **Not found** |

The two families found in §3 are **larger** than the RHI shader and carry program-data
pointers, which is *consistent* with a ShaderCore-layer object. But consistency is not
identification, and I am not claiming they are `FShaderResource`.

---

## 8. Native renderer requirements

| Requirement | Status | Blocker |
|---|---|---|
| Shader caching | **Blocked** | No stable engine-side identity recovered |
| Stage binding | **Partial** | Slot→setter mapping is verified (PS/VS); the *created* object's stage is not |
| Shader translation | **Blocked** | Program data is GCN register arrays; no portable IR |
| Descriptor layout | **Partial** | Slot indirection (`+0xB0+0xC2`) verified; parameter map not |
| Vertex input | **Blocked** | Not associated with any recovered shader object |
| Shader lifetime | **Partial** | Refcount contract verified; creation site not |
| Draw association | **Partial** | Receiver selection verified; creation→slot edge missing |

### 8.1 The three concepts, kept separate

- **Shader identity** — which compiled shader the game selected. **Not recovered.**
- **Shader interface** — inputs, parameters, resources. **Partially recovered** (slot
  indirection, program-data sizes).
- **Shader execution** — turning PS4 instructions into PC GPU code. **Blocked**, unchanged.

Recovering identity would not solve the other two. This pass did not even recover identity.

---

## 9. Next experiment

**H2:** the object occupying `shader_state+0x200` is produced by a constructor whose vtable is
neither `0x3F102D8` nor `0x3F11580`, and that constructor writes both `+0xB0` and `+0x1D8`.

### Method (static, bounded)

1. Enumerate every vtable in the dynlibdata segment whose methods include a write to `+0x1D8`
   followed by a refcount increment.
2. For each, check whether the same constructor also writes `+0xB0`.
3. The intersection is the slot-object candidate set.
4. Verify by finding a store of that object into `shader_state+0x1E0`, `+0x1E8` or `+0x200`.

### Falsification

| Outcome | Meaning |
|---|---|
| Exactly one constructor writes both fields | H2 supported; that is the slot object |
| Zero constructors write both | The fields are written by separate functions; search must move to the slot-store sites |
| Multiple candidates | Discriminate by the slot-store site's expected vtable |

### Why this is the right next step

It targets the exact missing edge (§4.1) with a mechanical search, and it does not depend on
any assumption about UE4 class names or layouts.

---

## 10. Reproduction

### 10.1 Automated

```
python tools/research/verify_shader_resource_correction.py <path-to-v100.elf>
```

Ten check groups, all passing. Rejects a wrong binary by default.

### 10.2 Manual

```
# The two shader-family vtables
llvm-objdump -d --start-address=0x1209580 --stop-address=0x1209870 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x11E4680 --stop-address=0x11E53F0 --no-show-raw-insn v100.elf

# The stage-like constants
llvm-objdump -d --start-address=0x1209650 --stop-address=0x12096B0 --no-show-raw-insn v100.elf
llvm-objdump -d --start-address=0x11E4B60 --stop-address=0x11E4B80 --no-show-raw-insn v100.elf

# The program-data copy in the attach function
llvm-objdump -d --start-address=0x1209AD0 --stop-address=0x1209B50 --no-show-raw-insn v100.elf

# The binder's +0x260 predicate
llvm-objdump -d --start-address=0x0CBE399 --stop-address=0x0CBE3E0 --no-show-raw-insn v100.elf

# The receiver's unconditional +0x1E0 use
llvm-objdump -d --start-address=0x0CCD1AA --stop-address=0x0CCD210 --no-show-raw-insn v100.elf
```

### 10.3 Provenance

| Item | Value |
|---|---|
| ELF | `v100.elf`, SHA-256 `3F704995…A938BF` |
| SELF | Déraciné 1.00, SHA-256 `8caee78e…7011a0` |
| Toolchain | `llvm-objdump` 23.1.5, Python 3.15 |
| Function bounds | `.eh_frame_hdr`, 180,806 entries |
| Public reference | `aceyan/UE4.18.3_release` @ `4c9fc5d3` |

---

## 11. Summary

**Proven:** binder and receiver predicates are independent and asymmetric; two shader-family
object types exist with distinct vtables, distinct stage-like constants and program-data
pointers copied from parameter structs; the program-data layout is confirmed by reads.

**Not proven:** which object occupies the shader-state slots; whether the stage-like constants
are frequencies; the UE4 output hash; the cooked storage mechanism; the creation→slot edge.

**The blocker is one specific missing edge:** no recovered constructor writes both `+0xB0` and
`+0x1D8`, so the slot object is a third type that remains unlocated. §9 gives the mechanical
search that would find it.

**The native shader execution blocker is unchanged.** Even a complete creation-to-binding chain
would yield GCN register arrays, not portable shader code.
