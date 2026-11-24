# A-040 — Virtual provider to native resource descriptor

**Result: the provider-to-descriptor edge is proven on genuine gameplay, for both live object
classes, with the descriptor captured by value at the original writer boundary.**

A-039 stopped at the resolved virtual target. This phase crosses it: the provider is actually
executed, its return value is observed, the adapter consumes that exact value, and the 32-byte
descriptor it produces is captured before the original writer copies it.

## Provenance

| Item | Value |
| --- | --- |
| Branch | `research/a040-provider-descriptor-recovery` |
| Base | `research/a039-gameplay-resource-binding` @ `506174f5` |
| Executable | Déraciné 1.00 SELF, 81,506,767 bytes |
| SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Scene | classroom gameplay, headless |
| Scene gate | 1,855 draws / 872 Mpx at 67 s |
| Run | 90 s bounded, clean exit |
| Capture | `a040-gameplay-3` |

## 1. The complete chain

```
Grouped binder graphics-1 (0x0CC0300)
  ↓  pending 0x00000004, group 2, packed entry 0x02000000
Selected guest object 0x10012ffc80
  ↓  vptr 0x803ee1590 (RVA 0x03EE1590)
Virtual +0x38 target 0x800cf9e20 (RVA 0x0CF9E20)
  ↓  executed; returns object+0x60
Provider result 0x10012ffce0
  ↓  passed as rsi to the adapter
Descriptor adapter 0x800cc13b0 (RVA 0x0CC13B0)
  ↓  reads *(provider_result+0x18)
Descriptor source 0x10013606c0
  ↓  tail-jump with r8 = that pointer
32-byte resource writer 0x802f62a40 (RVA 0x02F62A40)
  ↓
Descriptor bytes captured by value, 32 bytes
```

Every edge belongs to the same guest invocation. The binder record and the chain record agree on
the selected object, the vptr and the provider target on **64/64** records.

## 2. Corrections to A-039

### The worked example mixed two runs

A-039's summary showed cached group `0x100b9d91c0` and object `0x100b9cfc80`. **Neither appears in
the committed `a039-gameplay-7` fixture.** The fixture's objects are `0x100536fc80` and
`0x1006cb4de0`, and its cached groups are in the `0x1005327...` range.

The addresses came from an earlier exploratory run and were carried into the report without being
re-derived from the committed evidence. The report now uses fixture sequence 5, which is in the
committed file.

### The stage-record attribution was wrong

A-039 associated shader `bac57337904948a7` with a 196-byte stage at `0x2000068600`. A-038R's own
fixture says otherwise:

| Shader | A-038R stage | A-038R size | A-039 claimed |
| --- | --- | ---: | --- |
| `bac57337904948a7` | `0x20000a5400` | **492** | 196 at `0x2000068600` |
| `d5050ca2fb2370b9` | `0x2000068600` | **196** | — |

The 196-byte stage belongs to `d5050ca2fb2370b9`, not to `bac57337904948a7`. A-039's report
attached the wrong stage record to the shader.

**The A-039 fixture does not carry stage data at all**, so the claim was never supported by the
committed evidence. The corrected report removes it. The shader hash itself is unaffected: it was
read from the OrbShdr trailer at the address the stage record named, and that read is what produced
`bac57337904948a7`.

### The declaration join was never established

A-039's headline said "from shader declaration". The fixture records shader hashes and packed
binding entries, but not parsed GNM declarations or slot-map entries. The declaration-to-grouped
join is **not** proven, and the corrected headline says so.

A-040 does not close that gap either. What it closes is the provider-to-descriptor half.

## 3. Static verification of the provider and adapter

All addresses verified against the approved SELF before any hook was installed.

### The two provider methods

| Method | RVA | Machine code | Returns |
| --- | --- | --- | --- |
| `0x0CF9E20` | `0x0CF9E20` | `lea 0x60(%rdi),%rax; ret` | `object+0x60` |
| `0x0CFFEC0` | `0x0CFFEC0` | `mov 0x58(%rdi),%rdi`; null → global; `jmp *0x38(%rax)` | its inner object's inline result |

The first is a leaf that returns an inline subobject. The second delegates to another object's own
`+0x38` method, so its result is **not** `selected_object+0x60`.

### The adapter

