# Independent Binary Atlas A-026 — shader metadata records are PS4 GNM InputUsageSlots

**Static original-target finding.** Original Déraciné 1.00 SELF (81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`). The hash-gated local verifier passed **90/90 checks** on the uploaded original SELF. No runtime reachability claim, and no proprietary bytes committed. Inner ELF VAs throughout; original file offset = VA + `0x1FD30` in the verified text segment.

## Identification: a standard PS4 platform record, not a unique Déraciné format

A-025's four-byte records at parser `0x02F5D290` match the PS4 `GnmInputUsageSlot` described in [OpenGNM's header](https://github.com/PS4-OpenGNM/opengnm/blob/main/include/gnm_shader.h):

| Original record byte | PS4 GNM meaning | Decoder read |
|---|---|---|
| 0 | `usagetype` | `0x02F5D3B6` |
| 1 | `apislot` | `0x02F5D3E4` |
| 2 | `startregister` | `0x02F5D3B0` |
| 3 | packed flags or SRT size | `0x02F5D3DA` |

First-pass parser dispatch at `0x035C9CFC` accepts 38 kinds `0x00..0x25`; all signed-relative jump entries were checked against original code.

| GNM usage type | Original first-pass handler | Output metadata slot map |
|---|---|---|
| `0x00 IMM_RESOURCE` | `0x02F5D3FF` | `+0x22+2*apislot` |
| `0x01 IMM_SAMPLER` | `0x02F5D418` | `+0xA2+2*apislot` |
| **`0x02 IMM_CONSTBUFFER`** | `0x02F5D44E` | **`+0xC2+2*apislot`** |
| `0x03 IMM_VERTEXBUFFER` | `0x02F5D482` | `+0xEA+2*apislot` |
| `0x04 IMM_RWRESOURCE` | `0x02F5D4B6` | `+0x82+2*apislot` |

**Correction to A-022/A-025 interpretation:** the user-data binding map at `+0xC2` is specifically the PS4 **immediate constant-buffer** map, not a generic map for every shader resource type. This identifies GNM-level binding categories, but not necessarily source-level UE4 `FUniformBuffer` objects.

Other first-pass kinds include `0x07` GDS counter, `0x08` GDS memory, `0x0A` shader resource table, `0x0D` LDS ES/GS size, `0x12` fetch shader subpointer, `0x1A` internal global table, and `0x1B` extended user data. Their single-byte metadata destinations were recovered from original instructions. Kinds `0x22..0x25` write metadata bytes `+0x12..+0x15` but remain **unlabeled**; the compared public enum does not identify them. The common first-pass skip handler at `0x02F5D440` is **not** proof of runtime non-use.

## Second pass: pointer-table declarations

The decoder also performs a second pass: original instructions `0x02F5D630..0x02F5D657` subtract `0x13` from the GNM usage type and range-check seven entries in the separate table `0x035C9D94`. All seven dispatch values verified.

| Usage type | GNM meaning | Second-pass target |
|---|---|---|
| `0x13` | resource table pointer | `0x02F5D659` |
| `0x14` | internal resource table pointer | skip `0x02F5E078` |
| `0x15` | sampler table pointer | `0x02F5D707` |
| `0x16` | constant-buffer table pointer | `0x02F5D7C7` |
| `0x17` | vertex-buffer table pointer | `0x02F5D8B7` |
| `0x18` | streamout buffer table pointer | skip `0x02F5E078` |
| `0x19` | RW resource table pointer | `0x02F5D8EB` |

The non-skipped paths record starting registers, flags and auxiliary state. Do not reduce indirect-pointer bindings to immediate four-DWORD descriptors.

## UE4 versus Déraciné versus GNM

- Standard **UE4 4.18**: higher-level shader resource tables and parameter maps, e.g. `FShaderCompilerResourceTable` in [ShaderCore.h](https://github.com/aceyan/UE4.18.3_release/blob/4c9fc5d3a2bf51029aa7d7295e09620d298739ef/Engine/Source/Runtime/ShaderCore/Public/ShaderCore.h). Different type/layout.
- Standard **PS4 GNM**: `GnmInputUsageSlot` four-byte shader input declarations and GPU stage user-data registers. These semantics transfer across PS4 games irrespective of engine.
- **This exact decoder implementation** at `0x02F5D290`: attribution to Epic's PS4 backend, Sony middleware, or Déraciné modification **not established** without a second original executable or matching source. Do not call it unique or byte-identical to other games.

## Use in SC-series

Keep `usagetype`, `apislot`, `startregister`, flags, shader-stage writer, resolved register offset and actual descriptor contents separate. The A-022 map is **immediate constant-buffer**; other categories use independent maps. Parse GNM input-usage declarations to type shader bindings without inferring resource types exclusively from emitted PM4. Respect the A-024 caveat that the effective stage-map pointer may live in writer-owned scratch state.

## Verification and future work

Local `python verify_a026.py <private-original-1.00-eboot.bin>` yielded **90/90**: original image identity; exact opcode anchors; all 38 first dispatches; all 7 second dispatches; map addresses and lengths. GitHub copy is a parallel read-only implementation, not separately executed yet. The full standalone local report and verifier were saved as independent artifacts.

Next: inspect a concrete PS4 shader binary's input-usage declarations to validate flags, resource-table indirection and type-to-descriptor resolution; or decode the higher-numbered kinds where public PS4 headers offer no names.