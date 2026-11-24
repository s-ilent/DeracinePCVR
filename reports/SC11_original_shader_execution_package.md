# SC-11: original shader execution package

## Result

The capture path now retains the effective shader binaries of a draw, and the package schema carries
them with the identity that distinguishes one compiled permutation from another. The validator
enforces six separate original-shader readiness dimensions, and 58 adversarial cases are rejected.

**The package is not yet executable end to end.** The shader modules are captured and verified, but
the standalone replay stops at structural validation because the descriptor layout the SPIR-V
declares is not yet reconstructed into a pipeline. Section 9 states exactly what remains.

| Item | Value |
| --- | --- |
| Base | `da969d4a` (SC-10S) |
| Package schema | 3 |
| C++ package tests | 40 passing (14 new) |
| Adversarial cases | 58/58 rejected (13 new) |
| Replay tools | 3, all building |
| Effective shader binaries captured | yes, with permutation identity |
| Standalone original-shader execution | **not achieved** |

## 1. Candidate selection

The task asked for a draw with vertex and fragment stages only, ordinary indexed triangle-list
geometry, one colour attachment, few shader resources, and fully captured vertex and index buffers.

The SC-10S draw (provenance 40) is the best available candidate:

| Property | Value |
| --- | --- |
| Indices | 1101 |
| Instances | 2 |
| Vertex streams | 4 |
| Shader buffers | 6 (ranges 16, 2848, 16, 2848, 2848, 288) |
| Texture | one 64x64 BC3 sRGB |
| Stages | vertex and fragment only |

The SC-09H classroom mesh (provenance 82) has more streams and no texture, so it exercises less of
the shader input surface. The SC-10S draw is the shorter path to a complete input inventory.

**The selection is not yet confirmed against a live capture.** No emulator run was performed, so the
candidate is chosen from the existing package metadata rather than from a fresh inventory of ranked
draws. That is a real limitation of this phase, stated in section 11.

## 2. Shader permutation provenance

This is the phase's most important finding, and it corrects an assumption that would have produced a
wrong package.

**The GCN program hash is not a unique identifier for the effective compiled shader.**

`PipelineCache::GetProgram` returns `HashCombine(params.hash, perm_idx)`, not `params.hash`.
`RefreshGraphicsStages` stores that value in `GraphicsPipelineKey::stage_hashes`, which is what the
rasterizer reads as `vs_hash` and `ps_hash`. So the hash a package has always carried is a
**permutation hash**, not a program hash.

The relationship, recovered from the source:

```
program hash        = params.hash, from the guest program header
permutation index   = the slot in Program::modules
permutation hash    = HashCombine(program_hash, permutation_index)
stage_hashes[]      = permutation hash          <- what the pipeline key carries
effective module    = patch SPIR-V if patched, else the generated SPIR-V
```

`HashCombine` is `seed ^ (hash + 0x9e3779b9 + (seed << 12) + (seed >> 4))`.

One program compiles to several permutations because the specialization depends on runtime state,
resource shapes, fetch-shader state and binding allocation. A hash match alone therefore does not
identify the module a draw executed. The package now records all four values, and the validator
checks that the permutation hash is the combination of the other two, so a mis-associated module is
refused rather than silently accepted.

### Patch state

`CompileModule` chooses between the generated SPIR-V and a patch:

```cpp
auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
```

The package records `patched` and both hashes. When a patch was applied, the effective module is the
patch and `effective_sha256` describes it; `generated_sha256` describes the unpatched output. A
consumer that loaded the generated SPIR-V for a patched draw would be running a different module, and
the two hashes are what make that detectable.

## 3. Effective SPIR-V acquisition

The existing infrastructure already had the pieces, and no redundant extraction was added.

| Facility | What it provides |
| --- | --- |
| `PipelineCache::CompileModule` | the only point where GCN words, generated SPIR-V and the patch decision are all in scope |
| `DumpShader` | writes `<stage>_<hash>_<perm>.spv` to the shader dump directory |
| `RegisterShaderBinary` | persists SPIR-V into the shader cache database |
| `DebugState::CollectShader` | retains name, stage, module, SPIR-V, ISA and patch state in memory |

