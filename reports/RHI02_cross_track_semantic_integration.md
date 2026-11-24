# RHI-02: cross-track semantic and resource integration

## Status

**RHI-02 offline implementation complete. Same-draw runtime verification pending.**

Phases 0, 1, 2 and 4 are implemented and tested. Phase 3 correlation and the Phase 5 contract are
documented here. The central empirical claim, that one genuinely captured UE4 semantic tape and its
corresponding resource package describe the same draw, is **not** demonstrated. Every mutation result
below is synthetic fixture evidence derived from a real resource package.

Result categories used throughout:

- **Demonstrated** — an implementation exists and an observed result was produced.
- **Supported** — static evidence or an implemented interface supports the conclusion, but the
  complete runtime behaviour was not observed.
- **Unverified** — the claim depends on missing captures, unknown mappings or unexecuted integration.

## 1. Proven capabilities

### Demonstrated

| Capability | Evidence |
| --- | --- |
| Scoped image-state capture restores every subresource | `deracine_readback_test`, 8/8 byte-for-byte, scoped isolation passes, whole-image control fails with 5 validation errors |
| Snapshot budget counts copies, not commits | 10 accounting tests |
| Multi-stage binding records survive a draw | accounting tests, `MultiStageRecordsForTheSnapshotDrawAreAllRetained` |
| Partial mip/layer readiness is refused | 45/45 adversarial cases, 3 new |
| Same-draw identity join with explicit field presence | 20 identity tests |
| Semantic payload controls the executed Vulkan draw | mutation harness, cases A–H |
| Resource substitution changes sampling without changing the draw | mutation case I |
| A mismatched package is refused before GPU execution | mutation case J |
| Draw bounds are checked before GPU work | 8 bounds tests |
| Standalone Vulkan validation passes | `validation: errors=0` on every mutation case |

### The mutation results

Run against the real SC-10S resource package with a synthetic tape fixture, validation enabled:

| Case | Executed draw | Image hash | Validation | Provenance |
| --- | --- | --- | --- | --- |
| A baseline | 1101 indices, 2 instances | 16502619428972027710 | 0 | `synthetic_fixture` |
| A2 baseline, instance view | 1101 indices, 2 instances | 12787758644946534384 | 0 | `synthetic_fixture` |
| B `NumPrimitives` 367→200 | 600 indices | 15575859710782580447 | 0 | `synthetic_fixture` |
| C derived count corrupted | refused: `derived_inconsistent` | — | — | — |
| D `NumInstances` 2→1 | 1 instance | 3231696310514719174 | 0 | `synthetic_fixture` |
| E `FirstInstance` 0→3 | first instance 3 | 7102317032020622509 | 0 | `synthetic_fixture` |
| F `StartIndex` 0→168, 600 indices | first index 168 | 927865855109694692 | 0 | `synthetic_fixture` |
| G `BaseVertexIndex` 0→−7 from index 7 | vertex offset −7 | 1986557011826621981 | 0 | `synthetic_fixture` |
| H `PrimitiveType` 0→2 | refused by the derivation | — | — | — |
| I texturing disabled | identical to A | 15954340755637218007 | 0 | `synthetic_fixture` |
| J package identity changed | refused before GPU | — | — | `rejected` |
| K matcher fixture, unchanged | joins | 16502619428972027710 | 0 | `synthetic_fixture` |

K exists so J's rejection is shown to be caused by the mutation rather than by the fixture. Without
it, J could pass for the wrong reason: a fixture with no comparable identity is refused regardless of
what the package says.

B, F and G change the image through the semantic payload alone. I holds the tape fixed and changes
only the sampling, and its draw arguments are identical to A. J is the test that prevents two
independently valid but unrelated captures being treated as one integrated draw.

### Two fixture defects this phase found and corrected

The mutation fixtures were out of bounds, and the draws would have executed anyway.

`StartIndex` 0→168 with 1101 indices reads `[168, 1269)` from a buffer holding 1101 elements.
`BaseVertexIndex` 0→−7 addresses vertex −7, because the baseline window starts at index 0. With
robust buffer access the driver returns zeros rather than faulting, so both mutations would have
appeared to pass while exercising an invalid draw.

`draw_bounds.h/cpp` now checks the draw against the resources it actually binds, before any GPU
work. The vertex range is computed from the values the draw will actually fetch, not from the
allocation size. The fixtures are corrected rather than the production semantics: `StartIndex` 168
now carries 600 indices, and the base-vertex case starts at index 7 where the window minimum is 7.

