# SC-06 Phase 5: one trustworthy draw association

## Result

A defensible RHI-to-GNM association is established for the title screen, and the ordering join is
validated. The GPU-side half remains unresolved, and the reason is now precise.

## The join that works

The capture hooks the wrapper at RVA `0x13A68B0`. The RHI probe observes the entry at RVA
`0x0CBB9C0`, whose caller is `0x13A68E8` — that is `0x13A68B0 + 0x38`, **inside the same
wrapper**.

So capture draw *N* and probe event *N* are the same call, observed at two points in one
function. The join is by ordinal, not by parameter.

### Validation

| Test | Result |
| --- | --- |
| Offset 0, index counts | **122 / 128 match** |
| Offset -1 | 106 / 127 |
| Offset +1 | 105 / 128 |
| Multiset equality over the first 128 | **identical** |

Offset 0 is the correct alignment, and the multiset is exactly equal. So the two streams are
observing the same draws.

### The six mismatches are ordering, not identity

| Index | Capture | GNM |
| --- | --- | --- |
| 1 | 48 | 11904 |
| 2 | 11904 | 48 |
| 37 | 11904 | 48 |
| 38 | 48 | 11904 |
| 54 | 11904 | 48 |
| 55 | 48 | 11904 |

Each mismatch is an adjacent swap of the same two draws. The GNM records come from several
`TaskGraphThreadNP` threads (0, 1, 2), so two draws issued on different threads can be logged in
either order. The capture and the probe each serialise them differently.

That is a real limitation of an ordinal join across threads, and it is bounded: 6 of 128, all
adjacent transpositions.

## The association, for one draw

Draw 0, title screen, verified scene:

| Layer | Value |
| --- | --- |
| Scene | classroom with main-menu overlay, dump at HMD frame 2400 |
| UE4 command | `CommandDrawIndexedPrimitive`, wrapper RVA `0x13A68B0` |
| Sequence | capture 1, probe 1, GNM 1 |
| Primitive topology | `PT_TriangleList` (UE4 0), `NumPrimitives=2` |
| Index count | 6 |
| Instance count | 1 |
| RHI index resource | `0x10044E49E8` |
| GNM `index_addr` | `0x2000042800` |
| GNM `cmdbuf` | `0x30000F0AE0` |
| GNM `size` / `flags` / `type` | 10 / 0x0 / 0x1 |
| VS resource | resolved via the alternate slot `+0x1E0` |
| Correlation | ordinal within one wrapper, multiset-validated |

Note the address: `0x2000042800`, not `0x200004280C` from the earlier health-warning run. The
title screen uses a different index buffer, which is expected.

## What is still missing

The **GPU-side resolved draw** for this specific draw.

The PM4 hook observes 128 packets, but they are a different 128-record slice of the stream
(Phase 4). So the chain is:

```
UE4 command  --[ordinal, validated]-->  RHI entry  --[same thread, 1:1]-->  GNM draw
                                                                              |
                                                                    [NOT JOINED]
                                                                              |
                                                                     PM4 / GPU draw
```

The first two arrows are established. The third is not.

## Why the third arrow is not established

Three candidate mechanisms, and the evidence rules out the first two.

### Ruled out: parameter matching

Phase 4 showed zero shared `(index_count, index_base)` pairs between the GNM and PM4 probe sets,
because the sets are different slices. Even within one slice, `index_count=48 @ 0x20000c0000`
recurs many times, so parameters cannot identify a draw.

### Ruled out: equal record caps

The two threads consume at different rates, so equal caps land at different stream positions.
Raising the caps would enlarge the slices but not align them.

### Remaining: a submission identity

The GNM call receives `cmdbuf` (`0x30000F0AE0` for draw 0). The PM4 packet is written into that
same buffer. If the PM4 hook recorded the packet's address within the command buffer, the two
would join on a real submission identity.

That is the smallest next step, and it needs one field in an existing diagnostic line.

## Title-screen coverage

Preliminary, from one capture. Not exhaustive game-wide coverage.

| Item | Value |
| --- | --- |
| Captured records | 16384 |
| Indexed draws | 5346 |
| Non-indexed draws | 36 |
| Stream sources | 9540 |
| Render targets | 364 |
| Viewports | 718 |
| Unknown commands | 0 |
| Distinct VS resources | 63 |
| Shader resolution | 5726 valid via alternate, 0 failures |

Index counts span 3 to 18339, so the scene exercises a wide range of geometry.

## What this does not claim

- That the GPU-side draw is associated. It is not.
- That the ordinal join is exact. It is 122/128 with 6 adjacent transpositions.
- That the title screen is representative of gameplay. It is not.

## Reproducible checks

```
$env:SHADPS4_DERACINE_RHI_PROBE='1'
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER='33'
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
```

Then compare the capture's `command_id=1` index counts against the `DERACINE_GNM_DRAW_INDEX`
`index_count` values in order. Offset 0 gives 122/128, and the multisets are identical.
