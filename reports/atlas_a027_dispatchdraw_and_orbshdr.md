# Independent Binary Atlas A-027 — DispatchDraw GNM usage types and OrbShdr trailer

**Original-target static research.** Déraciné 1.00 original SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Inner-ELF VAs (not loaded GPU/CPU addresses). File-backed text offsets for this region = VA+`0x1FD30`. Local original-binary verifier passed **52/52 checks**: 43 opcode byte anchors, 4 dispatch-table targets, 5 derived/synthetic tests. No runtime usage claim.

## The previously unknown GNM kinds 0x22–0x25 have public names

A-026 showed four unlabelled parser branches. The full public fpPS4 GNM enum identifies them as *DispatchDraw*-specific input usages (see [fpPS4 shader header](https://github.com/red-prig/fpPS4/blob/trunk/chip/ps4_shader.pas), `kShaderInputUsageImmGdsKickRingBufferOffse` through `kShaderInputUsageImmDispatchDrawInstances`). A-027 cross-checks the original Déraciné instruction boundaries and all four relative jump-table entries:

| Input usage kind | PS4 GNM meaning (from fpPS4; source spelling abbreviated) | Original Execute? No: parser handler, metadata store |
|---|---|---|
| `0x22` | Immediate GDS kick-ring buffer offset for DispatchDraw | `0x02F5D551`: `[metadata+0x12] = startregister` |
| `0x23` | Immediate vertex-ring buffer offset for DispatchDraw | `0x02F5D55F`: `[metadata+0x13] = startregister` |
| `0x24` | Pointer to DispatchDraw data | `0x02F5D56D`: `[metadata+0x14] = startregister` |
| `0x25` | Immediate `(firstInstance<<16)|(numInstances-1)` | `0x02F5D57B`: `[metadata+0x15] = startregister` |

The original source `startregister` byte is loaded into `ebx` at `0x02F5D3B0`; all four handlers store `bl`, *not* flags, API slot or a 16-bit descriptor mapping. The first pass range-check at `0x02F5D3D4` includes kinds 0..0x25. The second pass handles only usage kinds `0x13..0x19`. OpenGNM's public usage enum currently stops at `0x1E`; fpPS4's longer enum supplies these labels. Their presence in a generic decoder is **not evidence of Déraciné runtime DispatchDraw reachability** and does not establish a Déraciné-specific shader extension.

## Precise native shader-binary trailer provenance

A-025 found two header bytes preceding the computed end of a shader program. A-027 identifies the fields by matching the original decoder's offsets to the published 28-byte trailing **`GnmShaderBinaryInfo`** structure (signature `OrbShdr`, not the leading shader-file `Shdr` header). See [OpenGNM shaderbinary definition](https://github.com/PS4-OpenGNM/opengnm/blob/main/include/gnm_shaderbinary.h) and [fpPS4 trailer definition](https://github.com/red-prig/fpPS4/blob/trunk/chip/ps4_shader.pas).

Instruction chain:

- `0x02F5D2BF`: read stage word `+0x08` (lower shader program address); `0x02F5D2C2`: word `+0x0C` (upper).
- `0x02F5D2C8/0x02F5D2CC`: shift these by 8 and 40, respectively. `0x02F5D2C5/0x02F5D2D3`: extract the 23-bit size field from stage word `+0`.
- `0x02F5D35E`: derive footer at `program_address + size_field - 0x1C`.
- `0x02F5D2E6`: read footer `+0x0C` (the chunk usage offset in DWORDs); `0x02F5D2E0`: footer `+0x0D` (number of four-byte input-usage declarations).
- `0x02F5D354,0x02F5D386..0x02F5D395`: compute a reverse-indexed record loop; `0x02F5D3B0..0x02F5D3E4` load four fields per record.

Original machine-code byte arithmetic (NOT a safe parser without independent bounds checks):

```cpp
u64 program = (u64(stage_word_8) << 8) | (u64(stage_word_C) << 40);
u32 size_field = stage_word_0 & 0x7FFFFF;
u8* info = (u8*)(program + size_field - 0x1C); // 28-byte OrbShdr trailer
u8 chunk_base_dwords = info[0x0C];
u8 num_slots = info[0x0D];
u8* slots = info - 4 * (chunk_base_dwords + num_slots);
// Each slot: { usagetype, apislot, startregister, flags }
```

The original code's low-23-bit field is *added as bytes*, with no scaling; don't transfer external documentation's purported unit conventions without checking this exact implementation. The parser starts with stage-specific program records, then follows the **trailing binary-info structure**, not an Unreal Engine resource table or leading shader-file header. A read-only scan found no literal `OrbShdr` signatures in the submitted 81.5MB SELF. That does *not* exclude shaders in other archives or created at runtime; we did not decode a real Déraciné shader in this pass.

## Cross-game implication

`GnmShaderBinaryInfo` and `GnmInputUsageSlot` are PS4 GNM-level formats and should be reusable across unrelated engines. The discovered parser's code RVA and resource lifetimes remain specific to the original Déraciné executable. Treat DispatchDraw kinds as specialized user-data declarations, not as textures or constant buffers. Prefer verifying actual shader binary declarations when SC-series needs provenance; do not add hooks or change working replay purely on this static result.

## Reproduction and scope

`python tools/atlas/verify_independent_a027.py <private-approved-Deracine-1.00-eboot.bin>`. A corresponding standalone local report and verifier were run and stored separately; local result: **52/52**. No commercial binary, shader program bytes or original runtime logs committed.

Remaining: obtain a permitted live shader sample and validate header/signature/bounds and actual usage distribution; determine whether these four types are ever exercised in Déraciné gameplay.