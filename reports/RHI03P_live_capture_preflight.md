# RHI-03P: live-capture preflight

## Verdict

**`READY_WITH_EXPLICIT_CAPTURE_LIMITATIONS`**

The four preflight defects are fixed and the regression gate enforces its verdicts. A genuine
single-execution capture can now be attempted, with the limitations in section 9 stated up front.

## 1. Branch

| Item | Value |
| --- | --- |
| Branch | `integration/rhi03-unified-original-shader-renderer` |
| Base | `5c371679f3053820c8d62ed47429eb7c2bf5cd9e` |
| C++ tests | 307 across sixteen suites |
| Python integration cases | 39 |
| GPU mutation cases | 13, enforced |
| Vulkan validation | 0 errors on every executed case |

## 2. The four defects

### Defect 1: the runbook could not initialise a capture session

`CaptureSession::TryInstall()` requires both `SHADPS4_DERACINE_CAPTURE_SESSION` and
`SHADPS4_DERACINE_CAPTURE_SESSION_DIR`. The runbook set neither, so no canonical session identity
was initialised and the original-mode replay could not establish that the package, tape and bridge
came from one execution.

The runbook also named `manifest.json`, but the implementation writes and reads `session.json`.

Both corrected. Every environment variable the runbook names is now checked against its source
definition, and the artifact inventory marks `reference_pre.bin` as optional because a pass that
clears its colour attachment has no capturable pre-draw image.

The install order is verified: `CaptureSession::TryInstall()` runs at `linker.cpp:553`, before
`IndexIdentity`, `SemanticTape`, `ShaderBinaryCapture`, `ShaderInterface` and `ReferenceOutput`. A
missing directory is reported as `Capture session refused: ... is not set`.

### Defect 2: producer readiness was decided from a value

```cpp
package.readiness.pipeline_state_ready_original =
    out.topology != 0 && !out.color_formats.empty();
```

`VK_PRIMITIVE_TOPOLOGY_POINT_LIST` is 0, so a point-list draw reported its pipeline state as not
captured. The replay had already been corrected; the producer had not.

The producer now records a `PipelineStatePresent` block and serialises it, and readiness is
`present.topology && present.color_formats && push_constants_fit`. The validator accepts the
presence flags with `has_topology` as a fallback for a package written before they existed.

### Defect 3: push constants could carry a fabricated zero tail

```cpp
out.push_constant_bytes.resize(in.push_constant_size);
std::memcpy(out.push_constant_bytes.data(), in.push_constant_words,
            std::min<size_t>(in.push_constant_size, sizeof(in.push_constant_words)));
```

The source is `push_constant_words[16]`, 64 bytes. A declared size above that produced a vector with
a zero-filled tail that was never captured, and the replay would accept fabricated parameters as
genuine captured state.

The declared size is now validated against the capacity before any bytes are exported. An oversized
range records the size as zero, claims no bytes, and sets a limitation. Readiness stays false.

### Defect 4: the mutation harness did not enforce anything

`run_schema4_mutations.py` recorded outcomes and returned 0 unconditionally. A regression producing
wrong draw arguments, an unchanged image after a shader mutation, or validation errors would still
have reported success.

It is now a gate. Every case carries machine-checked expectations, an executed case must report zero
validation errors, and the harness exits nonzero on any failure. It writes `passed`, `failed`,
`case_results`, `failure_reasons` and `validation_errors`.

## 3. Source-verified capture flags

Every flag the runbook uses, checked against its definition:

| Flag | Purpose |
| --- | --- |
| `SHADPS4_DERACINE_CAPTURE_SESSION` | enable the session identity |
| `SHADPS4_DERACINE_CAPTURE_SESSION_DIR` | where `session.json` is written |
| `SHADPS4_DERACINE_RHI_PROBE` | the RHI entry probe |
| `SHADPS4_DERACINE_RHI_PROBE_AFTER` | the frame gate |
| `SHADPS4_DERACINE_DRAW_PROVENANCE` | per-draw provenance records |
| `SHADPS4_DERACINE_INDEX_IDENTITY` | the original index-object observer |
| `SHADPS4_DERACINE_VERTEX_INPUT` | vertex streams |
| `SHADPS4_DERACINE_SHADER_PARAMS` | shader buffers and the package |
| `SHADPS4_DERACINE_TEXTURE_CAPTURE` | texture contents |
| `SHADPS4_DERACINE_SHADER_BINARY` | effective SPIR-V modules |
| `SHADPS4_DERACINE_SHADER_INTERFACE` | descriptor interfaces |
| `SHADPS4_DERACINE_REFERENCE_OUTPUT` | reference images |
| `SHADPS4_DERACINE_DRAW_PACKAGE` | the package directory |
| `SHADPS4_DERACINE_SEMANTIC_TAPE` | the semantic tape |
| `SHADPS4_DERACINE_SEMANTIC_TAPE_DIR` | the tape directory |
| `SHADPS4_DERACINE_CMD_CAPTURE` | the deferred command capture |

`SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` is documented as one the run must **not** set: RHI-02I-R3 made
the package-selected provenance authoritative, and pinning it would be a selection mechanism rather
than evidence.

## 4. Artifact inventory

```text
<capture-directory>/
    session.json          the selection and verification record
    package.json          schema 4
    tape.json             schema 2, session-bearing
    index_bridge.json     the original-object-to-guest-allocation bridge
    resources.bin         geometry and shader-buffer bytes
    textures.bin          texture bytes
    stage_*.spv           effective shader modules
    reference_post.bin    the post-draw reference
    reference_pre.bin     OPTIONAL: only when that capture succeeded
```

## 5. Producer readiness results

| Case | Result |
| --- | --- |
| Topology zero present and valid | ready |
| Topology absent | refused: "no topology" |
| Nonzero topology | ready |
| Colour format absent | refused: "no colour attachment format" |
| Ready claim without the fields | refused |
| Diagnostic substitution without original readiness | accepted, original dimension stays false |

## 6. Push-constant bounds results

| Case | Result |
| --- | --- |
| Zero-byte range | accepted, no bytes |
| Exact capacity (64 bytes) | accepted, last word is the captured one |
| One byte below capacity | accepted |
| One byte above capacity | refused, no bytes |
| Far oversized (4096) | refused, no bytes |
| All-zero block | accepted, distinguishable from missing via the fit flag |

## 7. Mutation gate

| Case | Assertion | Result |
| --- | --- | --- |
| A baseline | 6 indices, 1 instance, exact reference | pass |
| B primitive count | index count 3, coverage changed | pass |
| C instance count | instance count 3 | pass |
| D start index | first index 3 | pass |
| E uniform | image changed, draw arguments unchanged | pass |
| F push constants | image and coverage changed, draw unchanged | pass |
| G cull mode | coverage 0 | pass |
| H write mask | `incomplete_capture` | pass |
| I permutation corruption | `permutation_hash_mismatch`, no execution | pass |
| J missing bindings | `missing_binding` | pass |
| K missing resource | `missing_resource` | pass |
| L invalid bounds | refused before GPU | pass |
| M corrupt reference | sidecar refused | pass |

**Negative control.** The gate asserts the baseline has 999 indices, which is false. It reports the
failure and the harness exits nonzero. A gate that always passed would look identical to a gate that
worked, so this is what distinguishes them.

**Second negative control.** Running the gate against a replay that does not implement the effective
path produces four failures and exit 1.

## 8. Regression totals

| Suite | Tests |
| --- | --- |
| Draw identity | 24 |
| Draw bounds | 8 |
| Semantic tape | 79 |
| Draw package | 49 |
| Index bridge | 16 |
| Capture session | 10 |
| Texture accounting | 10 |
| SPIR-V interface | 12 |
| Slot predicate | 13 |
| Bridge production | 16 |
| Package reader | 10 |
| Reference compare | 17 |
| Reference oracle | 8 |
| Pipeline presence | 7 |
| Capture artifacts | 16 |
| Producer readiness | 12 |
| **Total** | **307** |

Plus 20 selection cases, 19 integration cases and 13 enforced GPU mutation cases.

## 9. Limitations that remain

These are stated, not solved. They are the reason the verdict is not `READY_FOR_AUTHORIZED_CAPTURE`
without qualification.

| Limitation | Consequence |
| --- | --- |
| Colour attachment format is a GNM packed format, not a Vulkan enum | the replay renders into its own RGBA8 target; the original format is not reproduced |
| Blend factors and operations are not captured | a package that captured blend enable is refused rather than guessed |
| Effective shader state at the draw boundary is not recovered | the shader modules are captured, the binding that selected them is not |
| Texture and sampler descriptor writers are not established | a texture binding's original declaration is unresolved |
| Render targets are substituted | the diagnostic target is the replay's, not the game's |

None of these blocks the capture. They are what the capture is meant to produce evidence about.

## 10. Runtime-only unknowns

These cannot be established offline and are the purpose of the capture:

- whether the game's passes reach the capture path with the state the fixtures model;
- whether the index-object-to-guest-allocation mapping holds in a live process;
- whether a real package carries every field the effective renderer needs;
- whether the reference oracle's behaviour inside a real rendering pass matches;
- whether the deferred readback completes before package export in a real run.

## 11. The next experiment

`docs/research/RHI03_single_execution_capture_runbook.md`, executed once with explicit authorisation,
against the approved Déraciné 1.00 executable
`8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0`.

Its purpose is to establish what the original game actually provides, and to identify the remaining
fidelity gaps from real evidence rather than synthetic fixtures.