`0x0CC13B0`, ABI verified from its own prologue and body:

```
rdi = graphics context
rsi = provider result
edx = logical API slot
ecx = count
```

It advances `rsi` by `0x18`, optionally consults a descriptor-derived context cache at
`context+0x3D8/+0x410/+0x418/+0x420`, then tail-jumps at `0x0CC14E0` to `0x02F62A40` with
`r8 = *(provider_result+0x18)`.

**The binder's own code confirms the handoff.** At `0x0CC0437` it executes `callq *0x38(%rax)`,
then at `0x0CC0442` moves the return value into `rsi` and calls `0x0CC13B0`. The provider result
reaches the adapter as a direct register move, not through memory.

### The writer

`0x02F62A40` is the 32-byte immediate-resource writer. Its prologue and packet construction were
verified in A-030 and re-checked here. It consumes 8 DWORDs and advances the packet cursor by
`0x30`.

## 4. Runtime results

| Item | Value |
| --- | ---: |
| Adapter invocations | 4,820,459 |
| Adapter invocations without a matching provider | 4,820,395 |
| Complete provider chains | **64** |
| Chains with a 32-byte descriptor | 64 |
| Distinct object classes | 2 |
| Distinct shader hashes | 2 |

The high unmatched count is expected: the adapter has many callers, and only the grouped binder's
provider results are tracked. An unmatched call is counted and discarded, never attributed.

### Both classes complete

| vptr | Provider target | Shader | Chains | Descriptor |
| --- | --- | --- | ---: | --- |
| `0x03EE1590` | `0x0CF9E20` | `bac57337904948a7` | 42 | 32 bytes |
| `0x03EE1730` | `0x0CFFEC0` | `1c65203818af3aeb` | 22 | 32 bytes |

The first class satisfies `provider_result == object+0x60` on 42/42. The second satisfies it on
**0/22**, which is the correct result: its provider delegates, so its result is its inner object's
inline result instead. That asymmetry is itself evidence that the two classes are genuinely
different and that the matching rule is not a coincidence.

## 5. What each edge is

| Edge | Classification | Basis |
| --- | --- | --- |
| Binder dispatch and pending mask | **Observed** | read at the helper's entry |
| Group selection | **Verified derived** | recomputed, 64/64 |
| Packed entry decode | **Verified derived** | recomputed, 64/64 |
| Selected object and vptr | **Observed** | read from the object array |
| Provider target | **Observed** | read from the live vtable |
| Provider execution | **Observed** | the adapter received its return value |
| Provider result | **Observed** | adapter's `rsi` |
| Descriptor source | **Observed** | `*(provider_result+0x18)` |
| Descriptor bytes | **Observed** | copied by value at the adapter boundary |
| Adapter and writer identity | **Observed** | verified RVAs |
| Logical API slot agreement | **Verified derived** | 64/64 |
| Shader hash | **Observed** | read from the OrbShdr trailer |
| Declaration-to-grouped join | **Unverified** | not captured |
| Descriptor semantics | **Unverified** | width and bytes only |
| Timestamp writeback ordering | **Unverified** | not captured this phase |

## 6. What is not claimed

- **The descriptor's meaning is unknown.** 32 bytes is a GNM descriptor width, not a resource class.
  It is not established to be a texture, a buffer or a UAV.
- **The declaration join is not established.** The chain starts at the shader identity, not at a
  parsed GNM declaration.
- **The timestamp writeback was not observed this phase.** A-039 captured values at binder entry;
  A-040 did not add the before/after transition.
- **The second class's inner object was not captured.** Its provider delegates through
  `object+0x58`; the inner object's identity is read during matching but not recorded.
- **No comparison against shadPS4's Vulkan descriptors.** Not attempted.

## 7. Native resource-provider contract, from verified behaviour only

**The provider interface:**

- A resource object exposes a virtual method at vtable slot `+0x38`.
- Its signature is `void* method(object* this)` under the guest SysV convention.
- The return value is a pointer to a provider result object.
- Two implementations are observed:
  - **inline**: returns `this+0x60`, a subobject of the resource object itself.
  - **delegating**: reads `this+0x58`, falls back to a global when null, and tail-jumps through
    that object's own `+0x38`.
- The provider result carries a descriptor pointer at `+0x18`.

