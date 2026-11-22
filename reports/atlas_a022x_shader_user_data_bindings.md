# Independent A-022x — Deferred resource commands write GCN shader user data

**Scope:** Static original Déraciné PS4 1.00 SELF (81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`). All addresses are **inner ELF VAs**. Original file-backed code offset = VA + `0x1FD30`. No runtime reachability, exact C++ class name, or resource-category claim. Full locally verified report is also preserved independently.

## Result

The two resource-command vtables established in A-021x, `0x03EDFCA0` (Execute **`0x0CACDA0`**) and `0x03EDFCC0` (Execute **`0x0CACF80`**), lead to **shader user-data register binding**. Both source **four 32-bit words (16 bytes) at `resource+0x50`**, use a per-slot mapping to choose between a **CPU-side shadow register update** and a **PM4 `SET_SH_REG` packet**, update separate cached-resource pointer arrays and dirty masks, and release their own retained references before returning.

This is a new architectural bridge between high-level deferred commands and lower-level GNM/PM4 shader resource state. It is directly relevant to the SC-series work on shader user data, but it does *not* establish that the four words are specifically a buffer descriptor, a sampler, or a texture binding.

### Stage register base lookup

The original executable contains a **seven-entry little-endian WORD array** at `0x037530B0`:

| Target stage index | Stage | Full GCN shader user-data base |
|---:|---|---:|
| 0 | CS | `0x2E40` |
| 1 | PS | `0x2C0C` |
| 2 | VS | `0x2C4C` |
| 3 | GS | `0x2C8C` |
| 4 | ES | `0x2CCC` |
| 5 | HS | `0x2D0C` |
| 6 | LS | `0x2D4C` |

At `0x02F62538`, helper `0x02F62440` takes the table address. It selects `WORD[stage]` at `0x02F62543` and computes the packet offset `stage_base + mapped_slot - 0x2C00` at `0x02F62557`. The register labels match publicly documented AMD GFX7 register definitions, notably `SPI_SHADER_USER_DATA_HS_0=0x2D0C`.

### Command A: generic stage-mapped user data

- `0x0CACDAE`: reads graphics context from `command_list+0x20`.
- `0x0CACDD1`: obtains an integer stage selector from `[[cmd+0x10]+0x380]`. Exact source-level enum meaning remains unproved.
- `0x0CACDD7`: loads resource data from `resource+0x50`.
- `0x0CACDF5..0x0CACE0A`: calls **`0x02F62440`**, with writer `[context+0x460]+0x2E8`, stage selector, binding slot `cmd+0x18`, count 1, source pointer.
- `0x0CACE0F..0x0CACE29`: refcounted cached resource pointer at `context+0x568+8*slot`; dirty mask `context+0x928 |= (1 << slot)`.

The generic helper reads per-stage 16-bit slot maps at `+0xC2+2*slot`. **`0xFFFF` is unmapped.** For other values, the low 14 bits are the slot and bit 15 selects the direct-packet path. A nonnegative mapping copies one 16-byte block to a CPU-side register shadow with `vmovups`. A negative mapping builds a PM4 type-3 **`SET_SH_REG` (`0x76`)** packet with four user-data DWORDs, using the stage base above. The helper updates per-stage dirty state even when no payload packet is emitted. Consequently, RHI command count and PM4 packet count cannot be treated as interchangeable.

### Command B: fixed HS user-data bank

- `0x0CACF94`: reads context from `command_list+0x20`; `0x0CACF9C`: nested resource pointer.
- `0x0CACFC1`: reads slot mapping from the table at `[[context+0x460]+0xC490]+0xC2+slot*2`.
- `0x0CACFCA`: sentinel `0xFFFF`; `0x0CACFDC`: retains low 14-bit mapped slot; `0x0CACFE3`: tests sign/high bit.
- **Cached path:** `0x0CAD053` writes 16 bytes into the CPU-side shadow region at `state+0x7B78+4*mapped_slot`.
- **Direct PM4 path:** `0x0CAD0C3` or `0x0CAD144` emits the header `0xC0047600` (type 3, opcode `0x76`, four payload DWORDs); offset `mapped_slot+0x10C`, i.e. full register **`0x2D0C+mapped_slot`**, the HS user-data bank. Four payload DWORDs come from resource `+0x50,+0x54,+0x58,+0x5C`. The writer cursor advances 32 bytes, including its surrounding writer words.
- `0x0CAD17D..0x0CAD197`: updates cached resource at **`context+0x608+8*slot`** and marks **`context+0x92C`** dirty; writer dirty byte at `+0xC53D`.
- `0x0CAD1A6..0x0CAD1D4`: releases retained resource references within Execute.

**Important limit:** A fixed HS register base does not prove that tessellation shaders execute in Déraciné gameplay. No live traces of these commands were obtained.

## Immediate SC-series handoff

Candidate **two extra deferred Execute wrappers**: `0x0CACDA0`, `0x0CACF80`, with vtables `0x03EDFCA0` and `0x03EDFCC0`. They are *not* included in the initial five A5/SC-01 command wrappers. When aligning semantic commands with SC-09 GPU user-data captures, use: binding slot `cmd+0x18`; opaque nested reference `cmd+0x20`; synchronous 16-byte source snapshot `resource+0x50` after verifying source lifetime; stage map sentinel `0xFFFF`, low-14-bit index and sign bit; context cache/dirty fields `0x568/0x928` vs `0x608/0x92C`. The Execute methods release references, so never retain borrowed payload or resource pointers beyond execution.

**Next bounded question:** trace the producer of `[context+0x460]`'s per-stage slot maps and check real runtime reachability. Do not infer a concrete texture/sampler/buffer type without inspecting descriptor contents and/or the matching GNM program state.

## Verification

`python tools/atlas/verify_independent_a022x.py <private-original-1.00-eboot.bin>`

The verifier enforces the original image hash and checks the function bytes, shader-stage register array, PM4 packet encoding, and the two context cache/dirty paths. The fuller local analysis additionally passed 46/46 byte/register checks before upload. No SELF, ELF, shader binaries, game assets or protected logs are committed.

**Public cross-check for register naming:** Linux AMD GFX7 register definitions, e.g. `drivers/gpu/drm/amd/include/asic_reg/gca/gfx_7_0_d.h` (`mmSPI_SHADER_USER_DATA_HS_0 0x2d0c`), and shadPS4 PM4 enum `SetShReg = 0x76`.