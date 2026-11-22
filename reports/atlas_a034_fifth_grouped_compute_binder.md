# Independent Binary Atlas A-034 — Fifth grouped binding dispatcher and compute-stage sampler path

**Static original-target analysis** of approved Déraciné PS4 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Addresses are original inner-ELF virtual addresses, not runtime guest addresses; executable SELF file offsets = VA+`0x1FD30` in the inspected region. No original game/shader data is committed. Read-only locally run verifier: **123/123 passing** (45 exact instruction anchors, 46 direct E8-call targets, 30 bounded displacement checks, two arithmetic consistency checks).

## Primary discovery: a fifth grouped resource dispatcher

Earlier A-031/A-033 documented four grouped binding helpers at `0x0CC0070`, `0x0CC0300`, `0x0CC0670`, `0x0CC09E0`. A-034 finds an additional complete helper at **`0x0CC71A0..0x0CC74F5`**, invoked directly by six original call sites:

`0x0CB1561`, `0x0CB19C4`, `0x0CB4DFF`, `0x0CC3CA0`, `0x0CC3DC0`, `0x0CC5456`.

These six `E8 rel32` calls resolve unambiguously to `0x0CC71A0`. The helper accepts a context (`rdi`) and shader/resource state (`rsi`) and implements the same `pending = context_dirty & resource_group_mask` dispatch as the other four. Its distinctive offsets are:

| Helper | Context dirty mask | Group cache pointer array | Source resource mask |
| --- | --- | --- | --- |
| `0x0CC0070` | `+0x928` | `+0x568` | resource `+0x200` |
| `0x0CC0670` | `+0x92C` | `+0x608` | resource `+0x200` |
| `0x0CC0300` | `+0x934` | `+0x748` | resource `+0x200` |
| `0x0CC09E0` | `+0x938` | `+0x7E8` | resource `+0x200` |
| **`0x0CC71A0`** | **`+0x93C`** | **`+0x888`** | **resource `+0x200`** |

The fifth helper's mask-intersection reads appear at `0x0CC71B4/0x0CC71BB`, cache selection at `0x0CC7260`, and mask-clear at `0x0CC74DB`.

### Same serialized binding-list families, but a different sampler writer

The fifth helper references all three A-031 active packed binding lists:

1. **`resource+0x248`** at `0x0CC71C7`: selects a cached resource-object group and packed entry; calls object virtual slot `+0x38` at `0x0CC72D7`, then typed object adapter **`0x0CC13B0`** at `0x0CC72E5`.
2. **`resource+0x208`** at `0x0CC731C`: resolves object-array entries and calls conditional 16/32-byte resource adapter **`0x0CAC030`** at `0x0CC7355`.
3. **`resource+0x218`** at `0x0CC7398`: binds 16-byte sampler descriptors **inline** rather than calling the common `0x02F62D60` writer used by the original `0x0CC0070` helper.

The sampler path retrieves the stage-zero GNM map pointer `[[context+0x460]+0xC468]` (`0x0CC73C0/0x0CC73CA`) and indexes the **sampler-specific** `metadata+0xA2+2*api_slot` map at `0x0CC73D1`. It checks `0xFFFF` for unmapped entries, masks low 14 register-index bits, and branches on the signed flag:

- **Shadow path:** copies four DWORDs from `object+0x14` into writer shadow state at `writer+0x378+4*map_index` (`0x0CC7402..0x0CC7417`).
- **Direct path:** emits a four-DWORD `SET_SH_REG` packet with `0xC0047602`, a relative register offset of `map_index+0x240` (`0x0CC7460`), and the four sampler words from object `+0x14..+0x20` (`0x0CC746E..0x0CC749C`).
- Marks writer dirty at `writer+0xC538` (`0x0CC74A0`).

The register-relative offset `0x240` corresponds to **GCN CS user-data register base `0x2E40`** relative to the `SET_SH_REG` `0x2C00` space. This, together with the first slot in the seven-entry bank lookup from A-022, supports **compute-stage** classification. It is not proof any particular Déraciné gameplay draw/dispatch executed the path. The six callers confirm static call reachability.

