# A-038R — Binding provenance and capture hardening

**Result: the provider-to-writer association is now causal, the executable identity is enforced, and
the committed evidence reproduces the verifier.**

A-038's headline claimed the chain was complete through a resource provider. It was not: the upload
object was a global "last seen" value, so any writer inherited whatever upload ran most recently.
This phase replaces that with a per-thread, stack-shaped association matched on the provider's
output buffer, and narrows the claim to what is actually proven.

## Provenance

| Item | Value |
| --- | --- |
| Branch | `research/a038r-binding-provenance` |
| Base | `research/a038-binding-correlation-20261123` @ `e079be6e` |
| Executable | Déraciné 1.00 SELF, 81,506,767 bytes |
| SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Local path | `D:\Temp\Deracine\1.0\Image0\eboot.bin` |
| Runtime | shadPS4 fork, headless, `SHADPS4_VR=1 SHADPS4_OPENXR=0` |
| Scene | title screen, 90 s bounded run, clean exit |
| Capture | `a038r-7` |

## 1. The A-038 defect, and what replaced it

A-038 recorded `last_upload_object`, `last_upload_size` and `last_upload_valid` in global state.
A mutex made that data-race-safe but not causal: another thread, a nested call or an intervening
upload could hand a writer the wrong object.

The replacement is a per-thread stack of pending invocations. The deferred-upload caller at
`0x0CC0D50` pushes one entry naming the upload object and the exact stack buffer the provider will
fill. The writer pops the entry whose expected buffer equals its source. Nothing is inherited.

The caller's buffer address is derived from its own prologue, not guessed:

```
push rbp; mov rsp,rbp; push r15,r14,r13,r12,rbx; sub rsp,0x28
provider output buffer = rbp-0x40 = entry_rsp - 0x48
```

The writer is then called with that same address as `r8`. That equality is the causal link, and it
is checked rather than assumed.

### Why the provider is not the observation point

I first hooked the provider at `0x0CC1010`, which receives the object in `rdi` and the output
buffer in `rcx`. That is the more natural place, and it failed: the provider has many callers, most
of which never reach this writer, so its stack never drained. The run showed 2,054,826 overflows and
zero matches. The caller is the correct boundary because it is the specific path that reaches the
16-byte constant-buffer writer.

### Results

| Item | Value |
| --- | --- |
| Writer invocations | 3,459,195 |
| Records stored | 256 |
| Records resolved to a shader hash | 247 |
| Records with a matched declaration | 247 |
| **Causally matched upload records** | **27** |
| Unmatched writer records | 229 |
| Upload stack overflows | 0 |
| Invalid upload objects skipped | 538,275 |

Of the 27 matched records, 18 carry a real shader hash and a matched declaration. The other 9 have
a null hash because their stage record's program-data pointer was null, which is recorded rather
than papered over.

## 2. Executable identity is now enforced

A-038 declared `ApprovedImageSize` and `ApprovedImageSha` and never used them. The guard now
hashes the file the loader actually selected (`module.file`), before any observer is installed.

It does not hash the relocated in-memory image, because that is not the SELF. It does not trust a
hard-coded pathname. It fails closed on an unreadable file, a size mismatch, a hash mismatch, or the
known alternate image.

**Negative test, run:** the 1.01 image
(`8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75`) was refused before patching:

```
GNM binding capture refused: selected image is not the approved Deracine 1.00 SELF
  (verdict=alternate-image size=81506767 sha256=8da8e46a...)
```

No `gnm_binding` directory was produced. The prologues of that image match, which is exactly why a
prologue check cannot substitute for the full hash.

The existing VMA, module-name, title-id and exact-prologue checks are retained as secondary guards.

## 3. Hot-path work removed

A-038 checked capacity only after reading guest memory, resolving the shader and parsing the
declaration table. With 3.46 million writer calls and 256 stored records, that was 3.46 million
wasted record builds.

The capacity check now runs before any guest read. A full capture costs one atomic increment per
call.

