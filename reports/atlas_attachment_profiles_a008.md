# A-008 — Original-SELF attachment template values and four progressive profiles

**Status:** new original-target-byte-verified static finding, extending A-007; no live reachability, shader-hash or exact GPU register attribution. Original SELF 1.00 SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, 81,506,767 bytes, rehashed this run.

## Original-byte findings

The A-007 routine at ELF VA `0x02F5D0F0` has an instruction-boundary-confirmed unsigned selector `(value-1)<=3` at `0x02F5D101–0x02F5D10E`. Independently decoded scaled loads at `0x02F5D112–0x02F5D14F` access **five four-element DWORD arrays** and **one four-element QWORD array**. The six table bases are ELF VAs `0x035AF340`, `0x035AF350`, `0x035AF360`, `0x035AF370`, `0x035AF380`, `0x035C3940`. Unlike A-007, this run independently read and checked the **actual table contents** from the original SELF.

| Backing value | DWORD 1 | DWORD 2 | DWORD 3 | DWORD 4 | Fifth DWORD (nibble) | QWORD | Final packed DWORD |
|---|---|---|---|---|---|---|---|
| 1 | `0000c44c` | `00000000` | `00000000` | `00000000` | 4 | `0000000000000010` | `00108001` |
| 2 | `22eea66a` | `00000000` | `00000000` | `00000000` | 6 | `0000000000001032` | `0020c002` |
| 3 | `5bb137d9` | `1ff5739d` | `00000000` | `00000000` | 7 | `0000000054106327` | `0030e003` |
| 4 | `5bb137d9` | `1ff5739d` | `6e8224a8` | `0ac640ec` | 8 | `a85410feb632dc97` | `00410004` |

**New structural observation:** the four cases are not arbitrary independent command templates. The DWORD arrays progressively populate additional nonzero fields: case 1 has one nonzero DWORD, case 2 one, case 3 two, case 4 four. Cases 3 and 4 share the first two DWORDs; the QWORD increases in populated bits across cases. The code at `0x02F5D1C1–0x02F5D1FD` repeats the four-DWORD row four times in the 18-DWORD first packet. At `0x02F5D210–0x02F5D21B` it writes the QWORD as two DWORDs in the next packet. At `0x02F5D257–0x02F5D265` the separate 3-DWORD packet combines `value&7 | ((value&7)<<20) | ((fifthDWORD&15)<<13)`; the table therefore determines the packed values shown above.

**Address provenance:** the six table ELF VAs fall in the first ELF PT_LOAD executable segment, with original SELF file offsets `VA + 0x1FD30` (respectively `0x035CF070`, `0x035CF080`, `0x035CF090`, `0x035CF0A0`, `0x035CF0B0`, `0x035E3670`). The code VA `0x02F5D0F0` maps to SELF offset `0x02F7CE20`. These are **file offsets**, not loaded guest addresses, relocated table slots, PS4 GPU VAs or shader hashes. The table entries here are scalar command data, not proven pointer relocations; no relocation-adjusted values are claimed.

## Interpretation, competing hypotheses, and limits

High confidence in the selector, six array accesses, exact original-byte table values, progressive row structure, fourfold emission and packed final DWORD. Moderate confidence these represent attachment-related GPU state profiles, because the A-006 call chain passes an attachment backing DWORD to the A-007 emitter. **Not established:** whether the progressive fields represent MRT slots, multisampling, colour/depth attachment format, blend state, or another graphics configuration. In particular, a fourfold repeat does not by itself prove four render targets; the four cases might encode different render-state families. The 64-bit values have not been decoded as hardware registers. No runtime reachability or resource ownership follows from this static evidence.

## Reproduction

Run `python tools/atlas/verify_attachment_profiles_a008.py <private-original-self>`. The checker hashes the original SELF, reads each table from the verified first PT_LOAD file mapping, asserts all 24 table values, disassembles the original function window with objdump, checks instruction boundaries and RIP-relative table references, and derives the four final packed DWORDs. Do not commit the executable or original data dump.

**Next:** trace the producer of backing DWORD `[resource_virtual_+0x38_result+0x3C]` and enumerate its assignments, then test whether its allowed values 1–4 correspond to specific attachment types or render-target modes. Alternatively decode the 0x6900 packet headers against verified PS4 GNM register semantics.