Two of these were unusable for this phase:

- **`DumpShader` is gated on `IsDumpShaders()`**, which is off in every lab, and it writes the
  generated SPIR-V only, never the patched module.
- **`RegisterShaderBinary` is a no-op unless the pipeline cache is enabled**, which is also off. It
  is called from `CompileModule` but returns immediately.
- **`CollectShader` retains everything needed but is only consumed by the GUI shader list**, never
  written to disk.

No shader dumps or shader cache exist anywhere on disk, which confirms that the SPIR-V was never
persisted by any previous run.

`Core::ShaderBinaryCapture` therefore hooks `CompileModule` directly. It is opt-in, bounded, and
retains the GCN words, the generated SPIR-V, the patch SPIR-V and the patch decision, keyed by
(program hash, permutation index). A repeated permutation is not stored twice.

## 4. Complete input inventory

The package now carries, per stage: program hash, permutation index, permutation hash, hardware and
logical stage, patch state, effective and generated SHA-256, entry point, byte lengths, and sidecar
filenames. The bytes live in private sidecars and are never committed.

The other inputs were already captured by SC-09H through SC-10S and are unchanged:

| Input | Where |
| --- | --- |
| Vertex streams, strides, formats, divisors | `vertex_bindings[]` |
| Index buffer, width, first index, base vertex | `index_binding` |
| Shader buffers with exact bytes | `shader_buffers[]` + `resources.bin` |
| Textures and samplers | `textures[]`, `samplers[]` + `textures.bin` |
| Push constants | `pipeline_state.push_constant_bytes` (new) |
| Pipeline state | `pipeline_state` (new) |

### Push constants

`vk_rasterizer.cpp` already built `push_data` and recorded it per stage. The package now also carries
it as a pipeline-level block with its offset and size, because the push-constant range is a property
of the pipeline layout rather than of a stage.

### Pipeline state

Captured from the live pipeline key and register state at the draw, not reconstructed: topology,
cull mode, front face, polygon mode, depth test and write, depth compare, colour formats, sample
count, blend enables, write masks, viewport rectangle and depth range, and the push-constant range.

The colour formats are stored as the guest's packed data and number format rather than a Vulkan
enum, because that is what the pipeline key actually carries. A consumer derives the host format the
same way the renderer did, rather than being handed a value it cannot check.

## 5. Output oracle

`Core::ReferenceOutput` captures the colour attachment before and after the draw.

The pre-draw image is taken after `BeginRendering`, where the target is bound and its layout is
settled, and before the draw is emitted. The post-draw image is taken after the draw. Both use
`Image::DownloadScoped`, which SC-10S introduced: the copy is scoped to the subresource being read
and restores it to the state it was found in, so the capture does not change what the game renders.

Formats outside a known set are refused rather than guessed, because a wrong stride would produce a
plausible-looking wrong image.

**The oracle has not been exercised against a live run.** It is implemented and compiles; no
reference image has been captured from the game.

## 6. Schema changes

Schema 2 to 3. A version 2 package is refused rather than reinterpreted, because its readiness flags
did not distinguish these dimensions.

New readiness dimensions, each derived from what the package carries:

| Dimension | Requires |
| --- | --- |
| `shader_binaries_ready` | both stages, each with an effective hash, bytes and a consistent permutation hash |
| `shader_bindings_ready` | the binaries, plus at least one binding |
| `shader_parameters_ready` | the bindings, plus push-constant bytes matching their declared range |
| `pipeline_state_ready_original` | a topology and at least one colour format |
| `reference_output_ready` | a post-draw hash and a non-zero extent |
| `original_shader_replay_ready` | all of the above, plus geometry |

The strongest claim requires every dimension below it. That is the check that stops a package from
asserting it can run the original shaders when one input is missing.

## 7. Validation and negative tests

| Suite | Result |
| --- | --- |
| `shadps4_draw_package_test` | 40 passed (14 new) |
| `test_validator_adversarial.py` | 58/58 rejected (13 new) |
| `validate_draw_package.py` | accepts a complete schema 3 package, refuses schema 2 |