One ordering subtlety had to be handled: the pending-upload drain must still run when the capture is
full, or the per-thread stack never empties and every later invocation is refused as overflow. The
drain is thread-local and allocation-free, so it runs unconditionally before the capacity check.

Measured at the observer level, not as a frame-rate claim:

| Metric | A-038 | A-038R |
| --- | --- | --- |
| Writer calls | 4,983,755 | 3,459,195 |
| Records stored | 256 | 256 |
| Work per dropped call | full record build | one atomic increment |
| Upload stack overflows | n/a | 0 |

The call counts differ between runs because the scene is not deterministic in draw volume. No
frame-rate improvement is claimed; none was measured.

## 4. The committed evidence now reproduces the verifier

A-038 committed a four-entry summary but reported a 12/12 result obtained from a separate private
export. The committed file and the verifier did not agree on schema.

That is fixed. `maps/a038r_binding_chain_fixture.json` is a sanitized, committed fixture with 67
records: all 27 matched records plus a 40-record sample of unmatched ones. It carries no descriptor
bytes, no shader code and no game assets. Descriptor identity is preserved as an FNV-1a fingerprint.

`tools/atlas/verify_a038r_binding_chain.py` reconstructs every conclusion from the recorded inputs.
It does not trust derived booleans. It recomputes:

- the encoded map word from the declaration's own fields;
- the declaration's API slot against the writer's slot;
- the map word's bit decomposition;
- descriptor width and fingerprint shape;
- `shader_resource + 0xB0 == map_object`;
- `trailer == code_address + stage_size - 28`;
- the matched record's source against its provider buffer;
- that unmatched records carry no upload object.

**19/19 checks pass on the committed fixture.**

### Negative fixtures

Six corruption modes, each targeting a record the relevant check actually examines:

| Mode | Corrupts | Rejected by |
| --- | --- | --- |
| `map-word` | decoded map word | encoder recomputation, bit decomposition |
| `slot` | writer API slot | declaration/writer slot agreement |
| `stage-size` | trailer address | stage-record arithmetic |
| `provider-source` | expected provider buffer | source/buffer equality |
| `descriptor-fingerprint` | fingerprint | placeholder detection |
| `declaration` | declaration register | encoder recomputation |

All six are rejected. An earlier version of the negative harness corrupted record 0, which was
unmatched, so five modes passed silently. The harness now selects a record the check examines.

## 5. What each edge is

| Edge | Classification | Basis |
| --- | --- | --- |
| Shader hash | **Observed** | read from the OrbShdr trailer at the computed address |
| Stage record size and program address | **Observed** | read at `shader_resource+0x1D8` |
| Trailer address | **Verified derived** | `code + size - 28`, checked on 247/247 |
| Declaration | **Observed** | parsed from the trailer's own table |
| Decoded map word | **Observed** | read from the writer's stage map object |
| Map word vs declaration | **Verified derived** | encoder recomputation, 247/247 |
| Descriptor bytes | **Observed** | copied by value at the writer boundary |
| Upload object | **Observed** | read at `context+0xAE0` |
| Provider output buffer | **Verified derived** | caller entry rsp minus 0x48 |
| Upload object → writer | **Observed causal** | source equals provider buffer, 27 records |
| Grouped `+0x38` provider | **Unverified** | binder never dispatches in this scene |
| Descriptor resource class | **Unverified** | bytes captured, meaning not decoded |
| Upload object ownership | **Unverified** | address only, no vtable or lifetime |

## 6. The grouped binder still never dispatches

The grouped binder at `0x0CC0070` fired 1,253,546 times with a **zero pending mask on every
call**. That is a bounded title-screen result, reported as such. It is not evidence that the grouped
path is dead in gameplay.

**The grouped virtual `+0x38` provider is not validated by this capture.** It is a different
mechanism from the deferred-upload caller at `0x0CC0D50`, and this report does not conflate them.

## 7. Independent A-035–A-037 assessment

