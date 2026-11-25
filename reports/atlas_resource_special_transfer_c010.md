# C-010 — Opposite-direction CPU transfers in special resource-layout paths

**Target:** original Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, verified in this investigation. Static analysis only. For both code windows, original SELF offset = ELF VA + `0x1FD30`; loaded guest addresses, relocation slots, GPU virtual addresses, shader hashes, and Vulkan bindings are distinct and not established here.

## New finding

The previously unclassified special-case consumers `0x64D0` and `0x7280` are **directionally opposite CPU memory-transfer routines**, rather than shader user-data writers. Both are called from the `+0x44 == 1` branch of C-009's wrappers (`0x61C3 -> 0x64D0`, `0x6F73 -> 0x7280`). Each independently checks non-null pointers, validates three start/end coordinate pairs and layout bounds (`+0x0C/+0x10/+0x14`), checks layout `+0x38 == 2`, and derives element width from layout `+0x24 >> 3`. Both call the shared address calculation helper `0x5B90` (`0x66DF`, `0x7482`) and format classifier `0x2EC0` (`0x6669`, `0x740C`).

The **direction** is established by independently decoded, instruction-boundary-confirmed paired loads/stores. At `0x6714/0x671E`, `0x6723/0x672D`, `0x6733/0x673C`, `0x6741/0x674B`, and `0x6751..0x6763`, the first helper loads from the address in RBX and stores to an RDX+RAX destination (1/2/4/8/16-byte cases). In the second helper, `0x74C2`, `0x74D1`, `0x74E0`, `0x74EF`, and `0x74FB..0x7507` store **to RBX**, reading from RDX+RAX. Thus these are reversed source/destination roles across the two wrappers. This is not merely a similarity of function shapes: the memory operands establish direction.

Both paths also compare a supplied stack argument with layout `+0x44` (`0x6519..0x651F`, `0x72C6..0x72CE`) and bound the region's extents before copying. No direct access to the C-008 derived size-like `+0x28` or power-of-two `+0x48..+0x58` is established in the verified windows; other paths remain unexamined.

## Reproducible original-byte evidence

| ELF VA | Original SELF file offset | Length | SHA-256 |
|---|---|---:|---|
| `0x64D0` | `0x26200` | `0x300` | `5afc5085eb015034c6acf4e42f0afdb3bfb3a661edff52a143026a9a8307bb8f` |
| `0x7280` | `0x26FB0` | `0x300` | `66db7e3d4e63995cb069752d2a25389779d9ee4fcfe34f354c537c8603f45f4a` |

Direct relative calls: `0x66DF -> 0x5B90`, `0x7482 -> 0x5B90`, `0x6669 -> 0x2EC0`, `0x740C -> 0x2EC0`. See `tools/atlas/verify_resource_special_transfer_c010.py` for SHA, call-target and exact instruction-byte assertions. GNU objdump x86-64 Intel decoding was used at the stated ELF VAs. FDE coverage and jump-table relocations are **not verified**.

**Confidence:** high for opposite CPU copy direction, shared address helper and checked layout fields; medium for classification as layout-specific upload/download-style conversion. **Competing interpretations:** generic CPU texture swizzle/unswizzle or staging transfer rather than UE4 RHI buffer upload/download. Neither shader stage nor GPU resource ownership is established. No live execution is claimed.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-10 can use the paired CPU-side layout transfer routines when interpreting guest-side resource bytes. SC-09 must not treat these as shader SGPR tables, shader hashes, Vulkan descriptors or proof of GPU residency. The directionality is useful for identifying which side of a candidate guest-memory transfer is linear versus layout-addressed, once pointer provenance is recovered.

**Next precise target:** trace callers `0x92E5` and `0x9655` upstream to the source and destination pointer construction, and verify whether the `0x5B90`-derived address is the layout-addressed side of both transfers. Recover FDE boundaries before C++ attribution.