**The adapter interface:**

- `adapter(context, provider_result, api_slot, count)`.
- It reads `*(provider_result+0x18)` as the descriptor source.
- It may consult a per-context descriptor cache before binding.
- It terminates in a typed writer selected by descriptor width, not by resource class.

**What a native adapter must preserve:**

- The provider is a virtual call whose target differs per object class. It cannot be flattened.
- The provider result is an intermediate object, not the descriptor. The descriptor is one field
  inside it.
- The descriptor is 32 bytes for both observed classes, but that is an observation about these two
  classes, not a rule.
- The logical API slot flows from the packed binding entry through the adapter to the writer.

**What it must not assume:**

- That every provider returns `this+0x60`. The delegating class does not.
- That a 32-byte descriptor is a texture.
- That one adapter serves all resource classes. `0x0CAC030` is a separate polymorphic adapter for
  the `+0x208` list and was not exercised here.

## 8. Verification

```
python tools/atlas/verify_a040_provider_descriptor.py maps/a040_provider_descriptor_chains.json
```

**22/22 checks pass.** The verifier recomputes every relationship from recorded primitives.

Ten negative modes are each rejected:

| Mode | Corrupts | Rejected by |
| --- | --- | --- |
| `object` | selected object | binder/chain agreement, return rule |
| `provider-target` | provider target | agreement, return rule, one-vptr-one-target |
| `provider-result` | provider result | return rule |
| `binder-link` | binder sequence | link check |
| `adapter-slot` | adapter API slot | slot agreement |
| `descriptor-source` | descriptor source | non-null check |
| `descriptor-width` | descriptor width | 32-byte check |
| `descriptor-fingerprint` | fingerprint | placeholder detection |
| `writer-target` | writer target | verified-RVA check |
| `thread` | guest thread | thread agreement |

The `thread` mode initially passed silently because the fixture omitted `guest_thread` from grouped
records. The fixture now carries it.

### Tests

| Suite | Tests | Result |
| --- | --- | --- |
| `shadps4_gnm_shader_metadata_test` | 12 | pass |
| `shadps4_gnm_upload_provenance_test` | 13 | pass |
| `shadps4_gnm_grouped_binding_test` | 11 | pass |
| `shadps4_gnm_provider_descriptor_test` | 9 | pass |

## 9. Reproducible capture

```
pwsh -File tools/headless-vr/headless-vr-gameplay.ps1 \
  -Exe <build>/shadPS4.exe -Label a040-gameplay-3 -Gate gameplay \
  -Eboot D:\Temp\Deracine\1.0\Image0\eboot.bin \
  -Shadow <private shadow with matching eboot hash> \
  -SourceLab <lab> -SavedataSource <save slot 1> \
  -LockScript <gpu-lock.sh> \
  -Env 'SHADPS4_DERACINE_GNM_BINDING=1' \
  -Seconds 40 -CleanExit -ExitAfterSeconds 95
```

The harness reports `valid=False` for a scene-band mismatch (Mpx 872–914 against a 550–750 band).
That is a pinned benchmark band, not a scene failure: the gameplay gate matched at 1,855–1,868
draws on every attempt. It does not affect the semantic evidence.

## 10. Remaining uncertainties

1. **The declaration-to-grouped join is open.** The chain starts at the shader identity.
2. **Descriptor semantics are unknown.** Width and bytes only.
3. **The timestamp transition was not observed.** A-039's entry-time values stand; A-040 did not
   add the before/after.
4. **The delegating class's inner object is not recorded.** Only used for matching.
5. **`0x0CAC030` was not exercised.** The `+0x208` list path is separate.
6. **One scene.** The classroom.
7. **No frame-rate claim.** Observation overhead was not measured against a control.

## 11. Next architectural unknown

The declaration-to-grouped join. The chain now runs from shader identity to descriptor bytes; what
it does not show is which GNM declaration in that shader's OrbShdr table corresponds to the packed
binding entry that selected the object.

That needs the shader's parsed declaration table captured alongside the binder record, and a rule
that maps a declaration's `api_slot` to the packed entry's `api_slot`. A-038R already parses those
tables; the missing piece is capturing them at the binder rather than at the writer.

That is the smallest next experiment. It is not started.