The bundle `A035-A037-independent-atlas-bundle.zip` was provided and inspected. All three verifiers
were rerun against the approved SELF:

| Report | Checks | Result |
| --- | --- | --- |
| A-035 | 64/64 | pass |
| A-036 | 72/72 | pass |
| A-037 | 118/118 | pass |

Each gates on the exact approved size and SHA-256 before examining any code byte.

**Contradictions with A-038: none found.** The reports are careful about their own limits, and each
states that it does not establish runtime identity.

**A-035** resolves the `+0x40` writeback as a float-rounded process-time scalar written through a
pointer, and the provider's output as a wrapper whose `+0x18` feeds the 32-byte resource writer
`0x02F62A40`. It explicitly does not resolve the virtual target or the returned class.

**A-036** offers a descriptor resource-class candidate. It states the grouped binder's dynamic type
remains unproven.

**A-037** verifies the queued-command to group-cache handoff and several compatible provider
vtables. It states the cache-array entry to specific derived object remains unjoined.

These are static candidates, not runtime observations. They are **not** integrated as repository
commits in this phase, because integrating them would require a separate documentation commit and
they add nothing to the runtime proof. They are recorded here as reviewed and consistent.

The distinction the task asked for is preserved: the grouped `+0x38` provider is not the
deferred-upload provider at `0x0CC1010`, and neither is the other.

## 8. A-028R provenance correction

A-028R's 248-shader collection used `D:\Temp\Deracine\CUSA11919-qual\eboot.bin`, which is the
**alternate** image (`8da8e46a...`), not the approved 1.00 SELF.

Validation of the parser on that sample remains valid: the metadata is read from live guest memory,
and the parser is image-independent. But those 248 records must not be retroactively attributed to
the approved 1.00 SELF. This phase's capture used the approved image and is attributed to it.

## 9. Tests

| Suite | Tests | Result |
| --- | --- | --- |
| `shadps4_gnm_shader_metadata_test` | 12 | pass |
| `shadps4_gnm_upload_provenance_test` | 13 | pass |

The provenance suite covers the single-thread path, two threads interleaving, nested and reentrant
uploads, out-of-order consumption, a writer with no upload, stale entries, source mismatch, duplicate
buffers, nesting overflow, and empty-stack safety.

## 10. Reproducible checks

```
python tools/atlas/verify_a038r_binding_chain.py maps/a038r_binding_chain_fixture.json
python tools/atlas/verify_a038r_binding_chain.py maps/a038r_binding_chain_fixture.json --negative map-word
```

Capture environment:

```
$env:SHADPS4_VR='1'; $env:SHADPS4_OPENXR='0'; $env:SHADPS4_VR_DEMO='0'
$env:SHADPS4_TITLE_EYE_WIDTH='2880'; $env:SHADPS4_TITLE_RESOLUTION='6'
$env:SHADPS4_DBG_AUTOPRESS='1'; $env:SHADPS4_DBG_AUTOPRESS_TO='60000'
$env:SHADPS4_DBG_AUTOPRESS_MENU='1'
$env:SHADPS4_DERACINE_GNM_BINDING='1'
$env:SHADPS4_EXIT_AFTER='90'; $env:SHADPS4_CLEAN_EXIT='1'
shadPS4.exe -g D:\Temp\Deracine\1.0\Image0\eboot.bin
```

## 11. Remaining uncertainties

1. **The grouped `+0x38` provider is unproven.** The binder never dispatches on the title screen.
2. **Only constant-buffer declarations are covered.** Resource, sampler and vertex-buffer writers
   were not exercised.
3. **The upload object's identity is an address.** Its vtable, ownership and lifetime are not
   established.
4. **The descriptor's resource class is not decoded.** The bytes are captured; their meaning is not.
5. **Nine matched records have a null shader hash.** Their stage record's program-data pointer was
   null. They are recorded as unmatched-hash rather than dropped.
6. **One scene only.** Gameplay may exercise different binding families.
7. **No frame-rate claim.** The hot-path change was measured at the observer level only.
