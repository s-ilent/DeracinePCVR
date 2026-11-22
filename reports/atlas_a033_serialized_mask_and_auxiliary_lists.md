# Déraciné independent Binary Atlas A-033 — Serialized group mask and auxiliary binding-list lifetime

**Target:** original Déraciné PS4 1.00 SELF, 81,506,767 bytes, SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. All code addresses below are **inner-ELF virtual addresses**, not loaded guest VAs, original SELF byte offset = inner ELF VA + `0x1FD30`. No original PS4 code, assets, or raw shader bytes are included. **STATIC** control-flow and field-layout evidence only; no new title-screen/gameplay runtime reachability measured.

**Verification:** `python A033_verify_group_mask_and_list_lifetime.py ORIGINAL_PRIVATE_EBOOT.bin` returned **120/120**: 80 exact original-byte instruction anchors, 10 constructor deserialization calls, 10 destructor free/release calls, and 20 bounded displacement-count tests across four known binding helpers. SHA/length gate always runs first. The absence tests are *scoped* to those four functions, not to the whole game.

## Result 1: the resource dirty-group mask at `shader_resource+0x200` is serialized explicitly

The A-031 grouped binder computes:

```
pending_groups = context_dirty_mask & *(uint32_t*)(shader_resource + 0x200);
```

A-032 recovered the five packed DWORD arrays but did not identify the source of `+0x200`. A-033 recovers two independent construction paths which read this mask **directly from the input stream before** deserializing the five arrays:

| Construction site | Exact effect | Notes |
| --- | --- | --- |
| `0x0CF49F4` | zero `resource+0x200` | initialization prior to reading the stream |
| `0x0CF4B04` → `0x0CF4B06` | read next source DWORD; store to `resource+0x200` | buffered source path |
| `0x0CF4B13` → `0x0CF4B24` | alternate source DWORD; store to `resource+0x200` | conditional source path |
| `0x0CF4B2F` → `0x0CF4B73` | conditional byte-reversal on the stored mask | source/reader flags control this path; exact archive endianness is not independently established |
| `0x0CF16EE` → `0x0CF16F0`, `0x0CF170B` | second setup path stores a source DWORD to `resource+0x200` | independent repeated construction code |
| `0x0CF1716` → `0x0CF1753` | second byte-reversal path | corroborates 4-byte mask normalization |

This is more specific than saying the group mask is "derived from bindings." It is an **explicit input record**, which the execution-time binder then intersects with context state. It may be useful to native deserialization because the guest already contains the group participation mask.

### The five arrays follow that mask in a fixed construction sequence

The stream setup then calls **the same helper `0x0CF0580` five times**, using the destination vector headers:

| Sequence | Destination `shader_resource+` | First constructor call | Second constructor call |
| ---: | --- | --- | --- |
| 1 | `0x208` | `0x0CF4B87` | `0x0CF1767` |
| 2 | `0x218` | `0x0CF4B97` | `0x0CF1777` |
| 3 | `0x228` | `0x0CF4BA7` | `0x0CF1787` |
| 4 | `0x238` | `0x0CF4BB7` | `0x0CF1797` |
| 5 | `0x248` | `0x0CF4BC7` | `0x0CF17A7` |

All ten E8 relative calls were resolved to **`0x0CF0580`** in the approved original image. The list helper reads a DWORD count and a sequence of 32-bit entries, as established in A-032. The payload remains an **opaque serialized list format** rather than a proven named UE4 asset type. The order of constructor calls and the location of the mask are original code observations, not speculative file format reconstruction.

## Result 2: `+0x228/+0x238` have a demonstrated lifetime but not a demonstrated graphics binding use

A-031 proved four executed-*if-called* grouped binders:

| Binder | Context dirty mask | Direct array references |
| --- | --- | --- |
| `0x0CC0070` | `context+0x928` | resource `+0x248`, `+0x208`, `+0x218` |
| `0x0CC0300` | `context+0x934` | `+0x248`, `+0x208`, `+0x218` |
| `0x0CC0670` | `context+0x92C` | `+0x248`, `+0x208`, `+0x218` |
| `0x0CC09E0` | `context+0x938` | `+0x248`, `+0x208`, `+0x218` |

**A-033 source-bounded check:** the binary windows of these four helpers contain one immediate `disp32` reference apiece to each of `+0x208`, `+0x218`, `+0x248`, and **zero** immediate `disp32` references to `+0x228` or `+0x238`. This is an exact byte pattern check across all four full helper bodies, not a guessed C++ reconstruction. It does *not* rule out other functions, derived-pointer arithmetic, indirect consumers or branches outside the scanned bodies.

The latter two lists **are not dead allocations**: the shader-resource teardown explicitly passes all five list backing pointers, including `+0x238` and `+0x228`, through the same memory-release helper `0x0CA4B00`:

- Teardown beginning `0x0CEB330` reads and releases pointers at `+0x248` (`0x0CEB38A` / call `0x0CEB391`), `+0x238` (`0x0CEB396` / `0x0CEB39D`), `+0x228` (`0x0CEB3A2` / `0x0CEB3A9`), `+0x218` (`0x0CEB3AE` / `0x0CEB3B5`) and `+0x208` (`0x0CEB3BA` / `0x0CEB3C1`).
- An independent destructor-like path beginning `0x0CEB3F0` repeats the five pointer loads/calls at `0x0CEB44A...0x0CEB481`.
- Release routine `0x0CA4B00` has an explicit null-pointer test at `0x0CA4B0A` and a routed allocator/virtual free path; it is not a shader descriptor writer.

Other teardown candidates near `0x0CEB8D3` and `0x0CEB973` show the same five-field release order. Therefore **retain and release all five lists** in any guest-visible resource model even if the initial native rendering bridge only *consumes* three.

## A directly implementable native-port boundary

One minimal, source-backed guest resource shape is:

```
SerializedShaderBindingState {
    group_mask: u32,              // resource+0x200; already serialized
    lists[5]: Vector<u32>,        // headers at +0x208,+0x218,+0x228,+0x238,+0x248
}
```

Its guest layout and lifetime must be preserved independently of the Vulkan backend. The *observed* draw-time consumer uses list 0 (resource adapter), list 1 (sampler) and list 4 (polymorphic 32-byte resource path), but there is presently **insufficient evidence to name the semantics of lists 2 and 3**. Do not drop them from save/load, serialization, teardown or guest memory just because the four bound dispatchers do not read them directly.

This also narrows what a live A-028R/SC-series join should capture: shader-resource identity; `+0x200` group mask; group-pending intersection; the **specific active list** and raw packed entry; resulting object array index, logical API slot, selected guest resource object, 16/32-byte descriptor and writebacks. `GnmInputUsageSlot` metadata declares which binding is expected; the serialized list selects *which guest object* satisfies it, as shown independently in A-031.

## Remaining work / warnings

1. Original resource class names and the source archive/package are unknown. A successful deserializer call is not proof a particular UE4 file format was involved.
2. The positive consumers of `shader_resource+0x228/+0x238` remain unidentified. The bounded absence in four helpers cannot support a global "unused" claim.
3. Resource-provider `+0x28/+0x30/+0x38` constructor identity and the exact meaning of adapter writebacks remain unresolved.
4. No original shader hash, guest resource object identity or actual Vulkan resource was *dynamically joined* during this pass.

**Next experiment:** search for list-2/list-3 consumption through other vtables and/or direct resource-reading helper call sites, or obtain a tiny live snapshot of their serialized counts and list entries. If both remain empty for a representative shader set, that is a bounded runtime observation rather than a static proof of dead fields. Keep shadPS4 as observation infrastructure, not as a native rendering requirement.