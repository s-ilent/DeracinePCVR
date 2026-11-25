# RHI-04: first original capture

## Result

**Highest verified evidence level: 3 — the original index-resource bridge resolved.**

One bounded Déraciné 1.00 execution produced a genuine runtime observation of the original UE4
index-buffer object and its guest GPU allocation, with a determinable address-slot predicate, for 128
draws in one session. The resource package and the semantic tape were **not** produced, so the
same-draw identity gate could not run.

| Item | Value |
| --- | --- |
| Branch | `research/rhi04-first-original-capture-20261125` |
| Base | `fbe46f9ca341984386b0d614c14f112963d14e3b` (RHI-03P) |
| Launch permission | granted |
| Runs | **1** |
| Exit code | 0, clean exit |
| Session ID | `2490014584045497701` |
| Execution ID | `1795572179198350300` |
| Artifacts produced | 2 of 5 primary |

## 1. Executable identity

| Item | Value |
| --- | --- |
| Emulator source | `fbe46f9ca341984386b0d614c14f112963d14e3b` |
| Emulator SHA-256 | `87A8589C06F4A4FBCB81D873C5EEE31B45B4F7EA10C44BEFC43265B22D4EE6A7` |
| Game SHA-256 | `8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0` |
| Game image | the approved 1.00 SELF |
| Probe confirmation | `image_sha256=8caee78e...` logged at install |

The emulator verified the game hash itself before installing the probe. The qualification build was
not used.

## 2. What the run produced

| Artifact | Status | Size | SHA-256 |
| --- | --- | --- | --- |
| `index_bridge.json` | **produced** | 129,990 | `2D879109A0CB02FAE7F1BF2D51340F5E58F0FCEB195F1099DAF5C8F0AC6F4D58` |
| `session.json` | **produced** | 709 | `4F6435FAEF1F80AAE07F493C0736B659136CAF7AB43A6E7027CDBAD86F9BE735` |
| `package.json` | **absent** | — | — |
| `tape.json` | **absent** | — | — |
| `resources.bin` | **absent** | — | — |
| `textures.bin` | absent | — | — |
| `stage_*.spv` | absent | — | — |
| `reference_post.bin` | absent | — | — |

The emulator log is 13,239,033 bytes and is preserved privately.

## 3. Original command observations

The RHI entry probe installed against the approved image and recorded 128 events at the indexed-draw
implementation `0x800cbb9c0`, all from thread `80756`, all with the same caller `0x8013a68e8`.

| Field | Value |
| --- | --- |
| Provenance records | 128 |
| With a deferred command | 128 |
| Decoded records | 128 |
| Recorded records | 115 |
| Expired | 13 |
| Ambiguous | 0 |
| Inconsistent | 0 |
| Unmatched packets | 121,233 |

The probe's own summary reports `ambiguous=0` and `inconsistent=0`, so the command capture itself was
clean. The 13 expired records are draws whose provenance record outlived the capture window.

Draws observed include indexed draws with 48, 6, 144, 912 and 11,547 indices, one and five
instances, and one- and two-stage pipelines. This is the title's real draw stream, not a synthetic
fixture.

## 4. Index-resource bridge observations

This is the run's substantive result.

| Field | Value |
| --- | --- |
| Records | 128 |
| Session ID | `2490014584045497701`, one value across all records |
| Execution ID | `1795572179198350300`, one value across all records |
| Predicate determinable | **128 of 128** |
| Selected guest address present | **128 of 128** |
| Selected slot | `secondary_0x40`, every record |
| Distinct original objects | 12 |
| Distinct guest addresses | 12 |
| Index element widths | 2 and 4 |
| Address evidence | `runtime_observed` |

Example, provenance 1:

```
original_rhi_object   0x10044e85e8
resource_base         0x10044e8570   (object - 0x68, as recovered)
address_candidate_a   0x20000c0000
address_candidate_b   0x20000c0000
selected_slot         secondary_0x40
selected_guest_address 0x20000c0000
index_element_width   2
predicate_determinable true
```

**The statically recovered mapping is confirmed at runtime.** The resource base is the object minus
`0x68`, the element width is 2, and the selected address is a real guest GPU address. The
thread-dependent slot predicate resolved to `secondary_0x40` on every record, which is consistent
with the recovered rule: the record is non-null, the flag is zero, and the current guest thread id
differs from the recorded one.

Allocation size, offset and generation remain **unknown**, with their presence flags false. That is
correct: the observer does not read them at this call site, and inferring them would be a guess.

## 5. Why the package was not produced

One precise refusal, from `shader_param_capture.cpp:1476`:

```
Draw package for draw 11 refused: a descriptor interface declares a binding with no captured resource
```

Every draw reported `captured_bytes=0` in its shader-parameter summary. The descriptor interfaces
were captured — 272 of them, with 2 descriptors each — but the resources those descriptors name were
not retained, so the package validator refused the only candidate.

This is the first failing invariant: **the descriptor interface and the captured resource set
disagree.** The interface says a binding exists; the resource capture has nothing for it.

## 6. Why the tape was not produced

```
DERACINE_SEMANTIC_TAPE_SELECTED provenance_id=128 source=manual_or_last
DERACINE_SEMANTIC_TAPE_NONE the tape could not be built: draw_unresolved
```

