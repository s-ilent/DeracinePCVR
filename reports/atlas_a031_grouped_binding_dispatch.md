# Independent Binary Atlas A-031 — grouped shader-binding lists and guest resource-object provenance

**Finding type:** static, original Déraciné 1.00 PS4 SELF, plus an abstract native binding interface. **Not** a runtime join between a particular shader hash, guest object, and Vulkan descriptor. The original 81,506,767-byte SELF is exact SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. Addresses are inner-ELF VAs; original file offset = VA + `0x1FD30` in this code segment.

**Reproduction:** `python tools/atlas/verify_independent_a031.py <private-original-eboot.bin>`. Locally **110/110 checks passed**: 64 original instruction-byte anchors, nine independently resolved `E8 rel32` direct calls, one `E9 rel32` tail jump, four synthetic packed-entry round-trips, and 32 arithmetic sanity checks for dirty-group indexing. Thus **74 checks derive directly from original code**, while 36 test decoding invariants. No source game/shader bytes are committed.

## Result: a dispatch layer *above* the seven typed GNM descriptor writers

A-030 recovered seven immediate descriptor writers, but its direct-`E8` scan necessarily missed tail calls and paths that first resolve a guest resource object. A-031 identifies the original binder's grouped command lists and a full *static* producer path into two of those writers.

At **`0x0CBFFC5..0x0CC003E`**, a caller reads `shader_state = [context+0x468]` and conditionally processes its four non-null resource slots, using four distinct helpers:

| Shader-state source | Helper | Per-context dirty mask |
| --- | --- | --- |
| `shader_state+0x1E0` | **`0x0CC0070`** | `context+0x928` |
| `shader_state+0x1E8` | `0x0CC0300` | `context+0x934` |
| `shader_state+0x1F0` | `0x0CC0670` | `context+0x92C` |
| `shader_state+0x200` | `0x0CC09E0` | `context+0x938` |

The last three helpers' prologues independently verify `context_mask & shader_resource[0x200]` and load the binding-list source from `shader_resource+0x248`; they follow a similar grouped-dirty update pattern. These offsets are **not** a claim about UE4 shader-stage names; the relevant stage-number domains differ.

### Encoded 32-bit binding entries

In the `0x0CC0070` variant, the working group mask is:

`pending = [context+0x928] & [shader_resource+0x200]` (`0x0CC0087..0x0CC0099`).

At `0x0CC00D0`, `blsi` isolates the lowest set bit, and the following bit-scan arithmetic derives group index `k`. The group selects a retained/cached object at `[context+0x568 + 8*k]` (`0x0CC0140`), from which `+0x20` supplies an **array of resource-object pointers** (`0x0CC0150`). The shader resource provides three independent index/list tables at `+0x248`, `+0x208`, and `+0x218`. A per-group DWORD heads table points to grouped records, whose high byte is compared with `k` to continue iteration.

The original `BEXTR` control literal `0x1008` selects bits 8–23; `movzx` on `al` gives bits 0–7; `shr 24` gives the grouping byte. Accordingly:

```cpp
struct PackedBindingEntry { // conceptual decoded view of one original u32
  u8 api_slot;              // raw & 0xff
  u16 object_array_index;   // (raw >> 8) & 0xffff
  u8 group;                 // raw >> 24
};
```

**Important distinction:** this compact record is *not* `GnmInputUsageSlot` (also four bytes). `GnmInputUsageSlot` declares the shader's expected resource kind, logical slot and starting register; the packed command-list record selects a **live guest resource-object instance** and passes the logical slot to a binder. They serve complementary roles, and the formats should not be conflated.

### Three source-list operations verified for `0x0CC0070`

| Shader resource list | Original dispatch sites | Source-to-writer edge |
| --- | --- | --- |
| `+0x248` | `0x0CC016D..0x0CC01BC` | Pointer array `[cached+0x20]`, select `object_array_index`, call object virtual slot `+0x38`, pass returned binding object and `api_slot` to **`0x0CC13B0`** |
| `+0x208` | `0x0CC01F8..0x0CC0222` | Same cached array, select object, call shared binding adapter **`0x0CAC030`** with logical slot and mode; its exact dynamic descriptor-class selection is not classified in this pass |
| `+0x218` | `0x0CC026F..0x0CC02B3` | Select `object=[cached+0x20][index]`, take **`object+0x14`** as the 16-byte descriptor source, call **sampler writer `0x02F62D60`** with count=1 |

The first path's **`0x0CC13B0` resource adapter** is especially useful. It receives the object returned from the virtual method, advances to `+0x18` (`0x0CC13D3`), optionally resolves a resource identity via `0x03092D20` (non-flag path), and eventually forwards `source=[object+0x18]`, group/mode selector, logical slot, and writer `[context+0x460]+0x2E8` through a **tail jump at `0x0CC14E0` to the 32-byte immediate-resource writer `0x02F62A40`**. That original `E9 rel32` explains why A-030's `E8` direct-call count is not exhaustive.

Do not interpret `object+0x14` and `object+0x18` as a single shared resource structure. They arise in **different** observed families. Object ownership, allocation, memory lifetime, texture format and guest-visible copy semantics are still unresolved.

The first helper clears `context+0x928` on exit (`0x0CC02E1`); the other three clear `+0x934`, `+0x92C`, and `+0x938` respectively (`0x0CC064B`, `0x0CC09BB`, `0x0CC0D2B`). This makes dirty-group participation relevant to live binding capture: not every declared resource yields a writer call every draw.

## Native renderer consequence

The original binary now exposes **three distinct layers** that should be preserved separately:

1. **Shader declaration:** `GnmInputUsageSlot {usagetype, apislot, startregister, flags}` from the original `OrbShdr` metadata (A-025–A-028R).
2. **Live binding assignment:** `PackedBindingEntry {api_slot, object_array_index, group}` plus the correct list family and guest resource object; gated by shader-resource group mask and context dirty mask (A-031).
3. **Typed descriptor materialization:** copy 16/32 bytes from a live object, consult the corresponding decoded map, and issue a native typed resource binding rather than requiring GNM/PM4 to execute (A-030).

A possible independent semantic capture record is:

```text
BindingUpdate {
  shader_identity, shader_state_slot, rhi_context,
  group, packed_entry, source_list_kind,
  guest_object_identity, descriptor_source_identity,
  usage_kind, logical_api_slot, decoded_map_word,
  16_or_32_descriptor_bytes_by_value, sequence_id,
}
```

**Do not assume** that the shader hash is attached at the writer call site, that every `+0x248` entry is a texture, or that an unchanged group causes a typed write. The source-only evidence proves *what it would do if executed*, not that it executes for a given draw, nor any agreement with shadPS4's post-translation descriptors.

## Best next live correlation (for Codex, not a required native dependency)

On a known A-028R shader, observe one `0x0CC0070` execution **and a specific matching inner `0x0CC02B3` sampler call**, capturing packed entry, group, source object pointer, 16 source bytes and effective map value. Join to the **same stage/draw** only with verified shader-state identity and draw/synchronization provenance. A second pass can follow `0x0CC01AD -> 0x0CC13B0 -> 0x02F62A40` for a 32-byte resource. Avoid broad hooks until this narrow relation is seen.

**Suggested continued static research:** inspect `0x0CAC030` to classify the middle list and trace constructor/writer ownership of the `shader_resource+0x208/+0x218/+0x248` packed lists; then identify how their entries attach to UE4 RHI handles. That would strengthen the eventual native PC resource adapter without using the GPU emulator as a production backend.