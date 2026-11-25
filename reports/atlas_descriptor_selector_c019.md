# C-019 — Original shader descriptor writer's per-index register-offset selector

**Source:** original PS4 SELF 1.00 (81,506,767 bytes), SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, independently checked. All addresses below are **ELF virtual addresses**; original SELF file offset = ELF VA + `0x1FD30` for these locations. Loaded guest addresses, relocated slots, GPU VAs and shader hashes are distinct and not established.

## New original-byte finding

The two C-018 descriptor consumers share a **seven-entry 16-bit selector table** at ELF VA `0x037530B0` (SELF `0x03772DE0`). The table's first 14 bytes are `[0x2E40, 0x2C0C, 0x2C4C, 0x2C8C, 0x2CCC, 0x2D0C, 0x2D4C]`. Its SHA-256 is `36ea4b08ba4748fc9b77c7cf90e0a638da6f818f907f5e98615bbbb6620cd75b`. This establishes **actual original-target values**, not merely a table reference.

Instruction-boundary disassembly of `0x02F62B38..0x02F62B64`:
- `0x02F62B38 lea rsi,[rip+0x7f0571]` targets `0x037530B0`.
- `0x02F62B3F movzx ecx,r13w` takes the low 14 bits of the WORD mapping previously masked at `0x02F62AC7`.
- `0x02F62B43 movzx edx,word ptr [rsi+rdx*2]` indexes the selector table by **input ESI** (saved at `[rbp-0x58]`).
- `0x02F62B57 lea ecx,[rcx+rdx-0x2c00]` derives the control DWORD, stored at output record `+0x0C` at `0x02F62B61`.
- The second consumer independently uses the **same** table through `0x02F62CC8 lea rsi,[rip+0x7f03e1]`, then equivalent arithmetic.

Thus `control_dword = (mapping_word & 0x3FFF) + selector_table[input_index] - 0x2C00` (32-bit arithmetic). For input index 0 (the direct C-017 call path), the base is **0x240**, so the control DWORD is `0x240 + (mapping_word & 0x3FFF)`. Other observed table bases after subtraction are `0x00C,0x04C,0x08C,0x0CC,0x10C,0x14C`. Both consumers place this control DWORD immediately before the 32-byte descriptor payload within their 48-byte records.

This is a **concrete stage/index-dependent register-offset encoding path** suitable for SC-09 comparison. It is *not* proof that the seven entries are seven shader stages, nor a verified PS4 SGPR register name: the table is indexed without an independent bounds check inside these consumers, and the caller's valid input-index domain is not yet proven. The original `0x6875000DC0001000` record prefix remains uninterpreted.

**Confidence:** high for table bytes, both direct RIP-relative references, selector arithmetic and record field placement; medium for hardware-register-offset interpretation. Competing explanation: internal writer address offsets rather than direct hardware user-data register indices. No static proof of live execution.

**Verifier:** `tools/atlas/verify_descriptor_selector_c019.py` checks original SELF digest, table bytes, and instruction anchors.

**Transfer to SC-09/SC-10/SC-11 or FD4:** SC-09 can compare emitted record `+0x0C` against `0x240 + (mapping_word & 0x3FFF)` for the observed ESI=0 shader-state transition, then test whether this offset resolves to a specific PS4 user-data register. Do not infer a Vulkan binding or GPU address.

**Next precise target:** identify the producer of `[state+0xC180+index*8]` and the initialization of its `+0x22/+0x82` WORD submaps; independently join the resulting control DWORD to PS4 register definitions.
