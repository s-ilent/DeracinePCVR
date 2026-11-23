# C-011 — Paired CPU transfer API: original pointer provenance and direction

Target: original Déraciné 1.00 SELF, size 81,506,767; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently checked for this investigation. Static only. In these executable windows, **original SELF offset = ELF VA + 0x1FD30**. ELF VA, SELF file offset, loaded guest address, relocated pointer slot, GPU VA, shader identity, and Vulkan binding are separate domains.

## Independently established interface

Two complete entry points `0x91C0` and `0x9530` preserve the **same six-argument calling convention** before dispatching into the opposite-direction transfer helpers already identified in C-010. Entry instructions `0x91DF..0x91E8` and `0x954F..0x9558` map `RDI→RBX`, `RSI→R13`, `RDX→R14`, `RCX→R12`. Both require all four pointers non-null (`0x91EB..0x9219`, `0x955B..0x9589`), validate the descriptor passed as original RDX using `0xAD0`, copy descriptor bytes into a stack record, call `0x9C0` and `0x3020`, then select a format-dependent branch through separate 16-entry jump tables (`0x33CACD0`, `0x33CAD50`). The selected branch calls layout constructor `0x5760` and passes **the same argument positions** to the directional consumers:

| Stage | First entry | Second entry |
|---|---|---|
| Layout constructor | `0x92CA -> 0x5760` | `0x963A -> 0x5760` |
| RDI (derived layout) | `R14 = rsp+0xA0` | `R14 = rsp+0xA0` |
| RSI | `RBX = original RDI` | `RBX = original RDI` |
| RDX | `R13 = original RSI` | `R13 = original RSI` |
| RCX | `R12 = original RCX` | `R12 = original RCX` |
| R8D/R9D | restored original R8D/R9D | restored original R8D/R9D |
| Consumer | `0x92E5 -> 0x6190` | `0x9655 -> 0x6F40` |

Together with C-010's independently decoded opposite-direction loads/stores at `0x6714..0x6763` and `0x74C2..0x7507`, this identifies **one shared validated descriptor/region API with two CPU transfer directions**. The layout descriptor is original RDX, not the original RDI/RSI buffer pointer. The first two pointer arguments remain **unattributed buffers**: the entry prologues establish their positions but not ownership or whether either is GPU-addressable. No claim of UE4 resource class, shader stage, or live execution.

### Evidence and provenance

- ELF `0x91C0`, SELF `0x28EF0`, length `0x160`, SHA-256 `93430d415991cb48f1634be388544d6075898a4135f3bc129bee7f69d0ecac80`.
- ELF `0x9530`, SELF `0x29260`, length `0x150`, SHA-256 `87e90374c7c88bebd5e814130b6d6ddc77cd44c4dffec1525dc2f02c03d106c9`.
- Exact instruction-boundary decode with GNU objdump x86-64 Intel, beginning at each prologue; direct E8 targets independently checked by verifier. No new FDE, relocation, constructor or external caller has been established.
- Confidence **high** for ABI, layout construction, argument propagation and paired transfer direction; **medium** for classification as staging/layout conversion. Competing interpretation: generic CPU texture region conversion with no direct RHI buffer ownership. Prior C-008/C-009/C-010 edges are dependencies, not new findings.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-10/SC-11 can test original RDX as the layout descriptor and original RDI/RSI as the two transfer endpoints when investigating guest resource bytes. SC-09 should not mistake this CPU transfer interface for SGPR descriptor-table binding or a GPU address. **Next precise target:** identify direct callers and FDE ranges of `0x91C0` and `0x9530`, then prove which endpoint receives the `0x5B90` layout-derived address; prioritize C-001 shader vcall +0x38 ownership if these callers do not connect to RHI.
