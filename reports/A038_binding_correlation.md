# A-038 — Runtime shader-to-resource binding correlation

**Result: one complete, causally matched binding chain is proven on genuine Déraciné execution.**

A real shader declaration selects a real guest resource object, whose provider produces descriptor
bytes that a verified original binding writer consumes. The chain is closed end to end and
reproducible.

## Scope note: A-035 through A-037 are not available

The task named A-029 through A-037. **A-035, A-036 and A-037 do not exist in the repository.** Only
A-029 through A-034 are present on `research/atlas-independent-userdata-a022-20261123`. I searched
every remote branch and the local filesystem. The fields the task attributed to A-035
(`object+0x40` timestamp writeback, `+0x38` provider) and A-036/A-037 (constructor families,
deferred-command provenance) are therefore **not** used as established fact here. Where this report
touches those areas it says so explicitly and treats them as unverified.

## Provenance

| Item | Value |
| --- | --- |
| Branch | `research/a038-binding-correlation-20261123` |
| Base | `research/sc09-shader-params-20261123` @ `d6c09a93` |
| Executable | Déraciné 1.00 SELF, 81,506,767 bytes |
| SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Local path | `D:\Temp\Deracine\1.0\Image0\eboot.bin` |
| Runtime | shadPS4 fork, headless, `SHADPS4_VR=1 SHADPS4_OPENXR=0` |
| Scene | title screen, 90 s bounded run, clean exit |
| Capture | `a038-bind-8` |

**A path correction worth recording.** A-028R's runner used
`D:\Temp\Deracine\CUSA11919-qual\eboot.bin`, which hashes to
`8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` — **not** the approved 1.00
image. The approved image is at `D:\Temp\Deracine\1.0\Image0\eboot.bin`. The A-028R shader
metadata is unaffected (it is read from live guest memory, not the file), but any future static
work must use the `1.0\Image0` path.

## The proven chain

```
PS/VS shader hash d5050ca2fb2370b9
  ↓  OrbShdr trailer at 0x20000686a8, declaration table parsed
Original GNM declaration: IMM_CONSTBUFFER, api_slot 0, start_register 4, flags 0
  ↓  decoded by the original decoder's own rule
Decoded map word 0x8004  (direct emission, destination index 4)
  ↓  read by the original writer from map_object+0xC2+2*slot
Typed writer 0x02F62440 (16-byte immediate constant-buffer writer)
  ↓  source = stack buffer filled by the deferred-upload provider
Descriptor bytes by value: 00c8810e 20001000 03000000 ac7f0720
  ↓  direct path: SET_SH_REG packet at the writer's cursor
Destination: writer packet cursor 0x10046a0000, register offset from mapped index 4
```

Every arrow is an observed value, not an inference.

## What was captured

| Item | Value |
| --- | --- |
| Writer invocations observed | 4,983,755 |
| Records kept (bounded) | 256 |
| Records that resolved a shader hash | 247 |
| Records that matched a declaration | 247 |
| Map words equal to encoder(declaration) | **247 / 247** |
| Distinct shaders in the resolved set | 3 |
| Distinct `(hash, slot)` chains | 4 |
| Descriptor width | 16 bytes on all |

The three resolved shaders are `ad15cab1f19120ca`, `bac57337904948a7` and `d5050ca2fb2370b9`. All
three appear in the A-028R metadata sample, so the shader identity is corroborated by an
independent capture.

## The four verified chains

| Shader hash | Writer stage | Slot | Declaration | Map word | Mode | Index |
| --- | ---: | ---: | --- | --- | --- | ---: |
| `ad15cab1f19120ca` | 2 | 0 | kind 2, reg 4, flags 0 | `0x8004` | direct | 4 |
| `bac57337904948a7` | 2 | 0 | kind 2, reg 16, flags 0 | `0x0000` | shadow | 0 |
| `bac57337904948a7` | 2 | 1 | kind 2, reg 20, flags 0 | `0x0004` | shadow | 4 |
| `d5050ca2fb2370b9` | 2 | 0 | kind 2, reg 4, flags 0 | `0x8004` | direct | 4 |

The map word is not copied from the declaration. It is read from the writer's own stage map object
at `map_object + 0xC2 + 2*slot`, and it independently equals the encoder applied to the parsed
declaration on all 247 records. That is the join between the shader's metadata and the writer's
destination selection.

## The stage-number domain is not the shader's logical stage

**This is the most important negative finding.** The writer's stage index is 2 for every resolved
record, yet all three shaders are **vertex** shaders in the A-028R metadata. The writer's stage
index and the shader's logical stage are different domains, exactly as A-030 warned.

A native adapter must not map writer stage 2 to "pixel shader". The mapping has to be recovered
from the stage record, not from the writer's argument.

## How the shader hash was resolved

The first attempt failed on all 4.98 million calls. The shader resource's program-data pointer at
`+0x1D8` does **not** point at shader code. It points at the original **stage record**, whose
layout is the one the original decoder reads (Atlas A-027):

```
stage_record[0] & 0x7FFFFF   stage size in bytes
stage_record[2] << 8         program address
trailer = program + size - 0x1C
```

Two observed stage records decode exactly onto A-028R's independently captured trailers:

| Stage record | Size | Program | Trailer computed | A-028R trailer | Match |
| --- | ---: | --- | --- | --- | --- |
| `0x100440d434` | 196 | `0x2000068600` | `0x20000686a8` | `0x20000686a8` | yes |
| `0x1005082664` | 492 | `0x20000a5400` | `0x20000a55d0` | `0x20000a55d0` | yes |

### Secondary housekeeping: the stage-size field is now verified

