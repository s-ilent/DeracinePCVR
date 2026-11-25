# C-018 — Paired descriptor consumers select CPU cache or 48-byte writer record

Original 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` reverified. All addresses are **ELF VAs**; original SELF offsets equal VA + `0x1FD30`. Neither loaded guest nor PS4 GPU address or shader hash is inferred. No runtime use is established.

## New original-target finding

C-017's two calls at `0x0CB4C44` and `0x0CB4C66` enter structurally matching consumers `0x02F62A40` and `0x02F62BD0`, respectively. Their distinct lookup offsets are **`+0x22` versus `+0x82`**, within a per-index table reached through `[writer+0x2E8 + 0xC180 + index*8]`. The selector supplied in EDX indexes WORDs, each two bytes apart. A WORD of `0xFFFF` suppresses the update. The low 14 bits select a destination; the sign bit selects a distinct emission path. Bit 14's independent meaning remains unresolved.

* `0x02F62AB3` and `0x02F62C43`: WORD lookup at `[r9+0x22+selector*2]` and `[r9+0x82+selector*2]`. Both mask `0x3FFF` and test signed WORD.
* Nonnegative WORD: `0x02F62AD3..AE1` and `0x02F62C66..C74` load 32 bytes from descriptor R8 (copied into RBX) and store them to an indexed **CPU-resident cache**, at `writerState+0x90+index*0x1800+(word&0x3FFF)*4`. This is a CPU memory write, not a GPU address or Vulkan descriptor.
* Negative WORD: `0x02F62AF0..B97` and `0x02F62C80..D27` use writer pointer at state `+0xC090`, check remaining DWORD capacity against 11, invoke writer callback `+0x18` requesting 12 DWORDs if needed, and append **0x30 bytes**. The record starts with `0x6875000DC0001000`, contains a 32-bit stage/index-dependent control word, and copies eight descriptor DWORDs to record offsets `+0x10..+0x2C`. Both use a 16-bit lookup table at ELF VA `0x037530B0` indexed by input ESI.
* Both finish by setting byte `state+0xC250+index` to 1. This is a CPU-side dirty/participation flag; it is not evidence of GPU buffer-cache versioning.

The two functions are near-identical specialized consumers, differing in lookup subtable and branch offsets, not independently proven vertex/pixel shader stages. The direct producer calls in C-017 pass ESI=0, ECX=1 and different WORD selectors from selected-entry `+0xBC/+0xC0`. The exact shader-stage/SGPR meaning of `0x6875000DC0001000` requires a register-definition join.

## Evidence and reproducibility

Instruction-boundary disassembly via `objdump -D -b binary -m i386:x86-64 --adjust-vma` of original SELF windows. Function prologues at `0x02F62A40` and `0x02F62BD0`; RETs at `0x02F62BCA` and `0x02F62D5A`. Contiguous byte SHA-256:
* ELF `[0x02F62A40,0x02F62BCB)` = SELF `[0x02F82770,0x02F828FB)`: `39a67309be19bfccb4380dd3972494f0501eb8837ecd7575b4962176a4c2b301`.
* ELF `[0x02F62BD0,0x02F62D5B)` = SELF `[0x02F82900,0x02F82A8B)`: `ca32a701a962d7800b0ac37314e9475c89a65ee1c29584a6c45c2c4a8f006885`.

Checker: `tools/atlas/verify_descriptor_consumers_c018.py`. FDE records and exact C++ type ownership remain unresolved. Confidence high in field offsets, branches, record length and CPU-versus-writer split; medium in shader binding interpretation. Competing explanation: generic graphics-resource descriptor routing, not necessarily a per-stage shader binding interface.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can test whether selected-entry WORDs at +0xBC/+0xC0 resolve to nonnegative CPU-cache destinations or negative 48-byte command emissions, and correlate the resulting record with actual shader user-data registers without assuming a Vulkan binding number.

**Next precise target:** identify the producer of the per-index WORD tables at state `+0xC180`, and decode `0x037530B0` control-word selection against original PS4 register definitions.
