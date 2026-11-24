# SC-11R: original shader binding and reference capture correctness

## Result

The reference-output oracle is now synchronized and lifetime-safe, the descriptor interface of each
effective shader permutation is captured, an independent verifier checks that interface against the
SPIR-V, and a standalone Vulkan executable builds and runs a graphics pipeline from real shader
modules with a full descriptor set.

**No Déraciné shader has been executed.** No emulator run was performed. The pipeline is proven with
independently compiled test SPIR-V, which establishes the infrastructure, not original material
fidelity.

| Item | Value |
| --- | --- |
| Base | `be57718d` (SC-11) |
| Package schema | 4 |
| C++ package tests | 49 passing (9 new) |
| SPIR-V verifier tests | 12 passing (new) |
| Adversarial cases | 62/62 rejected (4 new) |
| GPU sync fixture | 4/4 properties, 0 validation errors |
| Standalone pipeline | executes, 65536 px, 16 colours, 0 validation errors |
| Real Déraciné shader executed | **no** |

## 1. The synchronization defect, and its repair

SC-11's `CaptureColorTarget` did this:

```cpp
image.DownloadScoped(...);          // records Vulkan commands
out.bytes.resize(byte_size);
std::memcpy(out.bytes.data(), download, byte_size);   // reads immediately
```

`DownloadScoped` records a copy. It does not complete it. The immediate CPU read therefore read
whatever the download buffer held before the GPU ran.

### The repair

The capture now allocates a **dedicated host-visible staging buffer per capture** and defers the CPU
read through `Scheduler::DeferPriorityOperation`, which runs only once the GPU has passed the tick
the copy was recorded at. The allocation is owned by the deferred callback, so it stays valid until
the read and is released immediately after.

A dedicated buffer rather than the shared download utility buffer, for two reasons:

1. The pre-draw and post-draw copies must not overwrite each other.
2. The shared buffer's `Commit()` calls `vmaInvalidateAllocation` **before** the copy executes,
   which is the wrong order for non-coherent memory. A coherent allocation needs no invalidate at
   all.

Each capture carries a transfer identity from `NextTransferId()`. `CompleteCapture` rejects a
readback whose identity does not match the slot's pending identity, so a late readback from a
previous capture cannot be attributed to a different draw.

`GetReference` returns false unless the post-draw readback is `Complete`. A pending or failed
capture is not a reference, so an unverified hash cannot become an oracle.

### The GPU proof

`tools/geometry_replay/reference_sync_test.cpp` reproduces the pattern on a real GPU with both
staging buffers poisoned to `0xAA` and `0xBB`:

```
negative control (read before completion): PASS  saw the staging buffer's prior contents
deferred read after completion: PASS  the read saw the copied contents
separate staging buffers: PASS  the second capture did not overwrite the first
validation errors: 0
reference sync test: PASS
```

The negative control is the important line. It demonstrates the defect is real and that the test can
see it, rather than only asserting the corrected path works.

The validation layer also caught a genuine bug in the test itself: freeing a command buffer while it
was still pending (`VUID-vkFreeCommandBuffers-pCommandBuffers-00047`). That is fixed.

## 2. Render-pass ordering

SC-11 captured the pre-draw reference after `BeginRendering(pipeline)` but before
`scheduler.BeginRendering(state, ...)`.

That is wrong. `BeginRendering(pipeline)` only constructs the render state; `scheduler.BeginRendering`
is what records the Vulkan render pass and therefore what executes the attachment load operation.
`vk_scheduler.cpp` sets `loadOp = cb.is_clear ? eClear : eLoad`, so a capture taken before the pass
reads the attachment as it was before the clear.

The capture now runs **after** `scheduler.BeginRendering`.

When the pass clears the attachment, the inherited contents are the clear value, not the image's
previous contents. That case is reported as unsupported rather than captured: the clear is already
recorded into the same command buffer, so a copy recorded after it reads the cleared image and a copy
recorded before it reads the wrong state. There is no capturable pre-draw image for that
configuration, and saying so is more useful than producing a wrong one.

Render-pass semantics were not modified to make the capture succeed.

## 3. Capture size and format

