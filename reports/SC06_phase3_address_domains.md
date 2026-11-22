# SC-06 Phase 3: the index-buffer address domains

## Result

The RHI `IndexBuffer` field and `regs.index_base_address` are **not comparable**. They are
different address domains, and the translation between them happens inside the guest.

The chain is now observed end to end on the title screen, with all three layers captured in one
run.

## The four domains

| Layer | Value observed | Domain |
| --- | --- | --- |
| UE4 RHI resource pointer | `0x10044E49E8`, `0x10044E8688` | guest heap, RHI object |
| GNM `index_addr` | `0x200004280C`, `0x20000C0000` | GPU-visible memory |
| PM4 `index_base` | same as GNM | GPU-visible memory |
| GPU `regs.index_base_address` | same as PM4 | GPU-visible memory |

The RHI pointer is in `0x1004...`; the GPU-visible address is in `0x2000...`. They are not the
same value, not a fixed offset apart, and not interchangeable.

## The translation is a function of the resource, not the draw

Two RHI resources were observed, and each maps to exactly one GPU address:

| RHI resource | GNM `index_addr` | Draws |
| --- | --- | --- |
| `0x10044E49E8` | `0x200004280C` | 3 |
| `0x10044E8688` | `0x20000C0000` | 125 |

The deltas are **different**:

```
0x200004280C - 0x10044E49E8 = 0xFFBB5DE24
0x20000C0000 - 0x10044E8688 = 0xFFBBD7978
```

So this is not a constant bias. It is a real resource-to-memory translation, and it must be
recovered per resource rather than computed.

## The observed chain

```
UE4 RHI entry (0x0CBB9C0)
  gpr_rsi = RHI index-buffer resource pointer
    -> guest translates to GPU-visible memory
sceGnmDrawIndex(cmdbuf, size=10, index_count, index_addr, flags=0, type=1)
  -> PM4CmdDrawIndex2 written into cmdbuf
PM4 decode on GpuCommandProcessor
  index_base == index_addr, index_count == index_count
GPU draw
  regs.index_base_address == index_base
```

Each arrow is observed, not inferred:

| Step | Evidence |
| --- | --- |
| RHI entry → GNM call | same thread, 2-15 us apart, 128/128 paired |
| GNM → PM4 | identical `index_count` and `index_addr`/`index_base` |
| PM4 → GPU | `regs.index_base_address` is the decoded packet field |

## Index element size and offset

`NumPrimitives * 3 == index_count` holds, so the topology is a triangle list and the index count
is derived from the primitive count rather than passed through. This was established in B3 and
re-confirmed here.

`StartIndex` is a separate stack argument and is **not** folded into `index_addr`. In the B3 data,
`StartIndex=6` appeared with `index_addr=0x200004280c`, and `StartIndex=0` with
`0x20000c0000`. So the start index is applied by the GPU as an offset into the index buffer, not
by adjusting the base address.

That matters for correlation: two draws can share an `index_addr` and differ only by
`StartIndex`, so the address alone does not identify a draw.

## What this means for the join

SC-05 proposed adding `regs.index_base_address` to the GPU trace. That is still the right next
step, but it must be compared against the **GNM `index_addr`**, not against the RHI pointer.

The correct comparison is:

```
RHI resource pointer  --(guest translation, per resource)-->  GNM index_addr
GNM index_addr        ==  PM4 index_base  ==  regs.index_base_address
```

So the join key is `(index_addr, index_count, StartIndex, instance_count)`, where `index_addr` is
taken from the GNM layer, not from the RHI payload.

## Why this is better than the SC-05 proposal

SC-05 said to add `regs.index_base_address` and compare it to the RHI `IndexBuffer` field. That
would have compared `0x2000...` against `0x1004...` and produced zero matches, repeating the
primitive-enum mistake in a different form.

The correction is to insert the GNM layer as the translation boundary. The GNM probe already
captures `index_addr`, so no new instrumentation is needed for the RHI-to-GNM half.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
```

Then:

```
Select-String -Path shad_log.txt -Pattern 'DERACINE_RHI_PROBE_EVENT'   # gpr_rsi
Select-String -Path shad_log.txt -Pattern 'DERACINE_GNM_DRAW_INDEX'    # index_addr
Select-String -Path shad_log.txt -Pattern 'DERACINE_PM4_DRAW_INDEX2'   # index_base
```

The RHI values are `0x1004...`, the GNM and PM4 values are `0x2000...`. They must not be compared
directly.
