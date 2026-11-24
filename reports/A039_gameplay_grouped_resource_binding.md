# A-039 — Gameplay grouped-resource binding recovery

**Result: the grouped resource-object binding path is exercised during real gameplay, and one
complete chain is proven from shader declaration through the selected guest object to the resolved
virtual provider target.**

A-038R found the grouped binder never dispatched on the title screen. That was a bounded result
about that scene, not about the game. In the classroom it dispatches constantly.

## Provenance

| Item | Value |
| --- | --- |
| Branch | `research/a039-gameplay-resource-binding` |
| Base | `research/a038r-binding-provenance` @ `324034f4` |
| Executable | Déraciné 1.00 SELF, 81,506,767 bytes |
| SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Scene | classroom gameplay, headless |
| Scene gate | 2,017 draws / 864 Mpx at 60 s |
| Run | 90 s bounded, clean exit |
| Capture | `a039-gameplay-7` |
| Save | slot 1, `m00_00_10_10`, all four files hash-verified against the pinned manifest |

## 1. Which grouped helpers dispatch

All five recovered helpers were instrumented, each with its own context offsets. Every invocation
is counted, so a zero result is a measurement.

| Helper | RVA | Dirty mask | Cache array | Invocations | Nonzero pending |
| --- | --- | --- | --- | ---: | ---: |
| graphics-0 | `0x0CC0070` | `+0x928` | `+0x568` | 2,685,838 | **0** |
| **graphics-1** | **`0x0CC0300`** | **`+0x934`** | **`+0x748`** | **1,740,764** | **1,359,297** |
| graphics-2 | `0x0CC0670` | `+0x92C` | `+0x608` | 0 | 0 |
| graphics-3 | `0x0CC09E0` | `+0x938` | `+0x7E8` | 6,412 | 0 |
| compute-0 | `0x0CC71A0` | `+0x93C` | `+0x888` | 124,737 | 0 |

**`graphics-1` at `0x0CC0300` is the gameplay binder.** It dispatched 1,359,297 times with a
nonzero pending mask. The helper A-038R observed, `graphics-0`, is called even more often and never
dispatches.

`graphics-2` was never called at all. That is a bounded observation about this scene, not a claim
that it is dead.

Each helper's prologue and both context offsets were verified against the original binary before
use. The offsets are distinct per helper, which is why a single shared constant would have been
wrong.

## 2. The proven chain

```
Shader bac57337904948a7
  ↓  context+0x468 → shader state → +0x200 primary resource → +0x1D8 stage record
  ↓  stage record: size 196, program 0x2000068600 → OrbShdr trailer
Original GNM declaration and logical API slot
  ↓
Grouped binding entry 0x02000000  (api_slot 0, object_index 0, group 2)
  ↓  pending 0x00000004, group 2 = lowest set bit
Cached group 0x100b9d91c0 → object array 0x100f2eeb60
  ↓  array[0]
Selected guest object 0x100b9cfc80
  ↓  vptr 0x803ee1590 (RVA 0x03EE1590)
Virtual +0x38 target 0x800cf9e20 (RVA 0x0CF9E20)
```

Every edge is an observed value read at the binder's entry, not an inference from co-occurrence.

### Verification of each edge

| Check | Result |
| --- | --- |
| `pending == dirty_mask & group_mask` | 64/64 |
| `group == lowest set bit of pending` | 64/64 |
| Packed entry decodes to its recorded fields | 64/64 |
| Packed entry group == mask-selected group | 64/64 |
| Object vptr non-null and in the loaded image | 64/64 |
| Provider target non-null and in the loaded image | 64/64 |
| Each vptr resolves to exactly one provider target | 2 classes, no conflict |
| Shader hash resolved | 64/64 |

## 3. Two object classes, each serving one shader

| Object | vptr (RVA) | Provider target (RVA) | Shader | Records |
| --- | --- | --- | --- | ---: |
| `0x100536fc80` | `0x03EE1590` | `0x0CF9E20` | `bac57337904948a7` | 42 |
| `0x1006cb4de0` | `0x03EE1730` | `0x0CFFEC0` | `1c65203818af3aeb` | 22 |

**The first vptr matches A-036's candidate `0x03EE1590` exactly.** That is a static candidate now
confirmed as a live runtime object class. The second, `0x03EE1730`, is **not** among the three
candidates the task listed (`0x03EE1590`, `0x03EE15F0`, `0x03EE1650`). It is a newly observed
class.

