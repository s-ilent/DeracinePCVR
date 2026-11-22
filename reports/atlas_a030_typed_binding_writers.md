# Independent Binary Atlas A-030 — seven typed GNM binding writers

**Status:** Original-SELF static discovery plus a metadata-derived interpretation of two *genuine* title-screen shader declarations reported by Codex A-028R. **Not** a live join between a specific guest shader, a guest resource object, and a Vulkan resource.

- Target: original Déraciné PS4 1.00 `eboot.bin`, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`.
- Addresses below: **inner ELF VAs**; verified original SELF text file offset `VA+0x1FD30`.
- Verification: `python verify_a030.py /private/original/eboot.bin` → **136/136 passed**. The verifier checks original code bytes, map offsets, payload widths, PM4 headers, packet reservations, dirty markers, 15 direct `E8 rel32` calls, and eight metadata-derived (not runtime) routing examples. No game/shader data is distributed.
- Source for real declaration examples: [Codex A-028R report](https://github.com/s-ilent/DeracinePCVR/blob/research/a028-gnm-shader-metadata-20261123/reports/A028R_gnm_shader_metadata_validation.md) @ `51051db3`; this investigation does **not** have the private raw spans and cannot independently re-run the 248-shader validation.

## Result: seven related GNM immediate-resource writer implementations

The previously identified `0x02F62440` is **only the immediate constant-buffer path**, not a universal writer. Six adjacent sibling routines share the same stage-map indirection, the `0xFFFF` missing-slot check, the masked 14-bit offset, a direct-PM4 path and a cached/shadow-register path. Each selects a different map produced by the A-025 decoder:

| Writer VA | GNM usage (`GnmInputUsageSlot.usagetype`) | Metadata map offset | Payload | Direct packet base | Original direct calls |
|---|---|---:|---:|---|---:|
| `0x02F62440` | `0x02` immediate constant buffer | `+0xC2` | 16 bytes (4 DW) | `0xC0047600` family | 4 |
| `0x02F625C0` | `0x03` immediate vertex buffer | `+0xEA` | 16 bytes (4 DW) | `0xC0047600` family | 2 |
| `0x02F62740` | `0x00` immediate resource | `+0x22` | 16 bytes (4 DW) | `0xC0047600` family | 0 direct `E8` sites found |
| `0x02F628C0` | `0x04` immediate RW resource | `+0x82` | 16 bytes (4 DW) | `0xC0047600` family | 0 direct `E8` sites found |
| `0x02F62A40` | `0x00` immediate resource | `+0x22` | **32 bytes (8 DW)** | `0xC0087600` family | 1 |
| `0x02F62BD0` | `0x04` immediate RW resource | `+0x82` | **32 bytes (8 DW)** | `0xC0087600` family | 7 |
| `0x02F62D60` | `0x01` immediate sampler | `+0xA2` | 16 bytes (4 DW) | `0xC0047600` family | 1 |

The two 32-byte variants load/store YMM words (`vmovups`) and advance PM4 writer cursor by **0x30 bytes**, whereas 16-byte variants use XMM words and advance by **0x20 bytes**. Reservations request **12** versus **8** command DWORDs respectively. The `SET_SH_REG` opcode is `0x76`, with stage-dependent header low-bit variation. These are **PS4 GNM descriptor widths**, not Vulkan descriptor set binding IDs.

For types 0x00 and 0x04, the public PS4 `GnmInputUsageSlot` flag `registercount` is compatible with 4DW/8DW usage, but the original direct caller set is not yet traced sufficiently to prove every size-variant selection comes from that flag. Both variants exist in the original code.

## Shared algorithm proven from original instructions

For a call with writer, `stage`, `slot`, `count`, `source`:

1. Get stage's **effective** map pointer from `writer+0xC180 + 8*stage`. It may be resource metadata or writer-owned scratch state (A-024).
2. Read the chosen typed map at `map + type_map_offset + 2*slot`; `0xFFFF` means unmapped and is skipped.
3. Extract `mapped = entry & 0x3FFF`; use **bit 15** to choose the mode:
   - **Bit 15 clear:** copy 16 or 32 bytes into writer's per-stage shadow area: `writer+0x90 + stage*0x1800 + mapped*4` (with a capacity/lifetime obligation not independently established here).
   - **Bit 15 set:** generate a PM4 `SET_SH_REG` packet for 4 or 8 descriptor DWORDs, using the seven-entry stage register-base table at `0x037530B0`: `register_offset = stage_base[stage] + mapped - 0x2C00`.
4. The writer records an update/dirty indicator at `writer+0xC250+stage`.

The original **map decoder** applies `startregister < 16 ? (0x8000 | startregister) : (startregister-16)` to sampler, constant-buffer and vertex-buffer usage kinds 1, 2 and 3 (verified directly from their dispatch handlers). Resource and RW-resource kinds 0 and 4 additionally OR **bit 14** when the GNM flags byte has bit 1 *clear*: `encoded |= (((flags & 2) ^ 2) << 13)`. The original writer masks the 14-bit destination index before using it, and independently tests bit 15 to select direct-vs-shadow. The GNM flag bit relates to `resourcetype` (V#/T#); its higher-level resource meaning is distinct from the address index. Do **not** confuse the resulting **per-writer map index** with Codex's Vulkan-side, flattened `sharp_idx` or with a Vulkan unified binding number. This observation also explains why declaring register 28 or 32 is not an attempt to index a 16-DWORD hardware array directly.

## Concrete genuine metadata examples: PS and VS corrected stage labels

A-028R independently verified these `GnmInputUsageSlot` declarations in title-screen shader metadata; we can now calculate the **original RHI writer path if bound through the recovered map**, without claiming a live link to a particular resource object:

| Original shader | GNM declaration | Record | Decoded map word | Writer mode |
|---|---|---|---:|---|
| PS `82c730ebc710047e` | immediate resource slot 0 | register 0, flags 3 | `0x8000` | direct, 32-byte resource descriptor (flags indicate 8DW) |
| PS `82c730ebc710047e` | immediate sampler slot 0 | register 8 | `0x8008` | direct, 16-byte sampler descriptor |
| PS `82c730ebc710047e` | immediate resource slot 1 | register 16, flags 3 | `0x0000` | shadow, 32-byte resource descriptor (flags indicate 8DW) |
| PS `82c730ebc710047e` | immediate sampler slot 1 | register 24 | `0x0008` | shadow, 16-byte sampler descriptor |
| PS `82c730ebc710047e` | immediate constant buffer slot 0 | register 28 | `0x000C` | shadow, 16-byte buffer descriptor |
| PS `82c730ebc710047e` | immediate constant buffer slot 1 | register 32 | `0x0010` | shadow, 16-byte buffer descriptor |
| VS `82c730eb4a0404ed` | immediate constant buffer slot 0 | register 4 | `0x8004` | direct, 16-byte buffer descriptor |
| VS `82c730eb4a0404ed` | immediate constant buffer slot 1 | register 8 | `0x8008` | direct, 16-byte buffer descriptor |

The observed PS `PTR_EXTENDEDUSERDATA` at slot 1/register 12 is **not** an `IMM_CONSTBUFFER` record, and is deliberately excluded from this table; it follows the separate A-029 metadata `+0x07` path.

**Verification qualification:** Original instructions were checked independently for the resource, sampler, constant-buffer, vertex-buffer and RW-resource decoder handlers, including the extra resource-class bit. Thus the **map words** above are original-code-derived from A-028R's published real shader declarations. The **32-byte payload width** comes from the independently proven 32-byte writer variant plus the public PS4 GNM `registercount` flag definition, **not a live resource call joined to this shader hash**. The Vulkan-side flattened `sharp_idx` remains independently defined.

## Direct guest code caller evidence

A whole-file `E8` relative-target scan found **15 candidate direct call sites**, all individually checked by `E8 rel32` address arithmetic in the original SELF:

- `0x02F62440`: `0x0CAB29D`, `0x0CACE0A`, `0x0CB9FD6`, `0x0CC0DC7`. At `0x0CAB29D`, source is `resource+0x50`, stage comes from a resource field `+0x380`, and the context's graphics writer is `[context+0x460]+0x2E8`.
- `0x02F625C0`: `0x0CBFF98`, `0x0CC16AF` (vertex buffer).
- `0x02F62A40`: `0x0CB4C44` (32-byte immediate resource).
- `0x02F62BD0`: `0x0CB4C66`, `0x0CC3538`, `0x0CC3B6C`, `0x0CC4917`, `0x0CFEF63`, `0x0CFF56B`, `0x0D0013B` (32-byte RW).
- `0x02F62D60`: `0x0CC02B3` (sampler); the source is selected from an indexed object array and advanced by `+0x14` at `0x0CC02A5..0x0CC02B3`.

No direct `E8 rel32` caller was found for either 16-byte resource or 16-byte RW variant. This **does not prove they are dead**: other forms of indirect dispatch, inlining or calls outside the scanned executable region are not excluded.

## Native renderer action

This is now enough to design a *portable typed resource-write intermediate representation*: `{GNM stage identity, usage kind, logical API slot, map entry, 16/32-byte source snapshot, destination mode, mapped index, declared register base}`. A semantic UE4 adapter should capture the source data **by value at execution**, preserve its resource identity separately, and then resolve it into a Vulkan buffer/image/sampler without reinterpreting original PM4 packets.

Still missing: a **runtime-correlated** `(shader hash, stage, api_slot, source resource object, copied descriptor words, guest memory target)` example, GPU-written resource synchronization, and proof that these writer calls fully cover game materials or draw submission. The title-screen metadata alone does not supply this. We should obtain *one* bounded live join before constructing native Vulkan resources from guessed source identities.

## Next bounded experiment for Codex

Select an A-028R-known shader and, without changing rendering, compare `GnmInputUsageSlot` `(kind,api_slot,startregister,flags)` against the effective shader map and the corresponding typed helper at execution. Capture 16/32 bytes by value at the source call and compare with shadPS4's observed descriptor words for the **same draw and stage**. Record mismatches and execution context. This provides the missing link between original guest resource state and replay inputs, without binding the native renderer to shadPS4 internals.