### Three renderer defects validation found

Enabling validation layers found defects the image comparison could not see:

- the depth view carried the colour aspect, which is invalid for a depth-only format;
- the sampler requested anisotropy without the device feature enabled;
- the texture upload and its barriers were recorded inside the render pass, where a buffer-to-image
  copy is not permitted.

All three are fixed. The baseline image hash is unchanged, so the fixes corrected real errors
without altering the rendered result.

## 2. Original command recovery

| Relationship | Confidence | Basis |
| --- | --- | --- |
| Executor `0x13764D0` is gated by thread identity and resets the allocator in place | Statically verified | A-9, `137695e`–`1376969` |
| The executor runs no command destructors | Statically verified | A-9, no `vtable[1]` call in the sequence |
| A command payload is valid only between append and the allocator reset | Statically verified | A-9 |
| The semantic tape's arguments come from the deferred command payload | Demonstrated | RHI-01R, 79 tests |
| The tape's draw is the draw the package describes | **Unverified** | needs one capture producing both |

The allocator lifetime is the reason the tape stores scalar payload values rather than pointers. A
capture that retained a borrowed command pointer would read freed memory after the reset.

## 3. Resource recovery

| Resource class | Source | Independent of shadPS4? |
| --- | --- | --- |
| Vertex buffers | SC-09H package, guest bytes | Contents yes, discovery no |
| Index buffer | SC-10S package, guest bytes | Contents yes, discovery no |
| Shader buffers | SC-09H package, guest bytes | Contents yes, discovery no |
| Texture contents | SC-10S package, guest bytes | Contents yes, discovery no |
| Sampler state | SC-10S package, captured state | State yes, association partly |
| Render targets | Not captured | No |
| Pipeline state | SC-11 package (schema 3) | Captured, not replayed |

The distinction matters. The independent renderer creates its own Vulkan buffers and images and
uploads the captured bytes. It never uses a live shadPS4 Vulkan handle. But **which** resources a
draw used is still discovered through shadPS4's translated state, not from the original UE4 objects.

## 4. Shader recovery

### The permutation identity model

SC-11 established a correction that would otherwise have produced a wrong package:

```
program hash      = params.hash, from the guest program header
permutation index = the slot in Program::modules
permutation hash  = HashCombine(program_hash, permutation_index)
stage_hashes[]    = permutation hash        <- what the pipeline key carries
effective module  = patch SPIR-V if patched, else the generated SPIR-V
```

`HashCombine` is `seed ^ (hash + 0x9e3779b9 + (seed << 12) + (seed >> 4))`.

The GCN program hash does **not** uniquely identify the host shader module. One program compiles to
several permutations because the specialization depends on runtime state, resource shapes,
fetch-shader state and binding allocation. A hash match alone therefore does not identify the module
a draw executed.

Seven identities must stay separate:

1. Original UE4 shader-resource identity.
2. Original GCN program identity (`params.hash`).
3. Shader permutation index.
4. Shader permutation hash.
5. Generated SPIR-V module identity.
6. Effective patched or unpatched SPIR-V identity.
7. Vulkan pipeline identity.

An original shader hash is not a shadPS4 permutation hash. Equating them would silently bind the
wrong module.

### What SC-11 has and has not done

| Item | Status |
| --- | --- |
| Effective shader binaries captured with permutation identity | Demonstrated |
| Permutation hash validated as the combination of its parts | Demonstrated, C++ and Python |
| Six original-shader readiness dimensions | Demonstrated |
| Standalone original-shader execution | **Not achieved** |
| Descriptor-interface capture | **Missing** |
| Reference-output correctness | **Unexecuted** |

The precise blocker is the descriptor **set** layout the SPIR-V declares: which bindings are uniform
buffers, storage buffers, sampled images or samplers, and in which set. The package carries the
bindings the production path constructed, but not the classification that turns a list of ranges
into a `VkDescriptorSetLayout`. `Shader::Info` is not retained in the package.

## 5. Original shader-resource identity

