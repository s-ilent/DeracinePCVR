# C-021 — Three typed resource-dispatch streams in the shader-context caller

**Original target:** Déraciné PS4 SELF 1.00, 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently rehashed. All addresses are inner ELF VAs. In verified windows, original SELF file offset = ELF VA + `0x1FD30`. Guest loaded addresses, relocation slots, GPU VAs, shader hashes and Vulkan bindings are not inferred.

## New finding

The original `0x0CC0190..0x0CC02D2` instruction-boundary-disassembled caller contains **three different resource-dispatch loops**, not a single generic list. Each extracts an index from packed 32-bit entries using `bextr eax,eax,edi` with `edi=0x1008`, i.e. bit start 8, length 16. The top byte is independently used as a group/termination comparison against `r13d`; the low byte supplies a separate argument. The index addresses `[r15+rax*8]`, where `r15` comes from a selected resource object `[rax+0x20]` at `0x0CC0150`.

1. First stream: `0x0CC0190` extracts packed index, obtains resource pointer, calls its virtual slot `+0x38` at `0x0CC01AD`, and forwards the returned pointer to direct `0x0CC13B0` at `0x0CC01BC`. This is a **resource-derived producer→consumer** edge, distinct from a raw GPU address.
2. Second stream: `0x0CC01F8` loads an array/table pointer from an enclosing object `+0x208`; `0x0CC0210` extracts packed index, obtains resource pointer `[r15+rax*8]`, and calls `0x0CAC030` at `0x0CC0222`.
3. Third stream: `0x0CC026F` loads a *different* table pointer at enclosing object `+0x218`; `0x0CC0290` extracts packed index and `0x0CC02A5..0x0CC02B3` passes `[r15+rax*8]+0x14` as a 16-byte descriptor pointer to the previously verified `0x02F62D60` four-DWORD descriptor consumer, with count 1 and state `[r12+0x460]+0x2E8`.

Both latter loops read an initial per-group DWORD at `[table+rbx*4]`, interpret it as a relative DWORD index, then walk packed entries at `table+index*4`. This establishes **two distinct enclosing-object table fields** and their specific consumer destinations. It does not establish allocation, mutation, bounds or exact class identity for either table. In particular, `0x1008` does **not** prove 16-bit indices are within `r15` bounds; validation may occur upstream.

**Evidence:** independently decoded with x86-64 objdump at exact instruction boundaries, original SELF SHA-256 checked, three region SHA-256 values:
- ELF `0x0CC0190..0x0CC01C1`, SELF `0x0CE00C0..0x0CE00F1`: `4c341b938c414cbdf6bd51e0a282be7c03b4a7848e95ba2fad9795ece4274549`
- ELF `0x0CC01F4..0x0CC023C`, SELF `0x0CE0124..0x0CE016C`: `4320f0bf3e168fbbbee555920f54558381af01d633cd4a205cbfd797d1685fae`
- ELF `0x0CC0266..0x0CC02D2`, SELF `0x0CE0196..0x0CE0202`: `040fd6d5c502e95c3601df8c291dc2ad388118474988770895b55a523a40b4e1`

**Confidence:** high for three dispatch streams, field offsets, index extraction, call destinations and descriptor pointer arithmetic. Medium for treating all three as a single shader binding phase; they might represent multiple generic resource update categories. FDE and relocations were not independently established in this pass. No runtime reachability claimed.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can compare the three original semantic resource paths separately: virtual `+0x38`→`0x0CC13B0`, `+0x208`→`0x0CAC030`, and `+0x218`→four-DWORD descriptor writer `0x02F62D60`. Avoid conflating CPU descriptor memory, resource-object pointers, GPU virtual addresses and Vulkan bindings.

**Next precise target:** independently trace the producer/initialization and mutation of enclosing-object `+0x218` table, and verify packed entry bounds and the `r15` resource-array allocation.
