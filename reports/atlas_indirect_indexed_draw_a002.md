# A-002 — Independently verified indexed-indirect RHI vtable member

**Status:** NEW ORIGINAL-TARGET FINDING (static only). **No runtime reachability claim.**

## Identity and evidence provenance

Target is Déraciné 1.00, original 81,506,767-byte PS4 SELF, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. The direct user attachment was hashed independently before analysis. The DSH ZIP also exactly matches SHA-256 `336d0746d64915c988fa985a92ce2eb8ea1ecf328034385c8d055766a93c984c`; its 33 members passed ZIP CRC verification. Neither proprietary file is in Git.

Repository baseline checked: `main` at `e2f981c40cba7c8490177c20c179cb3608cc1442`. Existing A2 work on branch `research/ue4-internal-rhi-a2-20261121` had already **enumerated** the slot and target, without analyzing this member's indexed-indirect command semantics. The earlier A/RHI transcript also contained an indexed-indirect GNM import call candidate at this address. These precedents are preserved, **not represented as fresh discoveries**. The new finding is the independently decoded **connection** of the vtable member to the indirect draw call and its argument/state contract, with original SELF bytes and relocation/FDE validation.

### Distinct coordinate systems (not interchangeable)

| Object | Inner ELF VA | ELF file offset | Original SELF container offset |
|---|---:|---:|---:|
| Vtable base | `0x03EDFCA0` | `0x03EE3CA0` | `0x03F00EE0` |
| **New vtable slot (+0x4F8)** | **`0x03EE0198`** | **`0x03EE4198`** | **`0x03F013D8`** |
| **Indexed-indirect method start** | **`0x0CBBEF0`** | **`0x0CBFEF0`** | **`0x0CDBC20`** |
| **DrawIndexIndirect call site** | **`0x0CBC359`** | **`0x0CC0359`** | **`0x0CDC089`** |
| GNM PLT import stub | `0x033C8390` | `0x033CC390` | `0x033E80C0` |
| GNM GOT / JUMP_SLOT | `0x04381DA8` | `0x04385DA8` | `0x043A2FE8` |

The ELF mapping comes from embedded ELF program headers and matching PS4 SELF **segment IDs**, not an assumed constant SELF-to-ELF delta across all segments. All shown VAs are *inner ELF*, not loaded runtime VAs. No GPU virtual address, shader hash, runtime method invocation, or runtime pointer identity is asserted.

## Independently verified binary facts

1. `PT_SCE_RELRO` table at `0x03EDFCA0` has the **already known** indexed-draw slot `+0x4F0` (`0x03EE0190 -> 0x0CBB9C0`). Its immediately following slot `+0x4F8` (`0x03EE0198`) stores `0x0CBBEF0`. Full `DT_SCE_RELA` enumeration confirms `R_X86_64_RELATIVE` at `r_offset=0x03EE0198`, symbol index 0, addend `0x0CBBEF0` (RELA index 37982).
2. `PT_GNU_EH_FRAME` at `0x03CFC414` supplies an exact FDE record at `0x038F7A50`, covering **`[0x0CBBEF0, 0x0CBC4D7)`** (length `0x5E7`). This is the decoded unwind range, **not** the next-function-start `0x0CBC4E0`. The 1,511-byte method-body SHA-256 is `53fbfe5da0dbe6a7e6bf85e37a0062fbbfb1557247c7506bcd3a6e74c5344bc5`.
3. Inside this FDE, instruction-boundary-decoded `0x0CBC359` is `call rel32 0x033C8390`. The `0x033C8390` PLT stub is a RIP-relative `FF 25` jmp via `0x04381DA8`. `DT_SCE_JMPREL` record 451 is type `R_X86_64_JUMP_SLOT` for that GOT slot and the dynamic symbol `ED9-Fjr8Ta4#A#B` (raw NID `ED9-Fjr8Ta4`). The repository's `sceGnmDrawIndexIndirect` HLE signature and independent published NID mapping identify the API; source-level API names are not recovered private UE4 C++ symbols.
4. The decoded call argument setup at `0x0CBC32F–0x0CBC35D` establishes `RDI=[R12]` (command-buffer write cursor), `ESI=9`, `EDX=[RBP-0x34]` (saved input), `ECX=6`, `R8D` and `R9D` from the low/high nibble of `EBX`, and a seventh stack argument `([R14+0x28]&1)`. The same 9-DWORD size is reflected in the post-call `[R12] += 0x24` **on zero return**, at `0x0CBC362`. This fits the GNM imported contract `(cmdbuf, size, data_offset, shader_stage, vertex_sgpr_offset, instance_sgpr_offset, flags)`; the specific semantic identity of the guest pointers/arguments still needs upstream call-path analysis.
5. The method accesses a command-stream/writer pointer at `[RDI+0x460]` near `0x0CBBF01`. At `0x0CBC367` it tests `[R13+0x4D0]`; `0x0CBC371` accesses `[R13+0x460]`; `0x0CBC3B5` writes `1` to `[R13+0x4D0]`. The writer object contains cursor/limit-like uses at `+0x08/+0x10` and invokes a refill callback through `[writer+0x18]`. Here `R13` was initialized from the **fourth method argument RCX**, so these are **not** automatically proven offsets on the same object as initial `RDI`. The code also writes packet-like constants through the stream; it cannot be reduced to a simple API forwarding stub.

