# Independent Binary Atlas A-023 — Shader-map provenance and six deferred data-upload channels

**Static original-target finding, not a live capture.** Approved 1.00 Déraciné SELF: 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Addresses are inner ELF VAs; original executable offset = VA + `0x1FD30`, RELRO vtable offset = VA + `0x21240`. Local verifier: **79/79 passed** on the original image. No game bytes, extracted ELF, shaders or runtime logs included.

## A-022 stage-map producer found

A-022's generic shader user-data binder reads a pointer from `[writer+0xC180+8*stage]`, then a 16-bit map word from `[stage_map+0xC2+2*slot]`. Here `writer=[context+0x460]+0x2E8`, so index zero reads `[[context+0x460]+0xC468]`.

Two independent producer paths install that pointer from the active shader-resource object:

| Site | Observed operation |
| --- | --- |
| `0x0CB12F7` | graphics state object from `context+0x460` |
| `0x0CB12FE` | program data from `shader_resource+0x1D8` |
| `0x0CB1305` | `lea rcx,[shader_resource+0xB0]` |
| **`0x0CB132A`** | `state+0xC468 = shader_resource+0xB0` |
| `0x0CB1331` | `state+0xC388 = program_data` |
| `0x0CC36B3` | second setup resolves resource from a wrapper's `+0x60` |
| `0x0CC36FC` | `lea rcx,[shader_resource+0xB0]` |
| **`0x0CC372B`** | second `state+0xC468` store |

**Consequence:** when the generic A-022 binder uses stage-index zero on these paths, its slot-map read `stage_map+0xC2+2*slot` becomes **`shader_resource+0x172+2*slot`**. The map pointer is borrowed from a shader-resource metadata region, not independently allocated by the writer. The constructor that populates these metadata bytes and the corresponding stage>0 map producers remain unresolved.

## A separate owner supplies a queued range-upload command

At `0x0CC369C`, another object pointer is saved to `[rsp+0x28]`, later reloaded at `0x0CC387E`. **Do not conflate this owner with the shader-resource object above.**

Its range header addresses are passed to two sibling helpers:

- `0x0CC388F`: `owner+0xBC` -> `0x0CC6EA0`;
- `0x0CC389C`: `owner+0xC2` -> **`0x0CC6BA0`** (call `0x0CC38B2`), alongside a caller-stack payload.

The second helper reads three 16-bit header fields at `+0,+2,+4`. The last is a length, bounded by `min(length,0x10)`; zero length skips enqueue. It copies at most **16 bytes by value** into an aligned command-list arena allocation and appends a `0x30`-byte linked command, vptr **`0x03EDF7E0`**:

| Command offset | Saved field |
| --- | --- |
| `+0x10` | associated source/wrapper identity |
| `+0x18` | pointer to copied command-arena bytes |
| `+0x20` | first 16-bit header field |
| `+0x24` | second 16-bit header field, destination offset |
| `+0x28` | bounded byte count |

The command's Execute at **`0x0CAB200`** passes the copied data and offset/count through `[command_list+0x20]+0xB08` to common writer **`0x0CAAEB0`**. The writer copies into the destination's backing pointer `+0x48`, updates its high-water length at `+0x50`, and marks dirty at `+0x40`. This by-value snapshot differs from the A-022 resource binders, which consult and release retained references during Execute.

## Six equivalent Execute variants

The following original RELRO vtables each contain a command whose virtual Execute loads `cmd+0x18/+0x24/+0x28`, selects a different context destination pointer, and tail-jumps to `0x0CAAEB0`:

| Vtable | Execute | Destination pointer in command-list context |
| --- | --- | --- |
| `0x03EDF740` | `0x0CAAE70` | `+0xAE0` |
| `0x03EDF760` | `0x0CAB100` | `+0xAE8` |
| `0x03EDF780` | `0x0CAB140` | `+0xAF0` |
| `0x03EDF7C0` | `0x0CAB1C0` | `+0xAF8` |
| `0x03EDF7A0` | `0x0CAB180` | `+0xB00` |
| `0x03EDF7E0` | `0x0CAB200` | `+0xB08` |

**Do not assign shader-stage names to these six pointers on ordering alone.** The `+0xB08` constructor is verified; other producers have not been traced. No gameplay reachability is claimed for any of the six.

## SC-09/Codex implications

1. The primary *stage-zero* shader-user-data slot map on these paths comes from `shader_resource+0xB0`. Read mapping words from `shader_resource+0x172+2*slot`, subject to valid resource lifetime and stage/context confirmation.
2. These additional deferred commands may explain constant-data updates missing from an RHI draw-only capture. Their payload is already snapshotted in the command list, so it can be copied synchronously at Execute rather than retaining pointers.
3. The owner `+0xC2` upload header and the shader resource `+0xB0` mapping are **different traced objects**. Do not treat them as the same table without further provenance.

**Verification:** `python tools/atlas/verify_independent_a023.py <private-original-1.00-eboot.bin>` — SHA/size-gated, 79 original-byte, relative-call, vtable and field checks. Full offline analysis and verifier are preserved in the independent research artifacts.

**Next bounded targets:** find the constructor that populates `shader_resource+0xB0`, then identify stage>0 map stores; or correlate this six-command upload family against the SC-series user-data observations. Neither the resource's specific UE4 class nor its runtime path is proved here.