### A guest-visible side effect before resource binding

The `resource+0x248` path reads a pointer from **selected_object+0x40** at `0x0CC72C0`, compares its double-precision value with a global double read at `0x0CC7285`, and can conditionally overwrite the pointed-to value at `0x0CC72D0` before calling the object's virtual `+0x38` and `0x0CC13B0`. The source-level meaning of this value is unproven. A native bridge must not automatically eliminate the guest-visible writeback just because it can bind a descriptor directly.

## Extended constructor coverage: six five-list deserialization sequences

A-033 verified two five-call construction sequences. An exhaustive direct `E8 rel32` scan targeting the shared vector deserializer **`0x0CF0580`** within original code window `[0x0CA0000,0x0D10000)` reveals **30 callers in six groups of five**:

| Group | First call | Last call |
| --- | --- | --- |
| 1 | `0x0CEF8B7` | `0x0CEF8F7` |
| 2 | `0x0CF1767` | `0x0CF17A7` |
| 3 | `0x0CF2557` | `0x0CF2597` |
| 4 | `0x0CF333F` | `0x0CF3389` |
| 5 | `0x0CF3E56` | `0x0CF3E96` |
| 6 | `0x0CF4B87` | `0x0CF4BC7` |

Each creates five vector/list headers at `resource+0x208/+0x218/+0x228/+0x238/+0x248`, consistent with A-032/A-033. This demonstrates wider constructor coverage than the two prior sequences. It does **not** identify the source archive or imply every constructor variant appears in gameplay.

## The two unclassified lists remain unclassified, with a stronger bound

A byte-displacement count of each of the **five** known grouped-binder bodies shows exactly one direct 32-bit displacement reference to each of `+0x200,+0x208,+0x218,+0x248`, and **zero** references to `+0x228` and `+0x238`, over these ranges:

`0xCC0070..0xCC0300`; `0xCC0300..0xCC0670`; `0xCC0670..0xCC09E0`; `0xCC09E0..0xCC0D40`; `0xCC71A0..0xCC74F5`.

These are **bounded negative tests**, not proof of total absence of other readers. Nearby `+0x228/+0x238` patterns include unrelated object layouts and control-flow displacements; matching a field number alone is not evidence of accessing the shader resource. The two auxiliary lists are still initialized/deserialized/released per A-033 and must be preserved in a native guest-object representation.

## Native renderer / Codex handoff

This gives a fifth, compute-compatible `BindingGroupUpdate` implementation. It can share a typed semantic interface with the four earlier grouped binders, but its sampler emission and the object `+0x40` writeback differ from the graphics variants. The correct native adapter interface must preserve the *source* `GnmInputUsageSlot` declaration, packed resource assignment, selected live object, 16/32-byte descriptor, command sequence, and guest-state mutation as separate fields.

For a future Codex live join, observe **one** `0x0CC71A0` invocation and stage-zero sampler path (if reachable), or one existing graphics binder, and correlate a shader hash against `resource+0x200`, selected packed entry and descriptor; do not add broad always-on hooks or make shadPS4 a production dependency.

**Unresolved:** exact original object classes, writeback semantics, positive consumers of lists `+0x228/+0x238`, real runtime reachability and descriptor-to-Vulkan equivalence. No runtime capture was conducted.

## Verification

Run `python tools/atlas/verify_independent_a034.py PRIVATE_ORIGINAL_1.00_EBOOT.bin`.

Local approved-SELF verification: **123/123**, comprising 45 original instruction anchors, 46 direct call-target checks (six calls to the fifth binder, ten calls to typed adapters, thirty constructor/deserializer calls), 30 five-helper displacement-count checks, and 2 arithmetic sanity checks. The exact original-SELF size and SHA-256 are checked *before* examining any code bytes. The verifier does not write or copy original game data.
