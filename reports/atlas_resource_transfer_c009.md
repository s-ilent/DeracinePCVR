# C-009 — Three C-008 consumers converge on CPU-side element addressing and copying

**Target:** original Déraciné 1.00 SELF (81,506,767 bytes), SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently checked for this investigation. Static only. In this mapped segment original SELF file offset = ELF VA + `0x1FD30`; no loaded guest VA, relocated slot, PS4 GPU VA, Vulkan binding, shader hash or runtime execution is inferred.

## Independently decoded finding

C-008's three immediately downstream consumers are **not three unrelated GPU resource creation paths**. ELF `0x6190` and `0x6F40` are closely corresponding, separate bounded-region transfer wrappers; both call the **same** element-addressing helper `0x5B90` (the third C-008 consumer) and classifier `0x2EC0`. Both take a derived layout record in RDI and check layout `+0x44` before entering a nested region loop. The special `+0x44==1` branch instead calls their respective local helpers `0x64D0` and `0x7280`, which remain unclassified.

In the general branch, both validate non-null RSI, RDX and RCX; read three start/end pairs from RCX `+0/+0xC`, `+4/+0x10`, `+8/+0x14`; reject nonpositive extents and extents exceeding layout `+0x0C/+0x10/+0x14` or supplied R8/R9 limits; and iterate a three-dimensional bounded region. At `0x636C` / `0x711C` each wrapper calls `0x5B90` with the layout in RDI and the address-output pointer in RSI. They then call `0x2EC0` with layout `+0x44`. The wrappers dispatch via separate 16-entry relative jump tables `0x33CAB10` (at `0x63BB`) and, for the second wrapper, a corresponding table in its matching branch (exact address to verify in next pass). The first wrapper's decoded cases at `0x63CF..0x6440` **load from one CPU pointer and store into another** in widths 1, 2, 4, 8 or 16 bytes. This is a concrete CPU-side copy operation, not merely a shader-state writer.

The common `0x5B90` helper reads layout `+0x08`, `+0x24`, `+0x38`, `+0x3C`, and conditionally `+0x64/+0x68`; it calls `0x4090` at `0x5BE9` to derive an addressing component and applies format-dependent coordinate bit manipulations. The exact layout semantics and full helper return contract remain unresolved. Critically, neither wrapper's demonstrated per-element loop directly reads the C-008 computed size-like `+0x28` or power-of-two `+0x48..+0x58` fields in the inspected general-path windows. Those fields may still be used in special helpers or deeper callees; absence here is not proof of irrelevance.

## Original-byte evidence

| ELF VA | Original SELF offset | Window size | SHA-256 |
|---|---|---:|---|
| `0x6190` | `0x25EC0` | `0x340` | `4afd160d0b2d1b841bad5c326ddd51f397f0127c410d88c7380e91c5162370be` |
| `0x6F40` | `0x26C70` | `0x340` | `4aa6a7c5bfd7c68ca51909b456894596f9864a7c21ae8263efdfbbca2c8e1928` |
| `0x5B90` | `0x258C0` | `0x140` | `5efea35fbff3a67e1093c507fc82bf03ad375bd3118429f9f357ab8f37a05eb2` |

Instruction-boundary decoded direct edges: `0x61C3 -> 0x64D0`, `0x636C -> 0x5B90`, `0x6380 -> 0x2EC0`, `0x6F73 -> 0x7280`, `0x711C -> 0x5B90`, `0x7130 -> 0x2EC0`, `0x5BE9 -> 0x4090`. Byte verification and instruction-boundary checking use `tools/atlas/verify_resource_transfer_c009.py`. FDEs, relocation of the jump tables and full special-path behavior remain unverified.

**Confidence:** high for direct call graph, shared layout accesses, three-dimensional region checks and first wrapper's CPU load/store operations; medium for the generic CPU transfer/format-conversion classification; unresolved for C++ identity, buffer ownership, GPU residency and relationship to UE shader uniform/storage buffers. Competing interpretation: this may be a general image/texture layout utility rather than a shader buffer interface.

**Transfer to SC-09/SC-10/SC-11 or FD4:** C-008 `0x5760` and consumers `0x6190/0x6F40/0x5B90` form a CPU-layout/transfer candidate. Do **not** promote their size-like output to Vulkan allocation or shader binding. SC-09 should prioritize actual RHI shader descriptor/user-data producers; SC-10 may use this mapping if CPU resource bytes need format-aware interpretation.

**Next precise target:** decode the two special-case helpers `0x64D0` and `0x7280`, and verify whether their differences implement opposite transfer direction; recover FDE bounds for both wrappers and establish whether `+0x28` or `+0x48..+0x58` are consumed there.