`MaxReferenceBytes` was 32 MiB, which cannot hold a 2880x3240 RGBA8 target (37,324,800 bytes,
35.6 MiB).

The bound is now configurable through `SHADPS4_DERACINE_REFERENCE_OUTPUT_MAX_BYTES`, defaulting to
64 MiB with a 512 MiB hard ceiling. A value outside the accepted range is refused rather than
clamped.

Failures are explicit through `CaptureStatus`: `Oversize`, `UnsupportedFormat`, `Multisampled`,
`NoColorTarget`, `AllocationFailed`, `CopyFailed`. An oversize capture is never cropped,
downscaled or truncated, because a reference that is not the whole attachment cannot be compared
pixel for pixel.

Multisampled attachments are refused rather than resolved, because this capture does not resolve and
copying a sample plane would present it as the image.

## 4. Descriptor interface capture

`Core::ShaderInterface` retains, per effective permutation: program hash, permutation index,
permutation hash, hardware and logical stage, patch state, and the descriptor declarations.

The layout is derived **exactly as `GraphicsPipeline::BuildDescSetLayout` derives it**: buffers
first, then images, then samplers, with a single unified binding counter advancing across all three.
That is the production allocation, so the captured interface is the one the renderer actually built.

Each descriptor records set, binding, class, count, stage flags, guest resource index and whether it
is written. The push-constant block records offset, size and stage flags.

Metadata is copied by value at the compiler boundary. Nothing borrows a pointer into mutable
`Shader::Info` or a temporary binding allocation.

The identity is `(program_hash, permutation_index)`. One program can produce permutations with
different resource interfaces, so a lookup by program hash alone is insufficient.

## 5. Independent SPIR-V verification

`tools/geometry_replay/spirv_interface.cpp` reads a module and extracts the descriptor declarations
it actually makes: set and binding decorations, descriptor class, array count, push-constant block
and entry point.

It is deliberately small and handles only the constructs a shader uses for descriptors. Anything
outside that set is skipped by its word count, which is what makes it robust to constructs it does
not interpret.

`CompareInterface` rejects: a binding the module requires but the capture omits, a class mismatch, a
runtime-sized array, an array count mismatch, and a push-constant range that does not cover what the
module reads.

The verifier found a real bug in itself during testing: the entry-point name loop broke out of the
byte loop but not the word loop, so the words after the null terminator were appended to the name.
The test caught it because it compares against `"main"`.

## 6. Standalone Vulkan pipeline

`tools/geometry_replay/original_shader_pipeline.cpp` builds and executes a graphics pipeline from
shader modules and their verified descriptor interface. It has no knowledge of a particular shader
hash: the layout is derived from what the modules declare.

The self-test uses real SPIR-V compiled by `glslc` from `test_shaders/sc11r_test.vert` and
`.frag`, both validated by `spirv-val`. The shaders exercise one vertex attribute pair, one uniform
buffer, one sampled texture, one sampler and one push-constant field.

```
selftest: vertex descriptors=1 fragment descriptors=1 pipeline descriptors=2 push_constant=16
  set=0 binding=0 type=6 count=1     (uniform buffer)
  set=0 binding=1 type=1 count=1     (combined image sampler)
validation_layers=enabled
device: NVIDIA GeForce RTX 3090 Ti
pipeline: created
result: non_black=65536 distinct_colors=16 validation_errors=0
check: covered=yes sampled=yes validation_clean=yes
```

The rendered image shows the 4x4 texture's gradient modulated by the uniform tint, which is what a
correct execution of those shaders produces.

**This establishes infrastructure, not original material fidelity.** The shaders are test shaders.

## 7. Schema 4

Adds `shader_interfaces[]` and replaces the zero-as-sentinel topology check with an explicit
`has_topology` flag. Zero is a legitimate Vulkan topology value, so it cannot mark missing data.

`shader_bindings_ready` no longer means "some binding exists". It now requires:

- every stage to carry an interface for its permutation;
- every descriptor to have a non-zero count;
- every buffer or image descriptor to resolve to a captured binding.

A sampler is not required to resolve to a buffer, because it legitimately has none.

## 8. Tests

