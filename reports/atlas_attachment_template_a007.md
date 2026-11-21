# A-007 — Four-way table-driven attachment state packet writer (original SELF)

**Status:** independently decoded and byte-verified static finding; no runtime reachability or exact packet-semantic claim. Original 1.00 SELF SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` (81,506,767 bytes), rehashed in this investigation. Extends A-006 (not a rediscovery of its call edge).

## New binary evidence (inner ELF VAs)

- A-006 emitter `0x0CC9240` invokes `0x02F5D0F0` at instruction `0x0CC93B5`, passing `RDI=[context+0x460]` (the command writer) and `ESI=original backing DWORD` (`R14D`). The independently decoded callee starts with a conventional prologue at `0x02F5D0F0` and ends with `ret` at `0x02F5D28E`; the following function starts `0x02F5D290`. No unwind/FDE boundary is claimed.
- `0x02F5D101–0x02F5D10E` computes `value-1` and unsigned-compares against 3. Thus only values **1, 2, 3, 4** select one of four table entries; all other inputs take the zero-default branch at `0x02F5D15E`.
- The valid branch independently loads **four DWORDs** through RIP-relative table bases `0x035AF340`, `0x035AF350`, `0x035AF360`, `0x035AF370` and a fifth DWORD from `0x035AF380`; it also loads a **QWORD** through `0x035C3940 + 8*(value-1)`. The fifth DWORD is masked with 0xF and shifted left 13. These are static table references, **not verified relocated table contents**. Precise runtime values require separate data-segment/relocation analysis.
- The writer at `[RDI]` uses `+0x08` end, `+0x10` cursor, `+0x18` growth callback and `+0x20` callback context. At `0x02F5D182`, if at most 21 DWORDs remain, it requests **22 DWORDs** from the callback. This is a capacity check, not a shader setter.
- On the successful path, `0x02F5D1AA–0x02F5D21B` emits a 0x48-byte first packet with QWORD `0x02FEC0106900` and four table-derived DWORDs repeated four times; then a 0x10-byte second packet with QWORD `0x02F5C0026900` and the table QWORD split into two DWORDs. This advances the cursor by **0x58 bytes (22 DWORDs)**. If growth fails, this pair is skipped.
- A separate final 3-DWORD command is emitted at `0x02F5D257–0x02F5D27C` with QWORD `0x02F8C0016900`. Its payload combines `value&7`, `(value&7)<<20`, and the table-derived nibble shifted 13. It has its **own** capacity check and growth call (`0x02F5D230–0x02F5D253`), so its success is independent of the preceding 22-DWORD reservation.
- The routine returns at `0x02F5D28E`. A zero-default input still emits the final command with its low three bits; it does not necessarily emit no state.

## Interpretation and limits

**High confidence:** A-006's attachment backing DWORD selects a four-entry graphics-state template and drives two related, separately capacity-checked command-emission stages. This is a new semantic bridge between the backing-object field and encoded graphics state, with a concrete writer ABI and packet sizes. **Moderate confidence:** these are GPU state packets; exact hardware register names, attachment format/resolve meaning and relocation-resolved table data are not yet proven. It is not evidence of backing ownership, resource lifetime, or actual execution on a live draw.

All code VAs here map to original SELF container offsets by **+0x1FD30** in the checked executable segment: `0x02F5D0F0 → 0x02F7CE20`, `0x02F5D1AA → 0x02F7CEDA`, `0x02F5D257 → 0x02F7CF87`, `0x0CC93B5 → 0x0CE90E5`. The table VAs are separate data-domain references; do not assume the code mapping proves their relocated values. These are not loaded guest VAs, PS4 GPU VAs or shader hashes.

## Reproduction

Verify original SELF SHA-256, read the bounded code windows at offsets above, then decode with `objdump -D -b binary -m i386:x86-64 -M intel --adjust-vma=<ELF VA>`. Run `tools/atlas/verify_attachment_template_a007.py` for byte-boundary and call-edge checks against the original SELF. No original bytes or private paths are committed.

**Next:** independently recover the table segment mapping/relocations and correlate the four cases with resource backing construction and the meaning of the three command headers.