| Finding | Confidence | Basis |
| --- | --- | --- |
| `0x0CBE210` is the shader-state binder, refcount-assigning `context+0x468` | Statically verified | `leaq 0x468(%r14)` plus refcount-assign call |
| `context+0x468` holds a **pointer** to the shader-state object, not an embedded member | Statically verified | Refcount-assign at `0x0CBEE2D`, `0x0CD9D67` |
| Field chain `context+0x468 → +0x200 → +0x1D8 → +0x40+idx*4` | Statically verified | `0x0CCD0D0`–`0x0CCD139` |
| Shader-resource object is 472 bytes, one shared allocator | Statically verified | 5,031 allocation sites |
| `writer+0xC468` holds `shader_resource+0xB0`, the map base | Statically verified | `0x0CB12FE`–`0x0CB132A` |
| The binder runs on the RHI thread | **Inferred** | A-9's two-thread gate, not observed for this function |
| `+0x1E0` and `+0x1E8` are the PS and VS resources | **Inferred** | Symmetric use, roles not proven |

**The consequence for RHI-02 is structural.** Shader-state binding is a direct context operation, not
a deferred command. The semantic tape therefore cannot carry the effective shader state, because the
recovered command cluster contains no shader-binding command. A tape that claimed to carry it would
be fabricating a command that was never emitted.

The correct join is: recover the effective shader state at the draw boundary from `context+0x468`,
independently of the tape. That is not implemented.

## 6. Original resource bindings

A-038 proved one complete chain on genuine execution:

```
PS/VS shader hash d5050ca2fb2370b9
  → OrbShdr trailer at 0x20000686a8, declaration table parsed
  → Original GNM declaration: IMM_CONSTBUFFER, api_slot 0, start_register 4, flags 0
  → Decoded map word 0x8004 (direct emission, destination index 4)
  → Typed writer 0x02F62440 (16-byte immediate constant-buffer writer)
  → Descriptor bytes by value: 00c8810e 20001000 03000000 ac7f0720
  → SET_SH_REG packet at the writer's packet cursor
```

Every arrow is an observed value.

**This covers one constant-buffer binding family.** It does not establish texture or sampler
mappings. The writer-stage index in that chain is not a logical fragment-stage identifier, and
treating it as one would be an unsupported leap.

### The evidence matrix

| UE4 semantic operation | Original shader/resource evidence | Effective host resource | Independent Vulkan operation | Confidence |
| --- | --- | --- | --- | --- |
| `DrawIndexedPrimitive` arguments | Deferred payload, RHI-01R | tape `draw` block | `vkCmdDrawIndexed` | Demonstrated (synthetic tape) |
| Index buffer | Guest address in package | `index_binding` | own `VkBuffer`, `UINT16` | Demonstrated (contents) |
| Vertex streams | Guest bytes in package | `vertex_bindings[]` | own `VkBuffer`, interleaved | Demonstrated (contents) |
| Sampled image | SC-10S `textures[]` | 64×64 BC3 sRGB | own `VkImage` + view | Demonstrated (contents) |
| Sampler | SC-10S `samplers[]`, verified association | sampler id 50 | own `VkSampler` | Demonstrated (state) |
| UV attribute | SC-10S `texture_coordinate` | binding 3, components 0,1 | vertex attribute 1 | Demonstrated |
| Constant buffer binding | A-038 chain, one family | shader buffers in package | not bound | Statically verified (chain), unresolved (this draw) |
| Effective shader state | `context+0x468`, binder `0x0CBE210` | not captured | diagnostic SPIR-V | Unresolved |
| Original shader module | SC-11 permutation identity | not replayed | diagnostic SPIR-V | Unverified |
| Texture descriptor writer | not established | — | — | Unresolved |
| Sampler descriptor writer | not established | — | — | Unresolved |
| Render targets | not captured | — | own offscreen target | Substituted |
| Viewport | tape `SetViewport` | tape | `vkCmdSetViewport` | Demonstrated |
| Pipeline state | SC-11 `pipeline_state` | not replayed | diagnostic pipeline | Substituted |

The diagnostic SPIR-V shader is a test mode. It is not original shader execution, and no row above
claims otherwise.

## 7. Independent Vulkan execution

The reference backend is direct Vulkan. It creates its own instance, device, buffers, images,
samplers and render targets. It consumes the tape for command semantics and the package for resource
contents.

Supported operations:

