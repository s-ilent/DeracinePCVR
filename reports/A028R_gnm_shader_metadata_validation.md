# A-028R — GNM shader metadata validated against genuine Déraciné shaders

**Result: the A-025–A-027 decoder findings are confirmed on real shader instances.** 248 distinct
Déraciné shader stages were captured from live guest GPU memory, their `OrbShdr` trailers and
complete declaration tables parsed, and the results cross-checked by three independent
implementations. Every one of the 1,664 declarations uses a known GNM usage type.

This is the first validation of the Atlas shader-metadata work against actual game shaders. A-028
recorded that no genuine sample existed; this phase supplies one.

## What was captured

One headless title-screen run, default-OFF collector enabled:

| Item | Value |
| --- | --- |
| Distinct shader stages | 248 |
| Vertex (VS) | 110 |
| Pixel (PS) | 128 |
| Compute (CS) | 8 |
| Export (ES) | 1 |
| Geometry (GS) | 1 |
| Declarations parsed | 1,664 |
| Unknown usage types | 0 |
| Dropped (record bound) | 0 |
| Truncated tables | 0 |
| Trailers outside the compiled span | 248 of 248 |

Two independent runs produced an **identical** set of 248 `(hash, stage)` pairs, so the sample is
deterministic rather than a lucky window.

## The trailer is real and its bounds hold

Every record was read only after `MemoryManager::IsValidMapping` accepted the whole
table-plus-trailer span, and every trailer carries the literal `OrbShdr` signature.

| Field | Observed |
| --- | --- |
| Signature | `OrbShdr` on 248/248 |
| Version | 7 on 248/248 |
| `shader_type` | 0=PS(128), 1=VS(110), 4=CS(8), 2=ES(1), 5=GS(1) |
| `flags` | 20 (243), 16 (5) |
| `is_srt` | false on 248/248 |
| `is_extended_usage_info` | true on 243/248 |
| `chunk_usage_base_offset_in_dw` | 2 (94), 3 (87), 6 (38), 5 (25), 4 (4) |
| `num_input_usage_slots` | 0–14, mean 9.29 (VS) / 4.73 (PS) |

`shader_type` agrees with the stage the renderer compiled the shader for on all 248 records, which
is an independent check that the trailer belongs to the program it was found behind.

## The declaration table is where A-027 says it is

A-027 derived `slots = info - 4 * (chunk_usage_base_offset_in_dw + num_input_usage_slots)` from the
original decoder's machine code. That formula was scored against three plausible alternatives on the
real sample, counting how many declarations land on a known GNM usage type:

| Table location | Known usage types | Total |
| --- | --- | --- |
| **`info - 4*(chunk + count)` (A-027)** | **1,664** | **1,664** |
| `info - 4*count` | 1,277 | 1,664 |
| `code_end + 4*chunk` | 932 | 1,664 |
| `code_end` | 927 | 1,664 |

The A-027 formula is the only one that is right for every declaration. The alternatives produce
byte values that are not usage types, which is what a wrong offset looks like.

## `bininfo_length` is not the stage-size field

A-028 warned that the trailer's own length field and the original decoder's stage-size field must
not be conflated. The real sample settles it:

- `bininfo_length` is the length of the **compiled code**, and the trailer sits *after* it.
- `trailer_offset_in_code + 28` (recorded as `program_size`) is the candidate for the original
  stage-size field, since `program + size_field - 0x1C` must equal the trailer.
- `program_size > bininfo_length` on **248/248** records, and the trailer is inside
  `[code, code + bininfo_length)` on **0/248**.

So `GetParams()`'s `std::span{code, bininfo.length / sizeof(u32)}` excludes the trailer and its
declaration table. That is exactly the gap A-028 predicted: an ordinary `DumpShader` output cannot
be fed to the parser, which is why this collector reads the trailer at its own address instead.

The gap between the code end and the table start is not constant (0 to 100 bytes across the sample),
so the table cannot be found by assuming a fixed layout. It has to be located from the trailer's own
fields, as the original decoder does.

## Independent reconstruction

Three implementations agree on all 248 shaders, byte for byte:

1. The C++ collector's `Parse()`, which is what produced the export.
2. A standalone Python re-parse of the private raw spans using only the A-027 formula: **248/248
   exact**, 0 mismatches, 0 bad signatures, 0 bad bounds.
3. The Atlas's own `parse_gnm_usage_a028.py` run over the same spans: **248/248 agreement**, 0
   mismatches, 0 failures.

The third is the important one: the Atlas parser was written independently, against synthetic
fixtures only, and had never seen a real Déraciné shader. It parses these correctly without
modification.

## Usage-type histogram

