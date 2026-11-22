# Independent Binary Atlas A-029 — Real-shader metadata and native extended-user-data contract

**Scope:** Independently verified original SELF byte paths, together with the separately collected A-028R *real Déraciné title-screen* statistics. This is a static integration contract, **not** an independent re-analysis of the private 248 shader binaries (they are not in this session) and **not** a native renderer implementation.

Source image: Déraciné 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Addresses are original inner-ELF VAs; text file offset VA + `0x1FD30` only in the verified executable mapping. `verify_a029.py` passed **29/29** (27 original instruction/dispatch checks plus two mathematical sanity checks).

## A-028R live validation (source: Codex branch `research/a028-gnm-shader-metadata-20261123` @ `51051db3`)

- Real stage set: **248**, including 110 VS, 128 PS, 8 CS, 1 ES and 1 GS; 1,664 GNM `GnmInputUsageSlot` declarations; no unknown types, truncation or drops in the title-screen/loading scenario. Exact `(shader_hash, stage)` set reproduced twice.
- The original input-usage table formula, `info - 4*(chunk_offset_dw + declaration_count)`, parsed **1664/1664** known kinds, substantially exceeding three tested alternative offsets. Three parsers agreed across 248 stages, including our standalone A-028 parser, without retrofitting it to the live samples.
- `OrbShdr` trailer version 7 in 248/248; `is_srt` **false in 248/248**; trailer `is_extended_usage_info` **true in 243/248**. `PTR_EXTENDEDUSERDATA` (usage kind `0x1B`) appears in **119 declarations**. These are *different predicates* and cannot be substituted for one another.
- Nonzero declarations for `PTR_RESOURCETABLE` (67), `PTR_SAMPLERTABLE` (65), and `PTR_CONSTBUFFERTABLE` (64) exist even though all `is_srt` trailer flags are false. **GNM pointer-table declarations are not equivalent to enabling the separate SRT trailer bit.**
- No DispatchDraw input kinds `0x22–0x25` in the observed 1,664 declarations. This is a bounded absence in title/loading, not a game-wide absence.
- The two previously named SC-09 hashes have **swapped stage prose labels**. `82c730ebc710047e` is PS, `82c730eb4a0404ed` is VS (A-028R capture and emulator CompileModule logs). No evidence that SC-09 effective descriptor measurements themselves are wrong.

## Original-code extended-user-data path

The same decoder from A-025–A-027 at `0x02F5D290` reads the incoming stage-record pointer in `RDX`, extracts **`[RDX] & 0x7FFFFF`** as the *stage program-size field* (`0x02F5D2C5`, `0x02F5D2D3`), and separately reconstructs the program address from `[RDX+8]` and `[RDX+0xC]`. This field is **not** `OrbShdr.length`, which is read from the trailer and measures compiled-code bytes in the validated A-028R sample. A-028R has not yet read `[RDX]` at runtime, so exact byte-for-byte stage-size equality on those 248 samples remains a proposed follow-up.

First-pass dispatch has distinct typed destinations:

| GNM usage | Jump target | Decoded destination in metadata object | Source byte |
| --- | --- | --- | --- |
| `0x02` immediate constant buffer | `0x02F5D44E` | `+0xC2 + 2*apislot` (u16) | mapping from `startregister`, encoded with path bits |
| `0x12` fetch shader subpointer | `0x02F5D51E` | `+0x04` (u8) | `startregister` |
| `0x1A` internal global table pointer | `0x02F5D52F` | `+0x0C` (u8) | `startregister` |
| **`0x1B` extended user-data pointer** | **`0x02F5D540`** | **`+0x07` (u8)** | **`startregister`** |

These destinations are confirmed both by the first dispatch table at `0x035C9CFC` and by original instruction stores, including `0x02F5D540: mov byte ptr [rdi+0x07], bl` and `0x02F5D46B: mov word ptr [rdi+r15*2+0xC2],r9w`. The later second-pass table handles pointer-to-resource-table variants `0x13..0x19`; it is **not** a substitute for the `0x1B` extended user-data pointer.

## Native Vulkan design consequences

1. Preserve **three independent shader properties**: (a) binary trailer `has_extended_usage_info`, (b) declaration(s) of type `PTR_EXTENDEDUSERDATA` with their `startregister`, and (c) logical resource declarations with independent `usagetype/apislot/startregister/flags` records. A parser output that collapses (a) and (b) into one boolean will lose information for real Déraciné shaders.
2. Distinguish hardware **16 user-data DWORDs per stage** from an implementation's *flattened descriptor/extended-user-data address space*. For example, the current shadPS4 implementation defines `Shader::NUM_USER_DATA_REGS = 16` but has a `Shader::Info::flattened_ud_buf` and a population/walker step. A-028R's GNM declaration `startregister` spans **0–60**, and the observed sample includes `IMM_CONSTBUFFER` indices above 15; therefore a native renderer cannot blindly index `user_data[startregister]` for every resource. The precise relationship between *GNM* extended-data declaration and the flattened GPU-visible descriptor layout is **still to be reconstructed**, not automatically borrowed from shadPS4.
3. Maintain stage identifiers as separate domains: original GCN register-bank index (A-022 mapping `CS,PS,VS,GS,ES,HS,LS`), original metadata-decoder mode (six constructor cases `0..5`), shadPS4 `Shader::Stage` index (`PS,VS,GS,ES,HS,LS,CS`), and `OrbShdr.shader_type` (observed `0=PS,1=VS,2=ES,4=CS,5=GS`). The numeric value `0` does **not** mean the same stage in every interface.
4. Distinguish `api_slot` (logical GNM binding), `startregister` (declaration register index), `sharp_idx` (shadPS4 flattened resource-descriptor DWORD offset), and unified Vulkan binding number. A-028R found **14 non-monotonic** shader slot maps and **17 with gaps**, so neither `startregister = 4*api_slot` nor `VulkanBinding = api_slot` is a safe general rule.
5. **Preserve native backend independence**: the standalone GNM parser may use shader-memory bytes as input, but native draw/resource translation must not depend on `Rasterizer::Draw`, `AmdGpu::Regs`, or shadPS4 `TextureCache`/`BufferCache` as stable runtime interfaces. shadPS4 remains a valuable oracle/transition environment.

## Remaining work / bounded follow-up

- Codex A-028R can validate the original stage-size field by recording the source value `[stage_record] & 0x7FFFFF` alongside `trailer_address - code_address + 28` for the same 248 hashes, without dumping shader code.
- Obtain a **metadata-only** complete per-shader `(stage,hash,usage records)` export in a private local analyzer, correlate `0x1B` and trailer bit 2 per stage and inspect patterns that require extended descriptor fetching. The aggregate numbers above establish non-equivalence, not which 119 shader instances carry the pointer.
- Follow the original decoder's `+0x07` pointer usage and stage-mapping paths to reconstruct actual descriptor indirection for a chosen VS/PS shader. One verified `api_slot -> startregister -> source descriptor -> Vulkan resource` chain will be more valuable than additional global histogramming.

No runtime mapping, direct native Vulkan execution, game-wide DispatchDraw absence or precise UE4 resource class identity is claimed.