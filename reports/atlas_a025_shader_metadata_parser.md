# Independent Binary Atlas A-025 — Shader metadata decoder produces the binding maps

**Static original SELF finding only.** Déraciné 1.00 SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. All addresses inner ELF virtual addresses; original SELF text offset VA+`0x1FD30`. Independent offline hash-gated verifier passed **97/97 original-byte/dispatch/caller checks**. No gameplay execution asserted.

## Result

The previously missing constructor is the decoder **`0x02F5D290`**. It initializes a shader-associated metadata buffer to 0xFF, traverses variable-length compact records of four bytes, and dispatches record kinds via a 38-entry signed-relative jump table at `0x035C9CFC`.

**Record kind `0x02`** dispatches to `0x02F5D44E`, whose instruction `0x02F5D46B` stores a 16-bit slot map entry at **`[metadata+0xC2+2*logical_slot]`**. The map is the same one A-022/A-024's shader-user-data writer reads at `0x02F624B2`; absent slots remain initialized to `0xFFFF`.

Record layout inferred from directly decoded accesses at `0x02F5D3B0..0x02F5D3FA`:

| Field in four-byte record | Role |
|---|---|
| byte 0 (loaded at record-3) | kind, valid dispatch cases 0..0x25 |
| byte 1 (record-2) | logical slot |
| byte 2 (record-1) | encoded register index |
| byte 3 (record+0) | flags |

For **kind 2** only, the register map value is computed as:
```
encoded = (index < 16) ? (0x8000 | index) : (index - 16)
metadata.map[slot] = encoded;    // +0xC2 + slot*2
```
Verified by `0x02F5D3BC, 0x02F5D45A..0x02F5D46B`. Bit 15 is later used to distinguish direct GCN register emission from cached/shadow register updates. A different **record kind 3** branches to `0x02F5D482` and writes another map at `metadata+0xEA` (`0x02F5D49F`), so don't assume every record describes a shader-user-data binding.

## Six source-level decode call paths

All six original direct calls target `0x02F5D290` (validated E8 rel32):

| Call | Decoder mode argument | Metadata destination |
|---|---:|---|
| `0x0CF5192` | 0 | `shader_resource+0xB0` |
| `0x0CF1D75` | 1 | `shader_resource+0xB0` |
| `0x0CF0297` | 2 | `shader_resource+0xB0` |
| `0x0CF4807` | 3 | `shader_resource+0xB0` |
| **`0x0CF0124`** | **4** | **`shader_resource+0x268`** |
| `0x0CF2B65` | 5 | `shader_resource+0xB0` |

For mode 4, `0x0CEFCF3` saves the resource in `r10`, `0x0CEFD07` advances that pointer by `0x268`, and `0x0CF0103` passes it as the decoder destination. This explains A-024's special stage-map pointer at `shader_resource+0x268`. The mode arguments are **decoder modes**, not automatically the same enumeration as the seven GCN stage register banks. No stage-6 constructor or indirect caller is claimed absent.

The parser source is shader-associated program data, passed as `rdx`. It reconstructs an encoded source pointer from `[rdx], [rdx+8], [rdx+0xC]`, reads two header bytes preceding the encoded stream and iterates four-byte records. The source-level shader format name is not yet proven.

## Handoff to SC-series

This establishes provenance from **packed shader metadata → decoded per-slot map → effective shader user-data register/sharp_idx**. Codex's SC-09 observed nine resolved register offsets; that is an observed subset, not a cap implied by the parser.

To correlate without disturbing working replay: snapshot the actual stage-indexed map pointer, logical slot, `u16 map[slot]`, mapped low 14-bit register index, high-bit path, and resulting `sharp_idx`. The metadata may be at resource `+0xB0` or `+0x268`, or at a writer-owned scratch map (A-024). Do **not** conflate `context+0x468` (RHI shader-state pointer) with `graphics_state+0xC468` (stage-index-zero map pointer). These use different base objects.

## Limits and next work

Kind 2 and kind 3 outputs are verified, but the remaining 36 record kinds, exact metadata schema, and runtime use are unresolved. This does not by itself identify texture/sampler/buffer classes. Next: recover a bounded classification of the other record kinds or prove their layout via consumers rather than inventing material semantics.

Full independent original-SELF report and 97-check verifier were tested locally; for the GitHub branch verifier see `tools/atlas/verify_independent_a025.py`. No copyrighted binary data is committed.