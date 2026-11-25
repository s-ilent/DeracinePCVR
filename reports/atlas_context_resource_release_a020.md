# A-020 — Mode-2 adjacent command family: destructor-style resource release and graphics-state invalidation

**Static original-target finding; no runtime reachability claim.** Original Déraciné 1.00 PS4 SELF size 81,506,767, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed this run.

## Independently decoded evidence

A-019 left `0x0CACDA0` unattributed, noting that it was not the global mode-2 node's Execute. The new evidence establishes its own vtable ownership and a destructor-style lifetime/state path.

* **RELRO vtable address point** inner ELF VA `0x03EDFCA0` (original SELF offset `0x03F00EE0`) contains QWORD `0x0CACDA0` in slot +0; +8 contains `0x0CACE70` (trivial return). This is **not** the separate mode-2 global node vtable `0x03EDFC80`, whose +0 points to `0x0CACD70`. The former is the previously known graphics-context table base, so avoid conflating command-node and context-object vtables.
* **Method** `0x0CACDA0` (SELF offset `0x0CCACAD0`) is decoded from entry boundary, not an opcode scan. It takes `RDI` as an object and `RSI` as another object; `[RSI+0x20]` becomes state/context `R12`. It reads a nested resource `[[RDI+0x20]+0x10]` and dereferences its first pointer as `R15`, with null and `[R15+0x60]` checks. A fallback global pointer is read at ELF VA `0x04496A00` if the nested pointer is null/inactive.
* **Conditional state emission** at `0x0CACDF5–0x0CACE0A`: calls `0x02F62440` with `RDI=[R12+0x460]+0x2E8`, `ESI=[ [RDI_original+0x10]+0x380]`, `EDX=[RDI_original+0x18]`, `ECX=1`, `R8=resource+0x50`; only if the chosen resource has nonzero `+0x60`.
* **Cache and dirty-mask update** at `0x0CACE0F–0x0CACE30`: calls `0x0CAB2E0` with `RDI=R12+0x568+8*slot`, `RSI=R15`, then ORs `1 << slot` into `[R12+0x928]`. The slot is `[object+0x18]`; the shift is x86 CL-masked, so no unverified slot-count bound is inferred.
* **Reference/lifetime evidence** at `0x0CACE31–0x0CACE61`: decrements `[[object+0x20]+0x10]+0x08`; on zero invokes `0x0CACE80` with that record's first QWORD; then **writes the same `0x03EDFCA0` vptr to `[object]`**, and tail-jumps to `0x0CACE80` with `[object+0x28]`. This supports destructor-style cleanup and release semantics rather than a simple command execution wrapper. Exact C++ destructor kind and `0x0CACE80` allocation contract remain unresolved.

These are inner ELF VAs. For these code windows original SELF offset = ELF VA + `0x1FD30`; for this RELRO table = ELF VA + `0x21240`. No loaded guest address, PS4 GPU address, shader hash, or runtime execution is asserted.

## Interpretation and limits

**High confidence** for an original-target graphics-context-family vtable member that performs resource selection, conditional writer emission, slot cache invalidation and reference/lifetime release. **Medium confidence** for exact destructor attribution: vptr restoration plus decrement/release strongly suggest destruction, but the method may be a compiler-generated deleting-destructor variant or another cleanup wrapper. The `0x03EDFCA0` vtable address point also contains the previously established draw member at +0x4F0; this links the new resource-lifetime path to the same table family, not necessarily to a single source-level class or active graphics pipeline.

This resolves the A-019 warning about `0x0CACDA0`: it belongs to a **different** table than the mode-2 global command `0x03EDFC80`. The two context fields `command_list+0x20/+0x28` remain unproven concrete types.

## Reproduction

1. Verify the private original SELF SHA-256 above.
2. Read little-endian QWORDs at `0x03EDFCA0+0x21240` and `0x03EDFC80+0x21240`; compare their first two entries.
3. Extract private bytes at `0x0CACDA0+0x1FD30` through `0x0CACE65+0x1FD30`; disassemble x86-64 Intel with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=0xcacda0`.
4. Verify the direct calls at `0x0CACE0A -> 0x02F62440`, `0x0CACE1A -> 0x0CAB2E0`, `0x0CACE46 -> 0x0CACE80`, and tail-jump `0x0CACE61 -> 0x0CACE80`; inspect the conditional branches, refcount decrement and vptr store.

Next: identify the allocation/construction site for this vtable and the concrete ownership of `[object+0x20]` and `[object+0x28]`; independently trace the context-list fields without assuming graphics/compute or stereo routing.