**Result:** High confidence this table member is an **indexed-indirect graphics draw emitter** above the GNM import. The table is consistent with a PS4 UE4 RHI graphics context. Its exact private C++ virtual method name, complete argument types, owner identity, and runtime frequency are **not** proved. This does not by itself provide a reusable complete Vulkan renderer interface or prove that no direct-PM4 variants exist.

## Alternative interpretations and limits

- **Likely UE4 role:** a platform implementation of an indexed-indirect RHI draw variant (potentially a `RHIDrawIndexedIndirect`-style operation). The original stripped executable does not establish a definitive C++ name or class; the method may include PS4-specific wrappers and inline packet logic.
- **Multiple related entries:** neighboring `+0x4E8 -> 0x0CBB410` also calls the same imported NID at `0x0CBB841`. Separate interfaces/variants are plausible; their precise parameter and ownership differences are **unresolved**. This report verifies **only +0x4F8** as the new method attribution.
- **Runtime:** A3/B3's 128 paired draws concern **the different indexed-draw method at +0x4F0**, not proof that this indirect path executes. No guest HMD, GPU, or emulator was run here.
- **Object layout:** repeated `+0x460/+0x4D0` uses and stream cursor/refill pattern are concrete but **do not** establish object allocation size, owning C++ type, or general resource lifetime.

## Reproduction and comparison evidence

- `python tools/atlas/verify_indexed_indirect.py <local-1.00-eboot.bin>` performs read-only source SHA verification, SELF/ELF segment mapping, actual RELATIVE/JUMP_SLOT enumeration, exact unwind-FDE decoding, selected instruction-boundary checks, import-NID linkage, and method-body hash. No game bytes are extracted or committed.
- Original source references: A2 report blob `1d403c7934712d6958dd747cc79c5581364436bc`; A2 candidate map blob `30551f900e52a1b685592dad2232e59e24f91cf8`; B3 address-map blob `967512ebf080ebfd847709ddd3346d1652f5ee66`. This avoids presenting previous draw confirmation as new.
- Sanitized transcript provenance: A/RHI agent event 237 (seq 235) previously listed the indirect GNM call candidate; A2 agent events 292 (seq 290), 936 (seq 934), 944 (seq 942), 1019 (seq 1017) enumerated/checked table pointer `0x03EE0198`; no independently decoded +0x4F8-to-GNM semantic contract was promoted in the published A2 report. Raw JSONL streams are untrusted and remain private.
- The project's HLE signature and nine-DWORD command behavior are at [`gnmdriver.cpp`, baseline `e2f981c4`](https://github.com/s-ilent/DeracinePCVR/blob/e2f981c40cba7c8490177c20c179cb3608cc1442/src/src/core/libraries/gnmdriver/gnmdriver.cpp#L494-L519); this **documents emulator behavior**, not a second copy of the guest. An external [PS4 GNM NID listing](https://github.com/boykopovar/AnyPS5/issues/17581) independently maps `ED9-Fjr8Ta4` to `sceGnmDrawIndexIndirect`.

## Next target

Recover the caller-side UE4 `FRHICommandList` / dispatch path that invokes `+0x4F8`, establish how `RDI` and the fourth argument `RCX -> R13` are constructed and owned, then map indirect arguments-buffer resource lifetime. Prefer a new verified producer/caller FDE over reiterating GNM import calls. Preserve separate address domains and avoid re-running A3/B3.
