# A4 address map

All addresses are inner-ELF virtual addresses from the pinned images. The runtime VA is
`module_base + RVA`, with `module_base = 0x800000000` in the observed session.

## Images

| Image | SHA-256 | Path |
| --- | --- | --- |
| 1.00 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` | `D:\Temp\Deracine\1.0\Image0\eboot.bin` |
| 01.01 | `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` | `D:\Temp\Deracine\CUSA11919-1.01\eboot.bin` |

Both are 81,506,767 bytes. Every address in this map was checked in both images and agrees.

## Segments

| Index | Type | Flags | File offset | Vaddr | File size |
| --- | --- | --- | --- | --- | --- |
| 0 | `PT_LOAD` | 5 (R+X) | `0x00004000` | `0x0` | `0x03E86D30` |
| 1 | `PT_SCE_RELRO` | 4 (R) | `0x03E8C000` | `0x03E88000` | `0x004FAE78` |
| 2 | `PT_LOAD` | 6 (R+W) | `0x04388000` | `0x04384000` | `0x00023058` |

Vtables live in segment 1, which is file-backed but not executable. A scan that only looks at
`PT_LOAD` will miss them.

## Verified at runtime

| Item | Value | Evidence |
| --- | --- | --- |
| Draw method | `0x0CBB9C0` | 128 captured entries |
| Runtime caller | `0x13A68E8` | `caller=` field, all entries |
| Call instruction | `0x13A68E3` | `E8 D8 50 91 FF` |
| RHI thread | 15136 (run 2), 27584 (run 3) | `thread_id=` |
| PM4 decode thread | 25652 | `GpuCommandProcessor` |

## Dispatch chain

| Item | Address | Meaning |
| --- | --- | --- |
| Wrapper entry | `0x13A68B0` | virtual method, reads object fields |
| Direct call to draw | `0x13A68E3` | `E8 rel32` to `0x0CBB9C0` |
| Wrapper table slot | `0x03F26A88` | holds `0x13A68B0` |
| Wrapper vptr store | `0x157F2A7` | `movq %rdx, (%rax)` |
| Context vtable base | `0x03EDFCA0` | holds draw/compute/shader methods |
| Context vptr store | `0x0CACE52` | `movq %rax, (%rbx)` |
| Context vptr load | `0x0CACE4B` | `lea 0x3232E4E(%rip), %rax` |

## Vtable slots

| Slot | Offset | Value | Role |
| --- | --- | --- | --- |
| `0x03EDFCA0` | `+0x000` | `0x0CACDA0` | first entry |
| `0x03EDFDB8` | `+0x118` | `0x0CADC00` | compute dispatch |
| `0x03EDFFE0` | `+0x340` | `0x0CB5130` | shader/pipeline state |
| `0x03EE0190` | `+0x4F0` | `0x0CBB9C0` | indexed draw |
| `0x03EE0918` | `+0xC78` | (exec) | last executable entry found |

The table is populated with plain 8-byte pointers, not relocation addends. Entries appear in
pairs separated by two zero slots.

## Writer object contract

Observed in the shader method `0x0CB5130`:

| Field | Use |
| --- | --- |
| `+0x18` | indirect call target, returns boolean in `%al` |
| `+0x20` | register array, loaded into `%rdx` as argument 3 |

Call shape: `writer->[+0x18](writer, size, writer->[+0x20])`.

Register-array sizes observed: `0x28` (40) for pixel shader, `0x1D` (29) for vertex shader.

## GNM stubs reached by the shader method

| Stub | Calls | Name |
| --- | --- | --- |
| `0x33C81B0` | 2 | `sceGnmSetVsShader` |
| `0x33C8340` | 3 | `sceGnmSetPsShader350` |
| `0x33C8380` | 3 | not resolved |
| `0x33C8270` | 3 | not resolved |
| `0x33C82C0` | 3 | not resolved |
| `0x33C81D0` | 1 | not resolved |
| `0x33C88A0` | 1 | not resolved |

## Runtime stack evidence

Every captured draw event carries `stack_args[4] = 0x803F26A88`, which is
`module_base + 0x3F26A88` — the wrapper table slot. This corroborates that the wrapper table is
live at runtime.

## Not established

- The object type behind either table. No RTTI or type name recovered.
- Whether all 92 `+0x18` writer calls target the same object.
- The names of five GNM stubs listed above.
- Runtime liveness of the context vtable `0x03EDFCA0`. Only the wrapper table is corroborated.
