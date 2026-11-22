# Independent Déraciné Binary Atlas A-032 — Deserialized grouped-binding lists and polymorphic descriptor adapter

**Target:** Original Déraciné PS4 1.00 SELF; 81,506,767 bytes; SHA-256 `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`. All executable addresses are inner ELF VAs, SELF offset = VA + `0x1FD30`. **Static only.** No live draw/shader hash to object correlation, and no game data committed.

**Verification:** `python verify_a032.py PRIVATE_original_eboot.bin` → **79/79 passed** locally. Breakdown: 60 fixed original instruction-byte anchors; 11 resolved E8 calls to the shared deserializer; two resolved E9 tail jumps to typed writers; one original entry dispatch call; five arithmetic checks on the five vector-header offsets (74 checks from original bytes/control flow, five from arithmetic invariants).

## Result 1 — The 0x0CAC030 adapter is NOT a single descriptor class

A-031 identified the `shader_resource+0x208` grouped list and its call to `0x0CAC030` at `0x0CC0222`. The adapter's entry contract is **RDI = graphics context, RSI = selected resource object, EDX = logical API slot, ECX = stage/mode selector**. Its first test returns immediately for a null object (`0x0CAC04A`), so not every packed list entry generates a writer call.

The selected object has three optional, distinct providers:

- `[object+0x30]` is a provider object; virtual slot **`+0x38`** returns a secondary object (`0x0CAC05D..0x0CAC06C`). The adapter probes a DWORD at `returned+0x08` to choose a binding path.
- When the provider is absent, it examines `[object+0x28]+0x08`, or `[object+0x38]+0x20` when the former is null (`0x0CAC081..0x0CAC09D`). These are *alternative internal representations*, not proof of UE4 object type inheritance.
- The adapter tests state codes **0, 2 and 4** at `0x0CAC09F..0x0CAC0D5`, with additional conditional checks and a per-context hashed cache (`context+0x3D8/+0x410/+0x418/+0x420`) prior to binding. The cache operations are present; their exact high-level identity/invalidation semantics are not proven.

It has **two distinct terminal writer paths**:

| Exit | Original evidence | Writer class |
| --- | --- | --- |
| `0x0CAC3E8` | Working 16-byte region derived from object `+0x14/+0x18` or its alternate backing (`0x0CAC344..0x0CAC3D2`); `E9` tail jump | `0x02F62740` — immediate resource **16-byte** writer |
| `0x0CAC53A` | Non-null provider `[object+0x30]` chooses working data at `object+0x40` (`0x0CAC502..0x0CAC529`); `E9` tail jump | `0x02F62A40` — immediate resource **32-byte** writer |

Important native-port warning: the 16-byte path can **write back** two DWORDs (`0x0CAC3A4`, `0x0CAC3B7`) to the current working resource object, not merely read a descriptor. One fallback advances the working source pointer by `+0x14` before the writes (`0x0CAC392`), so avoid treating the write offsets as fixed on every branch. A native semantic intercept that skips this adapter must preserve or demonstrate dispensability of these guest-visible cache updates. The 32-byte branch is not proven to mutate the same fields.

**Correction to A-031:** the middle list is *polymorphic immediate-resource binding*, selecting 16/32-byte writer pathways; it is **not** verified to be a sampler or buffer-specific list. The prior `+0x218` sampler list remains a separate typed 16-byte pathway.

## Result 2 — Five grouped binding lists originate in a shared stream deserializer

The shared initializer at **`0x0CF0580`** takes `RDI = stream-like reader`, `RSI = destination list object`. Destination layout is independently confirmed by accesses at `0x0CF05A5`, `0x0CF0757`, `0x0CF0780` and `0x0CF079D`: pointer at `+0`, count at `+0x08`, capacity at `+0x0C` (16-byte vector header). It reads a count and then reads **four bytes per list item** using either a buffered reader cursor or a virtual reader method at slot `+0x50`; the initial reader's vtable `+0x78` method participates in count/capacity handling. Reader flag `[reader+0x29] & 8` enables byte swaps on DWORDs. These are observable stream-reader operations; an exact source file type, archive format or on-disk format is not identified.

Original shader-resource construction paths prepare the following **five adjacent list holders**, not just the three consumed in A-031:

| Resource field | Intended structure from code | Status |
| --- | --- | --- |
| `+0x208` | 16-byte vector header, 4-byte entries | Passed as destination of first deserialization call |
| `+0x218` | ditto | Passed as second call; A-031 sampler input |
| `+0x228` | ditto | Third call; runtime consumer unclassified |
| `+0x238` | ditto | Fourth call; runtime consumer unclassified |
| `+0x248` | ditto | Fifth call; A-031 polymorphic/resource binding group |

At the constructor near `0x0CF169D`, first list address `resource+0x208` is loaded; calls `0x0CF1767`, `0x0CF1777`, `0x0CF1787`, `0x0CF1797`, `0x0CF17A7` all resolve to **`0x0CF0580`**, with the subsequent four list addresses explicitly reloaded between calls (`0x0CF176C`, `0x0CF177C`, `0x0CF178C`, `0x0CF179C`). Another constructor repeats the same five-call pattern at `0x0CF3E56/66/76/86/96`. `0x0CF333F` is an independent further call within another similar constructor. These are *creation-time* edges; the draw dispatcher reads the resulting `[shader_resource+offset]` pointers later.

The original list entries are read through a 32-bit stream interface, whereas runtime A-031 dispatch decodes each 32-bit packed binding assignment into group, object-array index and logical slot. This provides a plausible **serialized shader-binding metadata → live binding object selection** connection, rather than reconstructing bindings from PM4. The interpretation of the upstream source as a particular UE4 shader-cache file/package remains **unproven**.

## Native renderer implications

A native adapter should expose the **logical declaration**, **serialized list entry**, **selected resource object**, **descriptor representation/width**, **cache writeback**, and **effective Vulkan resource** as different pieces of information. It cannot assume each declaration corresponds to a draw-time writer call: dirty-group checks may skip unchanged groups, and `0x0CAC030` can return for null/ineligible source objects.

The critical next proof is a *live* join for one known A-028R shader: a shader-resource identity, a non-empty `+0x208` list entry, the source object and the chosen 16/32-byte descriptor path. Static analysis does **not** establish such a join. This work is independent of shadPS4's PM4 execution; shadPS4 may serve as an observational oracle but is not required by the eventual native renderer.

## Focused next question

Determine which constructors of the source-object `+0x28/+0x30/+0x38` alternatives populate each representation and whether the writebacks at `0x0CAC3A4/+0xCAC3B7` cache GPU addresses, resource identity, or another state. Also trace the consumption of the currently unclassified `+0x228` and `+0x238` list families.