| Suite | Result |
| --- | --- |
| `shadps4_draw_package_test` | 49 passed (9 new) |
| `shadps4_spirv_interface_test` | 12 passed (new) |
| `test_validator_adversarial.py` | 62/62 rejected (4 new) |
| `deracine_reference_sync_test` | 4/4 properties, 0 validation errors |
| `deracine_original_shader_pipeline --selftest` | executes, 0 validation errors |

The new adversarial cases cover a missing interface, a stage without an interface, an interface
binding with no captured resource, and a zero descriptor count.

## 9. RHI-02 reconciliation

`docs/research/native-renderer/SC11R_RHI02_RECONCILIATION.md` documents the plan. The two branches
are complementary: RHI-02 owns the semantic tape and draw identity, SC-11R owns original-shader
execution. Both descend from `da969d4a`.

The key points:

- RHI-02's `package_reader` should become the single reader, extended to schema 4.
- SC-11R's inline readers should be deleted in favour of it.
- The original-shader pipeline should take draw arguments from the tape via `draw_identity`, not
  from the package's `index_binding`.
- The stage identity and descriptor interface are additive; nothing existing changes meaning.

**No merge has been performed.** Integration requires separate authorisation.

## 10. Identity vocabulary

The five identities stay distinct and live in separate fields:

| Identity | Field |
| --- | --- |
| Original UE4 shader/resource identity | not recovered |
| Original GCN program identity | `program_hash` |
| shadPS4 permutation identity | `permutation_hash` |
| Effective SPIR-V module identity | `effective_sha256` |
| Vulkan descriptor interface | `shader_interfaces[]` |

**The permutation hash is not an original-game shader identifier.** It is a shadPS4-internal value
derived from the program hash and a compilation-order index. A-028R established that the GNM
metadata carries the original program identity; the permutation index is an artefact of this
emulator's specialization strategy.

## 11. What remains untested

**No emulator run.** The shader binary capture, the interface capture and the reference oracle have
never executed against the game. Every claim about them is a claim about code that compiles and about
synthetic fixtures.

**The captured-package path of the pipeline is not wired.** `original_shader_pipeline` accepts
`--selftest` only. Reading a schema 4 package and driving the same pipeline from it is the next
step, and it needs a real capture to be meaningful.

**The reference oracle has never captured a real attachment.** Its synchronization contract is
proven on a GPU fixture; its behaviour inside the emulator's render pass is not.

**The pre-draw reference is unsupported for a clearing pass.** That is a documented limitation, not
a defect.

**No `spirv-val` was run inside the pipeline.** The test shaders were validated by `spirv-val` at
build time; the pipeline checks the magic number and word alignment only.

## 12. Reproducible checks

```powershell
# C++ tests
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_draw_package_test
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_spirv_interface_test
.\build-tests\tests\shadps4_draw_package_test.exe
.\build-tests\tests\shadps4_spirv_interface_test.exe

# Validator and adversarial suite
python tools/validate_draw_package.py <package-dir>
python tools/test_validator_adversarial.py <package-dir>

# GPU fixtures (Release: the Debug tree's CRT flags abort on the debug heap)
cmake -S tools/geometry_replay -B build-geometry-replay-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-geometry-replay-rel
.\build-geometry-replay-rel\deracine_reference_sync_test.exe
.\build-geometry-replay-rel\deracine_original_shader_pipeline.exe --selftest \
    --shader-dir tools/geometry_replay/test_shaders
```

The live capture procedure is in
`docs/research/native-renderer/SC11R_LIVE_CAPTURE_RUNBOOK.md`. It has not been executed.

## 13. A build note

The standalone replay CMake project inherits `-D_DLL -D_MT` together from the MSVC toolchain, which
is contradictory. In a Debug build the debug heap aborts (exit 3) on the first allocation-heavy path.
The GPU fixtures must be built Release. This is a pre-existing property of the standalone project,
not something this phase introduced, and it is recorded here rather than worked around silently.

## 14. Scope

No new shader translator. No NRI work. No OpenXR. No whole-frame rendering. The game's rendering path
is unchanged: all instrumentation is read-only, version-gated and opt-in, and default OFF.
