# RHI-03: unified renderer integration

## Status

**RHI-03 offline integration complete. Original-game verification pending.**

| Item | Value |
| --- | --- |
| Branch | `integration/rhi03-unified-original-shader-renderer` |
| Head | `7bdbed49` plus the Phase 7D commit |
| Base | `99073a06` (RHI-02I) |
| C++ tests | 295 across fifteen suites |
| Python integration cases | 39 |
| GPU mutation cases | 13 |
| Vulkan validation | 0 errors on every executed case |

## 1. What was integrated

| Source | Commit | Contribution |
| --- | --- | --- |
| RHI-02I | `99073a06` | Original index-resource identity, single-execution capture |
| RHI-02I-R2 | `1f7a6439` | Production identity bridge, slot predicate, session identity |
| RHI-02I-R3 | `d2140955` | Structural index-address parsing, bridge selection, manifest schema 2 |
| SC-11R | `92820981` | Effective shader binaries, descriptor interfaces, reference output |
| SC-11R-H | `8a2ffd54` | Render-pass lifecycle correction |
| RHI-02 | `25bd7d3a` | Semantic tape, bounds, identity domains, synthetic labelling |

SC-11R and RHI-02I are siblings from SC-10S, not a linear chain. The merge was a reconciliation.

## 2. Schema compatibility

| Version | Carries | Accepted | Effective mode |
| --- | --- | --- | --- |
| 1 | shader buffers only | no | no |
| 2 | resources, geometry, textures | yes | no |
| 3 | session identity, effective shader binaries | yes, compatibility | no |
| 4 | descriptor interfaces, presence flags, reference images | yes | yes |

Schema 3 is accepted as a compatibility path because RHI-02I-R2 and RHI-02I-R3 write it, and
refusing it broke their fixtures for a reason unrelated to what they test. It carries no descriptor
interface, so effective-shader mode refuses it. A version outside this set is refused rather than
reinterpreted.

Schema 4 retains the `session_id` that version 3 introduced, with the same meaning. That is what
RHI-02I-R3's handoff requires.

## 3. Effective shader execution

The renderer loads a package's own captured modules from sidecars and executes them.

| Check | Behaviour |
| --- | --- |
| Schema | must be 4 |
| Stages | exactly one vertex and one fragment, by logical stage |
| Module bytes | must be present and readable SPIR-V |
| Permutation hash | must equal `HashCombine(program hash, permutation index)`, recomputed |
| Descriptor interface | the module is the authority; a captured interface that disagrees is refused |
| Bindings | every declared descriptor must have a captured binding naming a carried resource |
| Pipeline state | topology and a colour format must have been captured |

The descriptor correspondence is the production path's own record: `vk_binding`, `vk_offset`,
`vk_range` and `resource_ref`. The guest `resource_index` and `sharp_idx` are different namespaces
and are not compared against the Vulkan binding number.

### Evidence that the modules actually run

| Mode | Image hashes |
| --- | --- |
| effective | 18134581161301189413 |
| diagnostic | 9108263565764906557 |

The images differ. A renderer that validated the modules and then ignored them would produce
identical output, which is what the first wiring did before validation caught the missing descriptor
set layout.

## 4. Push constants

One `PushConstants()` mechanism, used by every pipeline-binding callback, with an explicit mode.

| Mode | Behaviour |
| --- | --- |
| Diagnostic | the diagnostic MVP |
| Effective, captured range | the exact captured bytes, offset and stage mask |
| Effective, no declared range | nothing |
| Effective, insufficient bytes | refused before GPU work |

A second path that pushed the diagnostic MVP would overwrite captured state, and for a shader
declaring no push constants it would issue a push the layout does not have.

Evidence, changing only the push bytes:

| Run | Hash | Covered |
| --- | --- | --- |
| baseline | 18134581161301189413 | 4096 |
| `offset.x = 0.5` | 8725907037563099941 | 3072 |

Draw arguments identical.

## 5. Pipeline state