I am not claiming a concrete C++ type for either. A matching vtable address is a class identity, not
a semantic one. What is established is that two distinct object classes serve two distinct shaders
through two distinct provider methods, consistently.

## 4. The timestamp writeback

A-035 predicted a writeback through `object+0x40` to a double, with compatible constructors
initializing that pointer to an inline double at `object+0x48`.

**Both predictions hold on live objects.**

| Object | `+0x40` pointer | Offset from object | Inline? |
| --- | --- | --- | --- |
| `0x100536fc80` | `0x100536fcc8` | **`+0x48`** | yes, inline |
| `0x1006cb4de0` | `0x100bd614a8` | `+0x50ac6c8` | no, external |

The first object's timestamp pointer is exactly `object+0x48`, which is A-035's inline-double
constructor case. The second points elsewhere, which is the other case A-035 allowed for.

### The values are float-rounded doubles

A-035 states the stored value is `double(float(process_time))`. Re-rounding each observed value
through float is a no-op on **64/64** records. A value produced any other way would not survive that
test.

### The first value is a sentinel

Both objects' first observed value is `0xC7EFFFFFE0000000`, which is `-FLT_MAX` widened to double.
That is a "never used" marker, and the binder overwrites it on first use. Subsequent values are
monotonic and track process time:

```
object 0x100536fc80:  -FLT_MAX → 2.343962 → ... → 4.401441
object 0x1006cb4de0:  -FLT_MAX → 4.319533 → ... → 4.401441
```

### What is not established

The writeback is **observed before** the provider call, which matches A-035's ordering. Whether the
provider reads the updated value, and whether any downstream consumer uses it for residency,
eviction or identity, is **not established**. I did not modify timestamps or inject values.

## 5. How the shader identity was resolved

The binder's `rsi` is the shader resource that owns the group mask and binding lists. Its `+0xB0`
is **not** the metadata the typed writer reads. I dumped the object and confirmed it is a different
family: its first word is a vtable, not an OrbShdr-bearing metadata block.

The shader identity therefore comes from the context's own shader state, which SC-02 established
independently:

```
context+0x468 → shader state
  +0x200 → primary shader resource   (null on this path)
  +0x1E0 → alternate shader resource (used when primary is null)
  +0x1D8 → stage record
```

The stage record's 23-bit size and word 2 give `program + size - 0x1C`, which lands on the OrbShdr
trailer. This is the same arithmetic A-028R validated on 248 shaders and A-038R confirmed on the
constant-buffer path.

**This is a correction to a natural assumption.** The binder's shader resource and the writer's
metadata object are different objects. Conflating them yields no shader hash, which is what the
first two gameplay captures produced.

## 6. What each edge is

| Edge | Classification | Basis |
| --- | --- | --- |
| Helper dispatch and pending mask | **Observed** | read at each helper's entry |
| Group selection | **Verified derived** | recomputed from pending, 64/64 |
| Packed entry fields | **Observed** | read from the binding list |
| Packed entry decode | **Verified derived** | recomputed, 64/64 |
| Cached group and object array | **Observed** | read at the helper's own offsets |
| Selected object | **Observed** | read from the object array |
| Object vptr | **Observed** | read from the live object |
| Provider target | **Observed** | read from the live vtable at `+0x38` |
| Shader hash | **Observed** | read from the OrbShdr trailer |
| Timestamp pointer and value | **Observed** | read before the provider call |
| Timestamp float-rounding | **Verified derived** | recomputed, 64/64 |
| Provider result | **Unverified** | not captured this phase |
| Descriptor from this path | **Unverified** | not captured this phase |
| Object class semantics | **Unverified** | vptr identity only |
| Timestamp downstream consumer | **Unverified** | not traced |

## 7. The missing edge

The chain is proven **to the provider target**, not through it.

The task's full chain continues:

```
provider result → descriptor source → original typed writer
```

That continuation is **not** captured. The provider at `0x0CF9E20` / `0x0CFFEC0` is a virtual
method; observing its return requires either a return observer or a hook at the adapter that
consumes its result. Neither was implemented this phase.

The constant-buffer writer path A-038R proved is a **different** mechanism: the deferred-upload
caller at `0x0CC0D50`, not the grouped binder. The two do not share a writer invocation in this
capture.

So the honest statement is: **the grouped binder selects a real object and resolves a real provider
method, and the descriptor that provider produces is not yet observed.**