A-028R inferred `program_size = trailer_offset + 28` without reading the original stage record.
This phase reads it. The stage record's 23-bit size field equals `trailer_offset + 28` on both
samples, which is exactly the value the original decoder's `program + size - 0x1C` requires.

**The inference is confirmed, and it is a different number from `bininfo_length`.** On the first
sample, size is 196 and `bininfo_length` is 116. They are not interchangeable.

## The descriptor source

The writer is reached from the deferred-upload caller at `0x0CC0D50`, not from a grouped binder.
That caller:

1. reads `context+0x468` (the shader state) and tests a validity byte at `+0x1D4`;
2. takes the upload object from `context+0xAE0`;
3. calls the provider at `0x0CC1010` with the upload object, the context, a mode byte and a stack
   buffer;
4. calls the 16-byte constant-buffer writer with `source = that stack buffer`.

The provider at `0x0CC1010` copies `upload_object+0x48` for `upload_object+0x50` bytes into the
caller's stack buffer, with a 0x40-byte cap and a 0x10-byte minimum. The descriptor bytes this
report records are read at the writer boundary, after that copy, by value.

**This is a different mechanism from the grouped binder.** The grouped binder at `0x0CC0070` was
also hooked and fired 1,760,640 times, but its `pending = dirty_mask & group_mask` was **zero on
every single call**. It never dispatches a writer in this scene. That is a real, bounded negative
result: the grouped-binding path is not the one that produces these descriptors on the title
screen.

## What is observed, derived, and unavailable

| Field | Status | Basis |
| --- | --- | --- |
| Writer entry arguments | **observed** | register frame at the original entry |
| Map object and decoded word | **observed** | read from the writer's own stage map |
| Descriptor bytes | **observed** | copied by value at the writer boundary |
| Shader resource | **derived** | `map_object - 0xB0` (A-025) |
| Program data / stage record | **observed** | read at `shader_resource+0x1D8` |
| Trailer and shader hash | **observed** | read at the computed trailer address |
| Declaration | **observed** | parsed from the trailer's own table |
| Upload object | **observed** | read at `context+0xAE0` |
| Upload size | **observed** | read at `upload_object+0x50` |
| Virtual provider target | **unavailable** | see below |
| Timestamp writeback | **unavailable** | A-035 absent; not investigated |
| Reference-count effects | **unavailable** | not instrumented |

## The missing edge: the virtual provider

The task asked for the virtual `+0x38` provider target. **This chain does not use it.** The
descriptor here comes from the deferred-upload provider at `0x0CC1010`, which is a direct call, not
a virtual dispatch. The `+0x38` path belongs to the grouped binder's `+0x248` list, and that
binder never dispatches in this scene.

So the chain is complete for the deferred-upload family, and the virtual-provider family is
**not** proven. I am not claiming the `+0x38` edge. It remains the next target, and it needs a
scene where the grouped binder's dirty mask is non-zero.

## What is not claimed

- **No one-to-one correspondence with Vulkan descriptors.** The 16-byte GNM descriptor is not a
  Vulkan binding. No comparison against shadPS4's resolved descriptors was made.
- **No claim that every declaration produces a writer call.** The grouped binder's zero pending
  mask shows the opposite is possible.
- **No claim about the descriptor's resource class.** The bytes are recorded; whether they are a
  V#, T# or buffer descriptor is not established here.
- **No claim about `+0x228`/`+0x238`.** A-033's unclassified lists were not touched.
- **No claim about the upload object's ownership or lifetime.** Only its address and size are
  recorded.

## Instrumentation

Default OFF. With `SHADPS4_DERACINE_GNM_BINDING` unset, absent or anything but `1`, nothing is
patched and no guest memory is read.

- Three guest entry observers: the 16-byte constant-buffer writer (`0x02F62440`), the grouped
  binder (`0x0CC0070`), and the deferred-upload caller (`0x0CC0D50`).
- Each verifies the image identity, the target's executable VMA, and the exact 16-byte prologue
  before patching. A mismatch refuses the install and disables the capture.
- Bounded at 256 records. No allocation in the hot path beyond one small vector per new record.
- Descriptor bytes are copied by value at the writer boundary, never re-read afterwards.
- No guest detours beyond the entry patches, no GPU readbacks, no renderer change.

## Reproducible checks

```
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_TITLE_EYE_WIDTH='2880'; $env:SHADPS4_TITLE_RESOLUTION='6'
$env:SHADPS4_DBG_AUTOPRESS='1'; $env:SHADPS4_DBG_AUTOPRESS_TO='60000'
$env:SHADPS4_DBG_AUTOPRESS_MENU='1'
$env:SHADPS4_DERACINE_GNM_BINDING='1'
$env:SHADPS4_EXIT_AFTER='90'; $env:SHADPS4_CLEAN_EXIT='1'
shadPS4.exe -g D:\Temp\Deracine\1.0\Image0\eboot.bin
```

Then `user/gnm_binding/gnm_binding_chain.json`, and:

```
python tools/atlas/verify_a038_binding_chain.py <chain.json> [<a028r_metadata.json>]
```

**12/12 checks pass.**

## Remaining uncertainties

1. **The virtual `+0x38` provider is unproven.** The grouped binder never dispatches here.
2. **Only constant-buffer declarations are covered.** Resource, sampler and vertex-buffer writers
   were not exercised in this scene.
3. **The upload object's identity is an address, not a type.** Its vtable and ownership are not
   established.
4. **The descriptor's resource class is not decoded.** The bytes are captured; their meaning is not.
5. **A-035 through A-037 are absent**, so the timestamp writeback and constructor families the task
   referenced could not be consulted.
6. **One scene only.** The title screen. Gameplay may exercise different binding families.