| Usage type | Total | VS | PS |
| --- | ---: | ---: | ---: |
| `IMM_CONSTBUFFER` (0x02) | 550 | 408 | 131 |
| `IMM_SAMPLER` (0x01) | 312 | 142 | 170 |
| `IMM_RESOURCE` (0x00) | 255 | 162 | 87 |
| `PTR_EXTENDEDUSERDATA` (0x1B) | 119 | 88 | 27 |
| `SUBPTR_FETCHSHADER` (0x12) | 109 | 108 | 0 |
| `PTR_VERTEXBUFFERTABLE` (0x17) | 109 | 108 | 0 |
| `PTR_RESOURCETABLE` (0x13) | 67 | 2 | 65 |
| `PTR_SAMPLERTABLE` (0x15) | 65 | 2 | 63 |
| `PTR_CONSTBUFFERTABLE` (0x16) | 64 | 2 | 62 |
| `IMM_RWRESOURCE` (0x04) | 12 | 0 | 0 (all CS) |
| `PTR_INTERNALGLOBALTABLE` (0x1A) | 2 | 0 | 0 (GS/ES) |

The stage split is a strong internal consistency check. Vertex shaders declare the fetch shader and
the vertex-buffer table; pixel shaders declare the resource, sampler and constant-buffer pointer
tables; compute declares the only RW resources. That is what the GNM model predicts, and it is not
something the collector imposed.

## DispatchDraw usage types 0x22–0x25 do not occur

**Zero** declarations of types `0x22`–`0x25` in 1,664 declarations across 248 shaders.

This is a bounded negative result. It says these types are absent from this sample — the title
screen and its loading path — not that Déraciné never uses them. DispatchDraw is a compute-driven
geometry path, and this sample's compute shaders are the eight that the title screen runs. A
gameplay capture would be needed to speak to the rest of the game.

## Correlation with SC-09

SC-09 reported two shader hashes with stage labels. Both appear in this capture, and **the stage
labels in the SC-09 report are swapped**:

| Hash | SC-09 report | This capture | Emulator's own compile log |
| --- | --- | --- | --- |
| `82c730ebc710047e` | VS | **PS** | `Compiling fs shader 0x82c730ebc710047e` |
| `82c730eb4a0404ed` | PS | **VS** | `Compiling vs shader 0x82c730eb4a0404ed` |

The emulator's own `CompileModule` log line is the tiebreaker and agrees with this capture. The
SC-09 report's prose labelled the two user-data dumps the wrong way round; its measurements are
unaffected, but the labels are wrong and should be corrected there.

The declarations for those two shaders:

```
82c730ebc710047e  PS
  IMM_RESOURCE        api_slot 0  start_register  0  flags 3
  IMM_SAMPLER         api_slot 0  start_register  8  flags 0
  PTR_EXTENDEDUSERDATA api_slot 1 start_register 12  flags 0
  IMM_RESOURCE        api_slot 1  start_register 16  flags 3
  IMM_SAMPLER         api_slot 1  start_register 24  flags 0
  IMM_CONSTBUFFER     api_slot 0  start_register 28  flags 0
  IMM_CONSTBUFFER     api_slot 1  start_register 32  flags 0

82c730eb4a0404ed  VS
  SUBPTR_FETCHSHADER      api_slot 0  start_register 0  flags 0
  PTR_VERTEXBUFFERTABLE   api_slot 0  start_register 2  flags 0
  IMM_CONSTBUFFER         api_slot 0  start_register 4  flags 0
  IMM_CONSTBUFFER         api_slot 1  start_register 8  flags 0
```

### GNM logical slots versus SC-09's resolved `sharp_idx`

These are **not** the same number, and the indirection is visible in the data.

- GNM `api_slot` is the logical constant-buffer slot, 0–8 in this sample.
- GNM `start_register` is the user-data register the shader reads that slot from, always a multiple
  of 4, spanning 0–60.
- SC-09's `sharp_idx` is the resolved dword index into the flattened user-data buffer, and it took
  exactly nine values: 0, 4, 8, 12, 16, 20, 24, 28, 32.

The capture shows why SC-09 saw only nine values: it sampled 552 bindings from a bounded window.
This sample's `start_register` values cover 0–60, so the nine values are a subset of the real range,
not a cap. SC-09's own report already said this ("an observed subset, not a cap implied by the
parser"), and the wider sample confirms it.

The indirection is directly observable. `api_slot → start_register` is a function within every
shader (248/248, zero violations), but it is **not** the identity and **not** always monotonic:

- 14 shaders map a higher `api_slot` to a *lower* register, e.g. `(6→4), (7→0)`.
- 17 shaders skip a register, e.g. `(0→4), (1→8), (2→16), (3→20)` — slot 2 starts at 16, not 12.