The new adversarial cases cover: a permutation hash that is not the combination of its parts, a
missing effective hash, missing bytes, a single stage, and each of the six readiness dimensions
removed in turn, plus a push-constant range that does not match its bytes, a pipeline state with no
topology, and a reference with no post image or no extent.

The validator independently recomputes `HashCombine` in Python, so the permutation identity is
checked by a second implementation rather than trusted from the producer.

## 8. Standalone replay

`tools/geometry_replay/original_shader_replay.cpp` is a new, narrow consumer. It:

- reads a schema 3 package and refuses any other version;
- loads each stage's sidecar and checks its length against the package;
- checks the SPIR-V magic number and word alignment;
- reports the permutation identity of each stage;
- refuses a package that does not carry both stages.

It does **not** yet build a pipeline. See section 9.

## 9. What blocks execution

The remaining step is reconstructing the descriptor layout from the SPIR-V and binding the captured
resources to it.

The package carries the bindings the production path constructed (`vk_binding`, `vk_offset`,
`vk_range`, stage, resource reference), but not the descriptor **set** layout the SPIR-V declares:
which bindings are uniform buffers, storage buffers, sampled images or samplers, and in which set.
The renderer derives that from `Shader::Info` at pipeline creation, and `Shader::Info` is not
currently retained in the package.

That is the precise missing input. It is not a compiler limitation and not a capture of the shader
bytes: the modules are present and verified. What is missing is the binding *classification* that
turns a list of ranges into a `VkDescriptorSetLayout`.

Two ways to close it, in order of preference:

1. Retain `Shader::Info`'s buffer, image and sampler descriptors alongside the binaries, which is
   the same object the renderer already has at `CompileModule`.
2. Reflect the descriptor layout from the SPIR-V with `spirv-cross` or a small parser, which needs
   no new capture but adds a dependency.

The first is the smaller change and reuses an object that is already in scope.

## 10. Native renderer handoff

The package is machine-readable and self-describing. A consumer needs:

- `shader_stages[]` for the modules and their identity;
- `shader_buffers[]` and `resources.bin` for the buffer contents;
- `vertex_bindings[]` and `index_binding` for the geometry;
- `pipeline_state` for the fixed-function state and push constants;
- `reference_output` for the comparison target;
- `readiness` to know which of these are actually present.

The outstanding gap is the descriptor set layout, as stated in section 9. A consumer cannot build a
pipeline layout from the current package alone.

## 11. Honest limits

**No emulator run was performed.** Per the standing instruction, the game was not launched. Every
claim about the capture path is a claim about code that compiles and about synthetic fixtures, not
about a live run.

**The shader capture has never executed.** `NoteCompiledShader` is called from `CompileModule`, which
only runs when the game compiles a shader. No binary has been retained from a real draw.

**The reference oracle has never executed.** Same reason.

**The candidate draw is chosen from package metadata, not from a live inventory.** The task asked for
a ranked inventory of real draws; that requires a run.

**The synthetic fixtures are synthetic.** The schema-3 packages used to exercise the validator and
the replay carry a 20-byte SPIR-V header, not a real module. They prove the plumbing, not the shader
behaviour.

**No `spirv-val` was run.** The task asked for structural SPIR-V validation where available. The
replay checks the magic number and word alignment; it does not run the full validator.

**The package is not executable.** Section 9 states the exact missing input.

## 12. Reproducible checks

```powershell
# C++ package tests
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_draw_package_test
.\build-tests\tests\shadps4_draw_package_test.exe

# Validator and adversarial suite
python tools/validate_draw_package.py <package-dir>
python tools/test_validator_adversarial.py <package-dir>

# Replay tools
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay
.\build-geometry-replay\deracine_original_shader_replay.exe <package-dir>
```

The live capture path, which has not been run:

```powershell
$env:SHADPS4_DERACINE_SHADER_BINARY='1'
$env:SHADPS4_DERACINE_REFERENCE_OUTPUT='1'
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
```

## 13. Scope

No new shader translator. No NRI work. No OpenXR. The game's rendering path is unchanged: all
instrumentation is read-only, version-gated and opt-in, and default OFF.