The tape selects the draw the package describes. With no package, it fell back to the last recorded
draw, provenance 128, and could not resolve that draw to a deferred command. This is a **consequence**
of the package failure, not an independent defect: the tape's selection depends on the package by
design, which is what RHI-02I-R3 established.

## 7. Same-draw identity result

**`RESOURCE_PACKAGE_MISSING`**

The identity gate requires four artifacts. Two exist. The gate cannot run, and no identity claim is
made.

What the bridge alone establishes, and what it does not:

| Claim | Status |
| --- | --- |
| The original UE4 index-buffer object resolves to a guest GPU address | **verified at runtime** |
| The address-slot predicate is determinable | **verified at runtime**, 128 of 128 |
| The element width is recovered | **verified at runtime**, 2 and 4 |
| The session and execution are single | **verified at runtime** |
| The bridge describes the same draw as a resource package | **unverified**, no package |
| The bridge describes the same draw as a semantic tape | **unverified**, no tape |
| The allocation lifetime is known | **unknown**, by design |

The session id proves the artifacts claim the same execution. It does not prove resource identity,
and `session.json` says so in its own note.

## 8. Shader and resource completeness

| Item | Status |
| --- | --- |
| Effective SPIR-V modules | **captured**, 272 with no duplicates and no capacity drops |
| Permutation identities | captured, with `perm_hash` distinct per permutation index |
| Descriptor interfaces | **captured**, 272, 2 descriptors each |
| Push-constant declarations | captured, sizes 0 and 4 |
| Shader buffers | **not retained**, `captured_bytes=0` |
| Textures | not reached |
| Reference output | not reached |

The shader capture worked. The resource capture did not, and the package requires both.

## 9. Replay

**Not attempted.** The identity gate did not pass, and the runbook requires it to pass before
effective-shader replay. Attempting it would have meant either passing `--synthetic-fixture`, which
the authorization forbids, or inventing the missing package.

## 10. Reference comparison

**Not attempted.** No reference image was captured, because the package was refused before the
reference export ran.

## 11. Vulkan validation

Not applicable to this run: the replay was not executed. The emulator ran with its normal
configuration and exited cleanly.

## 12. Evidence level

| Level | Evidence | Reached |
| --- | --- | --- |
| 0 | Emulator did not reach the capture point | no, it did |
| 1 | Original UE4 command observed | **yes**, 128 records |
| 2 | Resource package captured from the same execution | **no** |
| 3 | Original index-resource bridge resolved | **yes**, 128 records |
| 4 | Original same-draw identity accepted | no |
| 5 | Effective shader modules and resources resolved | no |
| 6 | Original indexed draw executed independently | no |
| 7 | Output comparable to original reference | no |
| 8 | Original reference matched | no |

Level 3 is reached. Level 2 is not, and level 4 depends on it.

## 13. First failing invariant

**The descriptor interface declares a binding for which no resource was captured.**

This is upstream of everything else. The package validator refuses a package whose interface and
resources disagree, which is correct: binding a resource the interface did not name, or naming one
that was not captured, would produce a package that cannot be executed faithfully.

## 14. Recommended next experiment

**One primary target: find why the shader-buffer resource capture retained zero bytes while the
descriptor interfaces were captured in full.**

The evidence points at a specific place. The interfaces come from `Shader::Info` at pipeline
creation, which succeeded 272 times. The resources come from the production binding path, which
reported `captured_bytes=0` on every draw. So the interface capture and the resource capture are
reading different things, and the resource side is finding nothing.

The investigation is offline and needs no game launch:

1. Read `shader_param_capture.cpp`'s binding loop and determine what makes a binding retain bytes.
2. Compare against the descriptor interface capture, which succeeded, to find what it reads that the
   resource path does not.
3. Check whether the binding status is `GuestMemoryObserved`, `MetadataOnly` or `Unsupported` for
   these draws. The summary reports `partial=0 unsupported=0 invalid=0`, so the bindings are not
   being rejected — they are not being reached at all.
4. Determine whether the capture window is the cause: the probe starts at 33 seconds and the draws
   observed are early-frame UI draws with small index counts, which may be a different path from the
   scene draws that carry real shader buffers.

A second run would then be needed to confirm, and that requires fresh authorization.

## 15. What this run established

The statically recovered index-resource mapping is **correct at runtime**. That was the largest
unverified assumption in the whole chain, and it now has 128 independent observations behind it.

The capture pipeline itself works: the probe installed against the approved image, the session was
created once, the bridge was written with a single session and execution, and the shader binaries and
interfaces were captured in full.

The failure is narrow and specific: the resource side of the shader-parameter capture retained
nothing, so the package could not be built, and the tape and identity gate depend on the package.

## 16. Private artifact locations

| Item | Location |
| --- | --- |
| Capture directory | `_build/rhi04-capture-1` under the local work root |
| Lab | `_build/headless/labs/rhi04-capture-1` under the local work root |
| Emulator log | the lab's `user/log/shad_log.txt`, 13,239,033 bytes |

These contain game-derived data and are not committed. The hashes in section 2 identify them.