So a consumer cannot compute `sharp_idx` from `api_slot` by multiplying by four. The A-023 map is
the indirection, and this capture observes its output. That is consistent with A-023 and with
SC-09's conclusion, now on a wider sample.

**No one-to-one correspondence is claimed.** A GNM `IMM_CONSTBUFFER` declaration is not a Vulkan
uniform buffer. SC-09 already established that its 552 bindings all classify as storage and
read-const because `uniform_buffer_limit` is 0. The GNM declaration says which register a slot
occupies; the renderer decides what descriptor goes there. The two are related through the
indirection, not equal.

## What is format validity, what is game usage, what is renderer interpretation

Kept separate deliberately:

- **Format validity** — the trailer signature, version, field ranges and table bounds. Verified on
  248/248 by three implementations.
- **Original-game usage** — which usage types Déraciné actually declares, and how often. Measured:
  the histogram above. The absence of 0x22–0x25 is a usage fact about this sample.
- **Renderer interpretation** — what shadPS4 does with those declarations. Not measured here. This
  phase adds no binding logic and changes no renderer behaviour.

## The collector

Default OFF. With `SHADPS4_DERACINE_GNM_SHADER` unset, absent or anything but `1`, no record is
produced and no guest memory is read.

- Observation point: `vk_pipeline_cache.cpp`, immediately after `AmdGpu::GetParams` has located the
  trailer, where both the program address and the trailer address are known. A new
  `SearchBinaryInfoPtr` returns the trailer's address; `SearchBinaryInfo` keeps its existing
  contract and now delegates to it.
- Bounds: the trailer is read first, its own fields decide the table size, and the whole
  table-plus-trailer span is validated with `MemoryManager::IsValidMapping` before any byte is
  copied. A span that is not fully mapped is skipped, not partially read.
- Bounded: 512 distinct `(hash, stage)` records, 64 declarations per shader, no allocation in the
  hot path beyond one small vector per new shader.
- Once per distinct `(hash, stage)`: a permutation recompile is the same metadata.
- Export: metadata JSON to `user/gnm_shader/`, from `Emulator::Shutdown` so a clean quit still
  reports it.
- Raw spans: written only under `SHADPS4_DERACINE_GNM_SHADER_RAW=1`, to a separate directory, and
  never committed. They contain original shader bytes.

No guest detours, no GPU readbacks, no renderer behaviour change.

## Architectural constraint

The parser is header-only and pure: `gnm_shader_metadata_parse.h` reads a `std::span<const u8>` and
nothing else. It has no dependency on shadPS4's GPU emulator, register state, resource caches or
Vulkan renderer, and the tests exercise it with no emulator, no GPU and no guest memory manager.

That is the shape the native port needs: given the bytes of a shader program, locate the trailer,
parse the declarations, and know nothing about how they were obtained. The collector in
`gnm_shader_metadata.cpp` is the shadPS4-specific half and is deliberately separate.

## Reproducible provenance

```
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_TITLE_EYE_WIDTH='2880'; $env:SHADPS4_TITLE_RESOLUTION='6'
$env:SHADPS4_DBG_AUTOPRESS='1'; $env:SHADPS4_DBG_AUTOPRESS_TO='60000'
$env:SHADPS4_DBG_AUTOPRESS_MENU='1'
$env:SHADPS4_DERACINE_GNM_SHADER='1'
$env:SHADPS4_DERACINE_GNM_SHADER_RAW='1'
$env:SHADPS4_EXIT_AFTER='90'; $env:SHADPS4_CLEAN_EXIT='1'
shadPS4.exe -g <eboot.bin>
```

Then `user/gnm_shader/gnm_shader_metadata.json` and `user/gnm_shader/*.bin`.

## Tests

12 tests in `src/tests/core/test_gnm_shader_metadata.cpp`, all passing. They pin the A-027 table
arithmetic, the fail-closed bounds (missing signature, truncated trailer, table outside the span),
the record bound, unknown-type preservation, and that `bininfo_length` is not the table offset.

## Remaining uncertainties

- **Gameplay is not covered.** This sample is the title screen and its loading path. The usage
  histogram and the 0x22–0x25 absence describe that sample only.
- **The stage-size field is inferred, not proven.** `program_size = trailer_offset + 28` is
  consistent with the original decoder's `program + size_field - 0x1C`, but this phase did not read
  the original stage record to confirm the field's value equals it.
- **`flags` semantics are not decoded.** 20 and 16 are observed; only bits 0 (`is_srt`) and 2
  (`is_extended_usage_info`) are named, following the existing `BinaryInfo` layout.
- **`chunk_usage_base_offset_in_dw` is used, not explained.** Its role in the table address is
  confirmed; what the chunk data it counts actually contains is not.
- **No renderer correlation was attempted.** This phase deliberately does not claim that a GNM
  declaration explains a specific Vulkan descriptor.