| Field | Captured | Applied |
| --- | --- | --- |
| Topology | yes | yes |
| Cull mode | yes | yes |
| Front face | yes | yes |
| Polygon mode | yes | yes |
| Depth test | yes | yes |
| Depth write | yes | yes |
| Depth compare | yes | yes |
| Sample count | yes | yes |
| Colour write mask | yes | yes |
| Colour attachment format | guest packed format | **no** |
| Blend factors and operations | **no** | **no** |

Presence is explicit per field. Zero is a legitimate value for every one of these enums:
`VK_POLYGON_MODE_FILL` is 0, `VK_COMPARE_OP_NEVER` is 0, `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` is 0 and
`VK_CULL_MODE_NONE` is 0. Testing a value against zero substituted the diagnostic default for exactly
the fields the game set to their zero value.

The run reports an execution classification: `diagnostic_substitution`,
`effective_captured_state` or `effective_partial_state`, plus the list of substituted fields.

### The two gaps

**Colour attachment format.** The producer records the guest's packed data and number format, not a
Vulkan enum. Converting between them needs a verified mapping that does not exist, so the replay
renders into its own RGBA8 target and reports the substitution.

**Blend state.** Blend factors and operations are not captured. A package that captured blend enable
is refused with a precise reason rather than having a blend state invented.

## 6. Reference comparison

| Status | Meaning |
| --- | --- |
| `exact_match` | every pixel matched |
| `pixel_mismatch` | comparable and different |
| `missing_reference` | the package carries none |
| `incompatible_reference` | extent, pixel size or format differs |
| `unsupported_configuration` | multisampled, cannot be read without a resolve |
| `incomplete_capture` | declared but absent, or a required pre-draw image is missing |

Extent, pixel size, byte length and format are checked before any pixel, so a configuration failure
is never reported as a rendering failure. Equal byte size is not comparable representation: RGBA8 and
BGRA8 differ in channel order.

`DependsOnPriorContents` decides from captured state whether a draw inherits what the target held:
blending reads the destination, a partial write mask leaves channels as they were, a depth test
leaves some pixels untouched. When it returns true the comparison requires the pre-draw image.

### The independent oracle

`reference_oracle.h/cpp` computes the expected image on the CPU from the geometry, without Vulkan. It
is restricted to a fixture simple enough to audit by hand: clip-space vertices with a flat colour, a
top-left fill rule, flat colour from the provoking vertex, and a background the draw inherits.

`seed_reference.py` remains for comparator self-tests and is labelled non-independent: a renderer that
produced the wrong image would seed the wrong reference and match itself.

## 7. GPU mutation matrix

Run against the schema-4 fixture with validation enabled.

| Case | Executed | Covered | Reference | Validation |
| --- | --- | --- | --- | --- |
| A baseline | 6 indices, 1 instance | 4096 | exact_match | 0 |
| B `NumPrimitives` 2→1 | 3 indices | 2080 | pixel_mismatch | 0 |
| C `NumInstances` 1→3 | 6 indices, 3 instances | 4096 | exact_match | 0 |
| D `StartIndex` 0→3 | 3 indices, first 3 | 2016 | pixel_mismatch | 0 |
| E uniform tint changed | 6 indices | 4096 | pixel_mismatch | 0 |
| F push `offset.x` 0→0.5 | 6 indices | 3072 | pixel_mismatch | 0 |
| G `cull_mode` 0→back | 6 indices | 0 | pixel_mismatch | 0 |
| H `write_mask` 0xF→0x1 | 6 indices | 4096 | incomplete_capture | 0 |
| I permutation hash corrupted | refused | — | — | — |
| J captured bindings removed | refused | — | — | — |
| K uniform resource removed | refused | — | — | — |
| L 300 indices from a 6-element buffer | refused before GPU | — | — | — |
| M reference hash corrupted | refused | — | — | — |

Case C is worth noting: three instances of the same geometry with no per-instance attribute rasterise
identically, so the image is unchanged while the executed instance count is 3. The draw argument is
the evidence, not the image.

