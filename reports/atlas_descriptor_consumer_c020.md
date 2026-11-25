# C-020 — 16-byte descriptor routing through a third original shader/resource consumer

**Input:** original Déraciné PS4 SELF 1.00, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, rehashed this run. All addresses are **inner ELF virtual addresses**; original SELF file offset = ELF VA + `0x1FD30` for the verified windows. No guest loaded VA, relocated table slot, GPU VA, shader hash, or live reachability is inferred.

## Independently verified new finding

Function `0x02F62D60..0x02F62ED9` is a **third descriptor-map consumer**, distinct from the previously established 32-byte `+0x22`/`+0x82` consumers at `0x02F62A40` and `0x02F62BD0`. It uses the **`+0xA2` WORD submap**, processes **16-byte input descriptors**, and emits **32-byte writer records** for negative mapping words. Its caller `0x0CC02B3` supplies one descriptor at a time from a resource-derived pointer `[r15+rax*8]+0x14`, with the index selected by `bextr` at `0x0CC0293`. This is a concrete producer/consumer edge above the command writer, not merely a neighboring function.

Instruction-boundary evidence:

- `0x02F62D8E`: load mapping-table pointer `[rdi+rdx*8+0xC180]`; `0x02F62DD2`: WORD `[r9+rax*2+0xA2]`. `0xFFFF` skips, low 14 bits provide the destination, and the sign bit selects CPU cache versus writer.
- `0x02F62DF5..0x02F62E04`: nonnegative mapping copies **one XMMWORD** from R14 to CPU cache `[r8+rax*4]`, where R8 was derived from `rdi+0x90+index*0x1800`.
- `0x02F62E10..0x02F62E54`: negative mapping checks available writer DWORDs and requests **eight DWORDs** through callback `[r12+0x18]` when remaining capacity is at most seven. This is capacity growth, not a shader-state callback.
- `0x02F62E58`: RIP-relative reference to the same seven-WORD table `0x037530B0` proven in C-019. `0x02F62E77`: control DWORD = `(mapping_word & 0x3FFF) + selector_table[index] - 0x2C00`.
- `0x02F62E67..0x02F62EA3`: record prefix `0x6875000DC0001000`, an additional DWORD at +8 (`0xC0047600` for index zero, `0xC0047602` otherwise), control DWORD at +0xC, four descriptor DWORDs at +0x10..+0x1C, and writer cursor advances by `0x20`.
- `0x02F62EC3`: mark `state+0xC250+index` dirty.
- Caller `0x0CC0298..0x0CC02B3`: loads context `+0x460`, adds `0x2E8` to obtain consumer state, selects descriptor pointer from `[r15+rax*8]+0x14`, sets count ECX=1, and calls `0x02F62D60` with a decoded direct E8 at `0x0CC02B3`.

**Exact body:** ELF `[0x02F62D60,0x02F62EDA)`, SELF `[0x02F82A90,0x02F82C0A)`, 378 bytes, SHA-256 `89673455122f571b5907fa474a61517d29c0b4839a67d7a33521114672a6c63b`. The direct caller is SELF offset `0x0CDFFE3`. No FDE or relocation was independently established for this new function.

**Confidence:** high for disassembly, +0xA2 lookup, 16-byte CPU copy, 32-byte emission, caller argument construction and common selector table. Medium for the interpretation as shader/resource binding rather than a generic descriptor routing family. The register meanings and `bextr` selector provenance remain unresolved. CPU cache updates are not GPU buffer-cache version transitions.

**Verifier:** `tools/atlas/verify_descriptor_consumer_c020.py` rechecks SHA-256 and the original target's instruction windows and body hash.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can distinguish this **four-DWORD descriptor path** from the earlier eight-DWORD paths by record size (32 versus 48 bytes), mapping offset `+0xA2`, and record +8 constant. It can compare the control DWORD against the C-019 selector equation without assuming a Vulkan binding or GPU address.

**Next precise target:** recover the `+0xA2` WORD-map producer and trace the caller's `[r15+rax*8]+0x14` descriptor owner, including the `bextr` selector source and its valid bounds.
