# A5 handoff: Déraciné deferred RHI command stream

Evidence package for the independent Binary Atlas track. Everything here is checkable against
the original SELF with `tools/a5/verify_self.py`, which needs only the Python standard library
and does not unwrap the ELF.

## Provenance

| Item | Value |
| --- | --- |
| Title | CUSA11919 |
| Image | 1.00, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Second image | 01.01, SHA-256 `8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75` |
| Source revision | `research/a5-semantic-rhi-20261122` |
| Base | A4 `6d08f2ff` |
| Address space | inner-ELF VA; runtime VA = `module_base + RVA`, `module_base = 0x800000000` |

Both images pass all 25 verification checks. No address in this package differs between them.

## The command object

```
+0x00  vptr
+0x08  Next
+0x10  command-specific payload
```

Execution dispatches through `vtable[0]`. Teardown dispatches through `vtable[1]`.

This diverges from the public UE4.18.3 `FRHICommandBase`, which declares
`{Next, ExecuteAndDestructPtr}`. The target binary uses a vtable instead. Treat the public header
as a guide to payload layout, not as a specification of the object header.

## Recovered commands

| Command | Vtable | Execute | Receiver |
| --- | --- | --- | --- |
| `SetViewport` | `0x03F26A08` | `0x13A6BB0` | `0x0CB8AE0` |
| `SetRenderTargets` | `0x03F26A28` | `0x13A6A30` | `0x0CC2420` |
| `SetStreamSource` | `0x03F26A48` | `0x13A6930` | inline |
| `DrawPrimitive` | `0x03F26A68` | `0x13A6900` | `0x0CBAAD0` |
| `DrawIndexedPrimitive` | `0x03F26A88` | `0x13A68B0` | `0x0CBB9C0` |

The five vtables are contiguous in one cluster. Each has a trivial destructor at slot 1, which is
what makes them findable: `tools/a4/cmdvtables.py` scans for that pattern.

## Claim classification

### VERIFIED

Read directly from the original bytes, and re-checked by `verify_self.py`.

- The five vtable addresses, their `Execute` pointers, and their trivial destructors.
- The command header: vptr at `+0x00`, `Next` at `+0x08`, payload from `+0x10`.
- The execute loop calls `vtable[0]`; the teardown loop calls `vtable[1]`.
- `SetRenderTargets` payload offsets `+0x10`, `+0x18`, `+0xD8`, `+0xE8`, `+0xF0`, and the
  24-byte render-target stride with eight entries.
- `SetViewport` payload offsets `+0x10` through `+0x24`, with `MinZ` and `MaxZ` as floats.
- `SetStreamSource` payload offsets `+0x10`, `+0x18`, `+0x20`, the `-0x68` buffer adjustment,
  and the `divl` that computes a vertex count from size and stride.
- The draw wrapper's `E8 rel32` at `0x13A68E3` targeting `0x0CBB9C0`.
- The context-side state offsets listed in the TSV.
- Function bounds, from the `.eh_frame_hdr` binary search table.

### STRONG INFERENCE

Consistent with the bytes and with the pinned public header, but not proven by a symbol.

- The command *names*. The executable is stripped. Each name is a mapping from payload shape and
  receiver behaviour to the public `FRHICommand*` struct.
- The payload *field names*. Offsets and sizes match the public structs; the names are the
  mapping.
- The `-0x68` adjustment is the `FVertexBuffer` to `FRHIResource` back-pointer. It is inferred
  because the field it lands on behaves as a byte size and the code divides by stride.
- `0x0CBAAD0` as the `DrawPrimitive` receiver. The payload shape matches, but the receiver was
  not decoded far enough to confirm its semantics.

### UNRESOLVED

- The object type behind the command list. No RTTI or type name recovered.
- Whether `FRHICommandBeginRenderPass` / `EndRenderPass` exist in this binary. They are the real
  pass boundary in the public header, and neither was found.
- `FRHICommandSetBoundShaderState`. Not yet located.
- The stack argument to the `SetRenderTargets` receiver. It is always zero at the one call site
  examined, so its meaning is not established.
- The exact class layout above `FRHIResource`.
- The debug branch in `SetStreamSource` that calls `scePthreadGetthreadid` and changes the
  pointer arithmetic. Its purpose is not established.

## What is NOT a pass boundary

`SetRenderTargets` and `SetViewport` are pass *state*, not pass *delimiters*. The public header
has explicit `BeginRenderPass` and `EndRenderPass` commands carrying `FRHIRenderPassInfo`, and
neither was found here. Do not treat the render-target setter as a pass boundary.

## State caching

Both `SetRenderTargets` and `SetViewport` compare against cached context state and suppress
redundant PM4 packets. A render-target change is not one packet per call. Any instrumentation
that counts packets will undercount state changes, and any that counts calls will overcount
packets.

## The recovered chain

```
SetRenderTargets -> SetViewport -> SetBoundShaderState -> SetStreamSource -> DrawIndexedPrimitive
```

Four of five are identified. `SetBoundShaderState` is the gap.

## Reproduction

```
# verify every claim against the original SELF (no ELF unwrap, stdlib only)
python tools/a5/verify_self.py <eboot.bin>

# function bounds from .eh_frame_hdr
python tools/a5/fde.py <unwrapped.elf> 0x13A6A30 0x13A6BB0

# find command vtables by the Execute + trivial-destructor pattern
python tools/a4/cmdvtables.py <unwrapped.elf>
```

`verify_self.py` exits 0 only when all 25 checks pass. It reads the SELF container directly and
maps the inner ELF program headers, so it works without the unwrap step.

## Next targets, in priority order

1. `FRHICommandSetBoundShaderState`. The last missing link in the chain. Look for a command whose
   payload is a single shader-pair reference.
2. `FRHICommandBeginRenderPass` / `EndRenderPass`. The real pass boundary. Look for a command
   carrying `FRHIRenderPassInfo`, which is a large struct with attachment arrays.
3. The `DrawPrimitive` receiver at `0x0CBAAD0`. Confirm or refute the identification.
4. The command list head and tail. Tracing them would establish emission order, which is what
   would let the chain above be asserted as an actual sequence rather than a set.

## Files

| File | Purpose |
| --- | --- |
| `maps/A5_address_map.tsv` | machine-readable addresses, fields, confidence |
| `reports/A5_phase1_render_targets_viewport.md` | SetRenderTargets and SetViewport |
| `reports/A5_phase2_stream_source.md` | SetStreamSource |
| `tools/a5/verify_self.py` | 25 checks against the original SELF |
| `tools/a5/fde.py` | function bounds from `.eh_frame_hdr` |