## 8. Native-adapter contract, from verified behaviour only

What a native adapter must preserve, and what it must not assume:

**Must preserve:**

- The five helpers are distinct, with distinct context offsets. They are not one function.
- Dispatch is gated by `dirty_mask & group_mask`. A zero result means no binding work.
- The group is the lowest set bit of that intersection.
- The packed entry is `{api_slot: u8, object_index: u16, group: u8}`. It is **not** a
  `GnmInputUsageSlot`, though both are four bytes.
- The selected object comes from `cached_group[group]+0x20` indexed by `object_index`.
- The provider is a virtual call through the object's own vtable at `+0x38`. The target differs
  per object class.
- The timestamp at `*(object+0x40)` is written as `double(float(t))` before the provider call, and
  starts at `-FLT_MAX`.

**Must not assume:**

- That one helper serves all stages. `graphics-1` dispatched; `graphics-0` did not.
- That the binder's shader resource is the writer's metadata object. It is not.
- That a vptr identifies a resource class semantically. It identifies a class, not a meaning.
- That every declaration produces a binding. The dirty mask gates it.
- That the timestamp affects identity. That is unestablished.

## 9. Verification

```
python tools/atlas/verify_a039_grouped_binding.py maps/a039_grouped_binding_chains.json
```

**20/20 checks pass** on the committed fixture. The verifier recomputes every relationship from
recorded primitives. It does not trust stored booleans.

Eight negative modes are each rejected:

| Mode | Corrupts | Rejected by |
| --- | --- | --- |
| `packed-entry` | packed entry | decode recomputation |
| `group` | selected group | lowest-set-bit recomputation, group agreement |
| `object-index` | object index | decode recomputation |
| `vptr` | object vptr | one-vptr-one-target mapping |
| `provider-target` | provider target | image-range check |
| `timestamp` | timestamp value | float-round-trip recomputation |
| `shader-hash` | shader hash | non-zero hash check |
| `pending` | pending mask | mask-intersection recomputation |

Two of these initially passed silently. The `vptr` mode added a constant that still mapped
one-to-one, and the `timestamp` mode used `1.0`, which is exactly float-representable. Both were
fixed to corrupt a property the check actually tests.

### Tests

| Suite | Tests | Result |
| --- | --- | --- |
| `shadps4_gnm_shader_metadata_test` | 12 | pass |
| `shadps4_gnm_upload_provenance_test` | 13 | pass |
| `shadps4_gnm_grouped_binding_test` | 11 | pass |

The grouped suite pins the helper table, the group selection, the packed-entry decode, and that a
packed entry is not a GNM input-usage slot.

## 10. Reproducible capture

```
pwsh -File tools/headless-vr/headless-vr-gameplay.ps1 \
  -Exe <build>/shadPS4.exe -Label a039-gameplay-7 -Gate gameplay \
  -Eboot D:\Temp\Deracine\1.0\Image0\eboot.bin \
  -Shadow <private shadow with matching eboot hash> \
  -SourceLab <lab> -SavedataSource <save slot 1> \
  -LockScript <gpu-lock.sh> \
  -Env 'SHADPS4_DERACINE_GNM_BINDING=1' \
  -Seconds 40 -CleanExit -ExitAfterSeconds 95
```

The harness reports `valid=False` on these runs for a scene-band mismatch (Mpx 837–933 against a
550–750 band). That is a pinned-band difference, not a scene failure: the gameplay gate itself
matched at 1,816–2,062 draws on every attempt. The band is a benchmark calibration and does not
affect the binding evidence.

## 11. Remaining uncertainties

1. **The provider result and descriptor are not captured.** The chain stops at the resolved virtual
   target.
2. **The second object class is new.** `0x03EE1730` is not among the A-036/A-037 candidates.
3. **No semantic class identity.** A vptr match is not a C++ type claim.
4. **The timestamp's downstream consumer is unknown.** The write is observed; its effect is not.
5. **`graphics-2` was never called.** Bounded to this scene.
6. **One scene.** The classroom. Other scenes may exercise different helpers.
7. **No descriptor comparison against shadPS4.** Not attempted; not required for this proof.

## 12. Next architectural unknown

The next question is what the provider returns and how that becomes a descriptor. That needs a
return observer on `0x0CF9E20` / `0x0CFFEC0`, or a hook at the adapter that consumes the result,
tied to the same invocation identity this phase established.

That is the smallest next experiment. It is not started.
