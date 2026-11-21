# A6 handoff: shader binding path and the absent pass boundary

Evidence package for the independent Binary Atlas track. Checkable against the original SELF
with `tools/a5/verify_self.py`, which now gates on image identity and runs 39 checks.

## Provenance

| Item | Value |
| --- | --- |
| Title | CUSA11919 |
| Image | 1.00, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Second image | 01.01, SHA-256 `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` |
| Size | 81,506,767 bytes, both |
| Source revision | `research/a6-shader-binding-20261122` |
| Base | A5 `53e67ad3` |
| Address space | inner-ELF VA; runtime VA = `module_base + RVA`, `module_base = 0x800000000` |

Both images pass all 39 checks. No address differs between them.

## Headline finding

`FRHICommandSetBoundShaderState` **does not exist as a deferred command** in this binary.

The public UE4.18.3 header declares it. This build does not use it on the path that reaches the
draw. Shader binding happens inside the recording routine at `0x0CB5130`, which is vtable slot
`+0x340` of the context vtable `0x03EDFCA0`.

This corrects the A5 chain, which listed `SetBoundShaderState` as the missing link. It was not
missing; it was never there.

## The binding edge

The definite ownership chain, verified byte by byte:

```
context      + 0x468  ->  shader-state object
shader-state + 0x200  ->  shader resource
shader-state + 0x1C0  ->  32-bit value passed as the GNM flags argument
shader-resource + 0x1D8  ->  program data
program-data + 0x3       ->  register-array selector byte
program-data + 0x40 + selector*4  ->  register array
program-data + 0x5E + selector*4  ->  per-index flag byte
```

The GNM setter receives a pointer into the register array, not a copy. The array lives inside
the shader resource's program data.

## Layer separation

| Layer | Location | Nature |
| --- | --- | --- |
| UE4 shader resource | `shader-state + 0x200` | reference-counted object |
| Program data | `shader-resource + 0x1D8` | PS4-side program block |
| Register array | `program-data + 0x40 + index*4` | raw GCN register values |
| GNM call | `sceGnmSetVsShader` | writes registers into PM4 |

The UE4 object and the register array are different things. The array is a field, not a
separate allocation. One shader resource carries multiple arrays, selected by the byte at
`program-data + 0x3`.

## Call sites

| Site | Target | Note |
| --- | --- | --- |
| `0x0CCD139` | `sceGnmSetVsShader` | array at `program_data + 0x40 + index*4` |
| `0x0CCD201` | `sceGnmSetVsShader` | array at `program_data + 8` |
| `0x0CCD0C0` | `sceGnmSetPsShader350` | null array; registers already written |

The third passes a null array because the pixel-shader registers were written by a preceding
`SetShReg` sequence. That is consistent with the A4 correction, where the null argument was
shown not to mean "disable".

## Claim classification

### VERIFIED

- The recorder `0x0CB5130` is vtable slot `+0x340` of `0x03EDFCA0`.
- The shader receiver `0x0CCC2F0` is called only from inside that recorder.
- The full field chain from `context+0x468` to the register array.
- The register-array pointer arithmetic and the `0x1D` size argument.
- The null array at the pixel-shader setter.
- The absence of a shader-binding command in the vtable cluster.
- The corrected `SetRenderTargets` extents: depth view 16 bytes at `+0xD8`, UAV count at
  `+0xE8`, UAV array 8 pointers at `+0xF0`.

### STRONG INFERENCE

- The names "shader-state object", "shader resource", "program data". They come from behaviour,
  not symbols.
- The byte at `program-data + 0x3` is a register-array selector. It is used as an index; its
  full meaning is not established.
- The per-index flag at `+0x5E` gates a variant path. The path is visible; its purpose is not.

### UNRESOLVED

- `BeginRenderPass` / `EndRenderPass`. Not found. Whether they exist elsewhere, or whether this
  backend uses implicit passes, is not established.
- Command ordering. The list head and tail were not traced to a producer.
- What `shader-state + 0x1C0` represents. It is passed as the GNM `flags` argument.
- The object type behind the command list. No RTTI recovered.

## What this does not claim

The absence of `BeginRenderPass` is an absence of evidence, not evidence that the backend uses
implicit passes. The render-target and viewport commands are state-cached and emit PM4 context
registers directly, which is *consistent* with implicit passes, but no mechanism was traced.

Command ordering is not established. A recovered set of command types is not an observed
sequence.

## Reproduction

```
# 39 checks against the original SELF; exits 2 on a non-approved image
python tools/a5/verify_self.py <eboot.bin>

# function bounds
python tools/a5/fde.py <unwrapped.elf> 0x0CCC2F0 0x0CB5130
```

## Next targets, in priority order

1. Trace the command list head and tail to a producer. Ordering is the largest remaining gap.
2. Decode `0x0CCD0C0` fully to complete the pixel-shader picture.
3. Determine what `shader-state + 0x1C0` is.
4. Find the pass-delimiting mechanism, given that `BeginRenderPass` is absent.

## Files

| File | Purpose |
| --- | --- |
| `maps/A5_address_map.tsv` | machine-readable addresses, fields, confidence, including A6 additions |
| `reports/A6_shader_binding.md` | the shader binding path and the absent pass boundary |
| `reports/A5_phase1_render_targets_viewport.md` | SetRenderTargets and SetViewport |
| `reports/A5_phase2_stream_source.md` | SetStreamSource |
| `tools/a5/verify_self.py` | 39 checks against the original SELF |
| `tools/a5/fde.py` | function bounds from `.eh_frame_hdr` |