| Operation | Implementation |
| --- | --- |
| Create and upload vertex/index buffers | `CreateBuffer` + mapped copy |
| Create and upload images | `vkCreateImage` + staging copy |
| Create image views | aspect chosen from the format |
| Create samplers | from captured state, anisotropy enabled |
| Bind vertex/index inputs | `vkCmdBindVertexBuffers`, `vkCmdBindIndexBuffer` |
| Bind sampled resources | combined image sampler descriptor |
| Set viewport and scissor | from the tape's `SetViewport` |
| Issue indexed draw | `vkCmdDrawIndexed` from the tape's derived values |
| Synchronize and read back | barriers, then `vkCmdCopyImageToBuffer` |

Validation is opt-in through `DERACINE_REPLAY_VALIDATION=1`. Every mutation case reports
`validation: errors=0`.

## 8. Missing original state

| Gap | Consequence |
| --- | --- |
| Effective shader state at the draw boundary | Cannot bind the original shader |
| Descriptor set layout classification | Cannot build a pipeline from captured SPIR-V |
| Texture and sampler descriptor writers | Cannot prove which original declaration produced a binding |
| Render targets | The diagnostic target is substituted, not recovered |
| Original pipeline state replay | Captured by SC-11, not consumed |
| Original material parameters | No material reconstruction |

## 9. Next-draw feasibility

To execute two consecutive original UE4 draws while preserving state dependencies, the following is
needed beyond the current implementation:

1. **A tape carrying more than one draw.** The current tape is bounded to one draw and its preceding
   state. `MaxTapeOperations` is 256, so capacity is not the limit; the exporter selects one draw.
2. **State-difference tracking.** The second draw may reuse the first's pipeline, viewport and
   bindings. The tape must express "unchanged since the previous operation" rather than repeating
   every state element, or the consumer must diff.
3. **Resource lifetime across draws.** A buffer bound by both draws must be uploaded once. The
   current replay uploads per run.
4. **Ordering guarantees.** The tape's operations are ordered, but the exporter does not yet
   establish that the state preceding draw 2 is complete rather than merely the state it happened to
   observe.

None of these is blocked by the current architecture. All are exporter and contract work.

## 10. Whole-frame feasibility

A complete frame needs command classes the tape does not carry:

| Missing class | Why it matters |
| --- | --- |
| Compute dispatches | The title uses compute for storage writes |
| Resource copies and resolves | MSAA resolve, depth aliasing |
| Multiple render targets | The deferred path writes several |
| Draw variants | Non-indexed, instanced-with-attributes, indirect |
| State-only commands | Blend, depth-stencil, rasterizer changes between draws |
| Fences and semaphores | Cross-queue dependencies |
| Presentation | The swapchain or OpenXR composition layer |

The frame also needs resource lifetime semantics the tape does not express: when a transient target
may be reused, and which barriers the original renderer relied on. shadPS4 currently derives those
from the PM4 stream, which is exactly the dependency RHI-02 exists to remove.

## 11. Native PCVR feasibility

The contract in `docs/research/RHI02_native_renderer_contract.md` is written so a native OpenXR
runtime can supply the device and the swapchain images. It does not assume shadPS4 owns the instance,
physical device, logical device, queue, render targets or composition images.

**OpenXR integration has not been demonstrated.** The contract permits it; nothing implements it.

The Native Renderer Foundation research informs the ownership model:

- NRI can adopt an externally created Vulkan device and images, which is the shape the contract
  assumes.
- NVRHI's state-seeding and image-state handling are pitfalls the contract names explicitly rather
  than leaving to the implementer.
- Plume currently lacks a suitable non-owning device interface, so it is not a candidate for the
  first native backend.
- OpenXR swapchain images cannot be independently allocated or destroyed, so the contract treats
  them as borrowed with an explicit acquire/release lifetime.

No RHI library is selected. Direct Vulkan is the current executable reference, and the choice of a
library should follow a working native backend rather than a source-level feature comparison.

## 12. Cross-title reuse

| Component | Classification |
| --- | --- |
| Semantic command vocabulary (`OpKind`) | Generic to Unreal's RHI architecture |
| `DeriveDraw` conversion | Specific to the PS4 UE4 implementation |
| `MatchDrawCommand` identity model | Generic to UE4's deferred RHI, with PS4-specific fields |
| `draw_identity` join | Generic to any two-capture join |
| `draw_bounds` check | Generic |
| Package schema | Specific to shadPS4's observation mechanisms |
| Permutation identity model | Generic to shadPS4's shader compilation, not to UE4 |
| Diagnostic replay | Specific to the diagnostic replay |
| Texture/sampler reconstruction | Specific to the PS4 GNM formats |

