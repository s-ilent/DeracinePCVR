# A-009 — Attachment backing metadata mutation during binding

**Status:** verified new static finding on original 1.00 SELF (SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, 81,506,767 bytes). No runtime reachability claim.

## Original-byte evidence

At ELF VA `0x0CC7E43`, the A-006 resource-binding helper obtains a backing pointer by virtual call `call [resource_vptr+0x38]` (at `0x0CC7E4A`), preserves the result in `r14`, loads backing DWORD `[r14+0x3C]`, and directly calls `0x0CC9240` (A-006/A-007 graphics-state writer).

**Newly verified second behavior in the same helper:** after the graphics-state writer returns, the helper conditionally **mutates the backing object**:

| ELF VA | Instruction/effect |
|---|---|
| `0x0CC7E69` | Read global byte `[0x047B438A]` |
| `0x0CC7E6F–0x0CC7E73` | Invert byte and test bit zero; skip mutation when original bit zero is set |
| `0x0CC7E75` | Read global DWORD `[0x04496748]` |
| `0x0CC7E7B` | Compare backing DWORD `[r14+0x08]` to zero |
| `0x0CC7E82` | If zero, initialize `[r14+0x08] = 1` |
| `0x0CC7E8A` | Set backing BYTE `[r14+0x10] = 1` |
| `0x0CC7E8F` | Set backing DWORD `[r14+0x0C]` to global DWORD from `0x04496748` |

The branch is reached only if the input resource pointer was non-null and the virtual method returned a non-null backing pointer. The original global bit zero must be clear. This is a distinct metadata update from the already-known `+0x3C` graphics-state selector. It gives four independently observed backing offsets (`+0x08,+0x0C,+0x10,+0x3C`), three explicit writes, and a conditional initialization.

A separate block at `0x0CC7F3C–0x0CC7F5F` reads a global index `[0x044965FC]`, indexes a global pointer table based at `0x044962C8`, adds `0x60`, and compares the resulting pointer with the same backing object `r14`. This proves the helper can recognize a distinguished globally indexed backing object, but does not prove the type or owner of that global table.

**Address provenance:** all instruction and global addresses above are ELF VAs; the instruction windows map to original SELF offsets via `VA + 0x1FD30` (e.g. `0x0CC7E43 → 0x0CE7B73`; `0x0CC7F3C → 0x0CE7C6C`). The global VAs are not original file offsets or GPU addresses; no loaded guest address, relocated table slot, or shader hash is inferred. The original SELF was rehashed and both disassembly windows checked against original bytes.

## Interpretation and limits

High confidence: the same backing object supplies a graphics-state selector and receives conditional metadata updates. Moderate confidence: `+0x08` is a one-time initialized flag/counter, `+0x10` a dirty/used flag, and `+0x0C` a frame/epoch-like stamp. **Those names are hypotheses**; `0x04496748` has not been traced to its producer, so it cannot yet be called a frame counter. The global object equality test is not sufficient to assign a C++ type or ownership model. This finding is not a constructor or proof of permitted `+0x3C` values.

## Reproduction

Run `python tools/atlas/verify_attachment_metadata_a009.py <private-original-self>`. It verifies the complete SELF digest and exact original-byte windows. Independent x86-64 instruction-boundary disassembly was performed with GNU objdump; the listed addresses and control flow were checked against those windows. No copyrighted bytes are committed.

**Next:** trace writers to global `0x04496748` and/or a producer of backing `+0x3C`; resolve the backing object's vtable implementation and the distinguished global table without importing external game structure offsets.