Case H is the dependency check working: a partial write mask makes the draw inherit prior contents,
and the fixture carries no pre-draw image, so the comparison reports `incomplete_capture` rather than
assuming a cleared target.

## 8. Original identity gate

The gate is wired end to end and fail-closed.

| Check | Enforced |
| --- | --- |
| Same non-zero session on tape, package and bridge | yes |
| Bridge `original_rhi_object` equals the tape's index resource, in the `Ue4Object` domain | yes |
| Bridge selected address equals the package's index address | yes |
| Address-slot predicate determinable | yes |
| Valid index width and range | yes |
| No unconfirmed generation | yes |
| Positive mismatch is fatal | yes |

The slot predicate is recovered from `0xCBBCCE`–`0xCBBCF1` and returns `Unknown` when any input is
missing, so an indeterminate predicate leaves the address absent rather than falling back to a slot.

Allocation size, offset and generation remain explicitly unknown. An address match with an
unconfirmed generation is insufficient evidence, because a reused allocation cannot be distinguished
from a genuine one.

### The manifest correction

`FinalizeSelection` set `identity_verified` after checking session, provenance, predicate
determinability and matching index addresses, but it never compared the tape's original
`FRHIIndexBuffer` against the bridge's `original_rhi_object`. The replay matcher does enforce that,
so the manifest was claiming stronger evidence than it had.

It now reads the tape's index object, compares it, and reports `preliminary_identity_check` when the
tape carries none. The replay still does not trust the manifest as proof: a manifest is a summary of
observations, not another independent observer.

## 9. Evidence classification

| Claim | Status |
| --- | --- |
| Synthetic renderer execution | **Verified offline** |
| Captured-style pipeline execution | **Verified for the fields the matrix exercises** |
| Original Déraciné shader execution | **Pending runtime capture** |
| Original same-draw identity | **Pending runtime verification** |
| Original pixel-level equivalence | **Pending a synchronized game reference** |

These are separate claims and are not combined.

## 10. Remaining blockers

| Priority | Blocker | Kind |
| --- | --- | --- |
| 1 | No capture has produced a tape, package and bridge from one execution | Missing evidence |
| 2 | The index-object-to-guest-allocation mapping is statically recovered but never observed | Unexecuted runtime verification |
| 3 | Colour attachment format conversion has no verified mapping | Missing implementation |
| 4 | Blend factors and operations are not captured | Missing evidence |
| 5 | Effective shader state at the draw boundary is not recovered | Missing implementation |
| 6 | Texture and sampler descriptor writers are not established | Missing evidence |
| 7 | Render targets are substituted, not recovered | Missing implementation |
| 8 | Multi-draw state-difference tracking does not exist | Missing implementation |

## 11. The next experiment

See `docs/research/RHI03_single_execution_capture_runbook.md`. It is prepared and not executed.

It requires the approved Déraciné 1.00 executable,
`8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`, and one bounded run producing
all artifacts from one execution. Observations from another build must not be combined.

## 12. Reproducing

```powershell
# The emulator and its tests.
pwsh -File _build/build.ps1 -Root <worktree>
pwsh -File _build/build.ps1 -Tests -Root <worktree> -Target <suite>

# The replay tools. Release, because the Debug tree's CRT flags abort on the debug heap.
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-geometry-replay

# The schema-4 fixture and its mutation matrix.
python tools/make_schema4_fixture.py <dir>
python tools/seed_reference.py <dir> <dir>/tape_front.ppm
$env:DERACINE_REPLAY_VALIDATION='1'
python tools/run_schema4_mutations.py .\build-geometry-replay\deracine_tape_replay.exe <dir> <work>

# The identity integration suites.
python tools/test_rhi02ir3_selection.py .\build-geometry-replay\deracine_tape_replay.exe
python tools/test_rhi02ir2_integration.py .\build-geometry-replay\deracine_tape_replay.exe
```