Déraciné, Summer Lesson and Happy Manager are all Unreal Engine titles on PS4, so the semantic
vocabulary and the deferred-command identity model should transfer. The PS4 RHI conversion rules
should transfer if the engine version is close. The package schema and the shadPS4 observation
mechanisms are ours and transfer trivially.

**Kowloon's Gate VR is not established as an Unreal title in this repository's evidence.** No
engine evidence for it exists here, so no reuse claim is made.

The engine version matters: UE4.18's `FRHICommandDrawIndexedPrimitive` payload layout is what the
tape encodes. A different engine version could reorder or resize that payload, and the tape would
need a version-specific decoder rather than assuming the layout.

## 13. Remaining uncertainties

Prioritised, distinguishing the kind of gap:

| Priority | Blocker | Kind |
| --- | --- | --- |
| 1 | No capture has produced a semantic tape and a resource package from one execution | Missing evidence |
| 2 | The two exporters select their draw independently, so one run does not guarantee one draw | Missing evidence |
| 3 | The tape's index identity and the package's index identity are in different domains | Missing evidence |
| 4 | Descriptor set layout classification is not captured | Missing implementation |
| 5 | Effective shader state at the draw boundary is not recovered | Missing implementation |
| 6 | Texture and sampler descriptor writers are not established | Missing evidence |
| 7 | SC-11's original-shader replay has never executed | Unexecuted runtime verification |
| 8 | SC-11's reference-output oracle has never run against the game | Unexecuted runtime verification |
| 9 | Render targets are substituted, not recovered | Missing implementation |
| 10 | Multi-draw state-difference tracking does not exist | Missing implementation |
| 11 | Whole-frame command classes are absent | Missing implementation |

### The identity-domain blocker

This is an acceptance blocker, not a warning, and it is now enforced in code.

The tape records the index resource as the RHI invocation argument, which is an original UE4
`FRHIIndexBuffer` object pointer. The package records the guest GPU address of the index buffer.
**These are not the same domain.** Their numeric equality or inequality is not evidence either way.

`IndexDomain` now tags both sides, and `JoinTapeToPackage` refuses to compare across namespaces. A
cross-domain pair reports `insufficient_evidence` with both domains named. An unknown domain cannot
establish the join either.

The synthetic fixture was corrected because it had been built the wrong way. It previously copied the
package's guest address into the tape, which made the identity check pass for the wrong reason: the
exact circularity this milestone exists to prevent. It now records no index identity and declares the
UE4-object domain, so the join reports insufficient evidence and the replay refuses to run without an
explicit `--synthetic-fixture` flag.

The synthetic label relaxes only the requirement for an original identity to exist. **A positive
mismatch is always fatal**, synthetic or not, so two captures that disagree cannot be executed
together under any label.

The remaining fix is an independently established object-to-buffer relationship: the original UE4
index-buffer object's guest allocation, resolved through the RHI's own resource tracking, compared
against the GPU address the draw consumed. That relationship is not captured, and the runbook names
it as the smallest implementation that would close the gap.

**The matcher must not be weakened to make existing packages pass.** If the domains differ, the
correct outcome is `insufficient_evidence`, not a fabricated join.

### The selection blocker

The two exporters select their draw independently.

| Exporter | Selection |
| --- | --- |
| Semantic tape | `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` if set, otherwise the last recorded draw |
| Resource package | scores every captured draw on geometry completeness, writes the first that validates |

Setting both output flags to one directory therefore does not guarantee they describe the same draw.
The tape defaults to the last recorded draw; the package writes the first draw that validates.

An explicit `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` is required, and it must be the provenance id the
package actually wrote. That is only knowable after the package exists, so the capture is two-phase.

**Pinning the draw is a selection mechanism, not evidence.** It makes the two exporters agree on
which draw to describe. It does not establish that they observed the same operation. The independent
identity check is what must establish the correspondence, and that check is blocked by the domain
problem above.

## 14. The genuine capture experiment

See `docs/research/RHI02_same_draw_capture_runbook.md`. It is prepared and not executed.

The experiment needs one bounded run producing both a tape and a package, then an offline join. It
requires explicit authorisation to launch the emulator.
