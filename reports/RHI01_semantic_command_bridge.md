# RHI-01: UE4 semantic command capture and native renderer bridge

## Result

One original UE4 deferred draw command, with the state that precedes it, has been captured before
GNM generation, exported as a backend-neutral command tape, and executed by a standalone Vulkan
program that never runs the game or the emulator.

The draw arguments the renderer uses come from the UE4 command record. That is not an assertion: a
mutation test changes the tape's index count and the rendered image changes with it.

| Item | Value |
| --- | --- |
| Draw | provenance 82, deferred command capture sequence 174 |
| UE4 command | `FRHICommandDrawIndexedPrimitive`, vtable `0x03F26A88`, Execute `0x13A68B0` |
| Semantic payload | `PrimitiveType=0`, `BaseVertexIndex=0`, `FirstInstance=0`, `NumVertices=593`, `StartIndex=0`, `NumPrimitives=1012`, `NumInstances=2` |
| Derived | `index_count=3036`, `first_index=0`, `vertex_offset=0`, `instance_count=2` |
| Downstream | submission 8913, packet offset `0x1B64`, GNM index count 3036 |
| Tape | schema 1, 58 operations (36 typed, 22 opaque), 33 resources, 3 unresolved |
| Replay | NVIDIA GeForce RTX 3090 Ti, 3 views, 42,077 distinct colours |

## 1. What was already proven, and what this phase adds

SC-07R established the exact chain `UE4 deferred command -> RHI invocation -> constructed packet ->
submission/buffer/offset -> decoded PM4 -> Vulkan draw`. SC-09H established that a captured
classroom draw's geometry can be rendered independently.

Both of those start from data reconstructed **after** shadPS4 has interpreted the PS4 GPU command
stream. RHI-01 moves the observation point earlier: it records what UE4 asked its RHI to do, before
any GNM command exists, and drives a renderer from that record.

## 2. The command family, re-verified against the target binary

The A4 correction's field mapping is confirmed by disassembling the shipped image, not by trusting
the public header. `FRHICommandDrawIndexedPrimitive::Execute` at `0x13A68B0`:

```
13a68b6:  movq  0x20(%rsi), %rax     ; command list -> RHI context
13a68ba:  movq  0x10(%rdi), %rsi     ; IndexBuffer
13a68be:  movl  0x18(%rdi), %edx     ; PrimitiveType
13a68c1:  movl  0x1c(%rdi), %ecx     ; BaseVertexIndex
13a68c4:  movl  0x20(%rdi), %r8d     ; FirstInstance
13a68c8:  movl  0x24(%rdi), %r9d     ; NumVertices
13a68cc:  movl  0x28(%rdi), %r10d    ; StartIndex
13a68d0:  movl  0x2c(%rdi), %r11d    ; NumPrimitives
13a68d4:  movl  0x30(%rdi), %ebx     ; NumInstances
13a68e3:  callq 0xcbb9c0             ; IRHICommandContext::RHIDrawIndexedPrimitive
```

The constructor at `0x141015C` writes the same layout, with `vptr` at `+0x00` and `Next` at
`+0x08`. This build uses a two-entry vtable per command, not the `ExecuteAndDestructPtr` function
pointer the public 4.18.3 header declares. The public header is a guide, not a specification.

## 3. A correction this phase had to make: the primitive type

The first tape built from the captured payload refused the draw as "topology not converted". The
cause was a real error in my own conversion table, not in the capture.

UE4's `EPrimitiveType` begins `PT_TriangleList = 0`. The AMD hardware topology numbers triangle
list as 4. The captured payload carries **0**, and `1012 primitives x 3 = 3036` indices, which is
exactly the index count the downstream GNM draw reported for the same execution.

So the payload's 0 is a triangle list, and treating it as unknown would have refused a draw whose
conversion is in fact confirmed by the oracle. The SC-09H package's `primitive_type: 4` is the GPU
topology, a different enum — precisely the conflation SC-07R warned about. The tape now uses the UE4
value and the conversion is checked against the downstream index count.

## 4. StartIndex is an index offset, not a byte offset

SC-07R left this open: "the evidence is consistent with an offset but does not prove it". The PS4
RHI settles it. At `0xCBB9C0`:

```
cbb9d1:  movl  0x10(%rbp), %eax      ; StartIndex (stack argument)
cbb9eb:  cmpl  $0x0, 0x88(%rdx)      ; index element width from the resource
cbb9f2:  setne %sil
cbb9f6:  leal  0x2(%rsi,%rsi), %esi  ; 2 or 4 bytes per index
cbb9fa:  imull %eax, %esi            ; StartIndex * element size
cbb9fd:  movq  %rsi, -0x40(%rbp)     ; byte offset into the index buffer
```

`StartIndex` is multiplied by the element width to form the byte offset. It is therefore an index
offset and is **not** folded into the base address. The tape preserves it as `start_index` and
derives `index_byte_offset` separately, so both the original and the derived value are visible.

## 5. The state-persistence problem, and what it cost

The task warned: "A draw may depend on state set much earlier." That is exactly what happened, and
it is the phase's most important negative finding.

The PS4 RHI **caches the viewport on the context**, not in the command list. `SetViewport` at
`0x0CB8AE0` compares the incoming rectangle against `+0x80/+0x84/+0x88/+0x8C` and `+0xA0/+0xA4`
and stores it back there only when it differs:

```
cb8f6a:  vmovss %xmm9, 0x80(%r13)    ; MinX
cb8f73:  vmovss %xmm8, 0x84(%r13)    ; MinY
cb8f81:  vmovss %xmm0, 0x88(%r13)    ; MaxX
cb8f8f:  vmovss %xmm0, 0x8c(%r13)    ; MaxY
cb8faa:  vmovss %xmm0, 0xa0(%r13)    ; MinZ
cb8fb8:  vmovss %xmm0, 0xa4(%r13)    ; MaxZ
```

A GNM viewport packet is emitted only when the value changes. The selected draw's command list
(`0x101065D400`) carries **no viewport command at all** — the viewport was set earlier, on another
list, and the draw inherited it from the context cache.

The existing capture could not recover it, for a second reason: it read the RHI context only for
draw commands, so every viewport and render-target record carried `context=0x0`. The state was
recorded but unattributable.

**Both defects are fixed in this branch:**

- `rhi_command_capture.cpp` now reads the command list's context for every command, not only draws.
- `rhi_entry_probe.cpp` reads the effective viewport from the context at the draw's RHI entry, which
  is the value the draw actually consumed, and stores it on the provenance record.

The tape produced from the **existing** SC-09H log therefore still has no viewport: that log was
written before the fix. The live exporter would carry it. This is reported as an unresolved
dependency rather than defaulted, and the interpreter refuses to execute without it unless the
caller supplies one explicitly.

## 6. The tape

Schema 1, versioned, backend-neutral. Operations are typed only when their source and semantics are
established; anything else is an `Opaque` record carrying its capture sequence, so the tape is
honest about what it saw rather than silently dropping it.

```
SetRenderTargets   evidence=verified_target_binary
BindVertexBuffers  evidence=tentative            (x34)
Opaque             evidence=tentative            (x22)
DrawIndexed        evidence=verified_target_binary
```

Every operation carries an evidence class: `verified_target_binary`, `public_source_correspondence`,
`runtime_confirmed`, or `tentative`. A reader can tell a disassembly-backed fact from a shape match
without reading this report.

Resources carry a provenance class that is deliberately separate from the command's evidence:
`ue4_object`, `downstream_oracle`, or `unresolved`. A UE4-origin command referencing an
oracle-recovered resource is a valid intermediate state, and the tape can say both at once.

### Draw payload preservation

The original semantic payload is preserved verbatim alongside the derived values:

```json
"draw":    { "primitive_type": 0, "base_vertex_index": 0, "first_instance": 0,
             "num_vertices": 593, "start_index": 0, "num_primitives": 1012,
             "num_instances": 2 },
"derived": { "valid": true, "index_count": 3036, "first_index": 0,
             "vertex_offset": 0, "instance_count": 2, "index_byte_offset": 0 }
```

The payload is not collapsed into GNM index count and hardware address. The conversion lives in one
function, `DeriveDraw`, which the exporter, the interpreter and the tests all call.

## 7. Provenance, preserved

The SC-07R identity rules are untouched. The tape records the submission id and packet offset, and
`packet_offset_known` is a separate flag so offset zero is not overloaded as "unknown".

For the selected draw:

| Layer | Value |
| --- | --- |
| Deferred command | capture sequence 174, list `0x101065D400` |
| RHI index resource | `0x101355BCE8` |
| GNM index address | `0x201DBE6000` |
| Submission | 8913 |
| Packet offset | `0x1B64` |
| GNM index count | 3036 |
| Vulkan draw | 1 child draw |

The tape and the SC-09H package describe the same execution: both are provenance 82, and the
package's index resource is the same 3036-element buffer.

### An identity defect found and fixed

The first tape selected the wrong draw. The provenance record's `command_capture_id` is the
deferred-command ordinal the wrapper assigned on its own thread
(`RhiCommandCapture::g_deferred_sequence`), **not** the capture slot's sequence number. Matching on
it selected a different draw with 1356 vertices and 958 primitives.

The exact identity available is the pair the RHI entry recorded: the command list the draw was
issued on, and the index-buffer resource it was given. The offline builder now matches on both and
refuses an ambiguous match rather than taking the first candidate.

## 8. The interpreter

`tools/geometry_replay/tape.cpp` is backend-neutral: it knows nothing about Vulkan. It reads a
tape, validates it, and replays it into a `CommandSink`. The Vulkan backend implements the sink.
That separation is the point — an NRI backend would implement the same sink without touching the
tape or its reader.

The interpreter refuses a tape that is not executable rather than defaulting missing state. Where a
caller supplies missing state, the substitution is named in the report:

```
substituted_state: viewport graphics_pipeline
```

## 9. The architectural test

The requirement was that the renderer obtain its draw count, first index, base vertex and instance
count from the UE4-origin record, not from the captured PM4 packet.

**Mutation test.** The tape's `num_primitives` was changed from 1012 to 500 and `index_count` from
3036 to 1500, and the replay re-run:

| Tape | Executed draw | Distinct colours | Non-background pixels |
| --- | --- | --- | --- |
| Original | `index_count=3036` | 42,077 | 79,528 |
| Mutated | `index_count=1500` | 19,009 | 35,721 |

The executed draw followed the tape, and the image changed with it. The renderer is not reading the
PM4 packet.

## 10. What is genuinely UE4-origin, and what is not

Reported separately, as required:

| Dimension | Status |
| --- | --- |
| Semantic commands recovered | 36 typed operations from the deferred command record |
| RHI resource identities recovered | index buffer from the UE4 payload; render targets from the command payload |
| Downstream resources supplied by the oracle | all geometry bytes, via the SC-09H package |
| Native operations executed | viewport, render targets, vertex/index binding, indexed draw |
| Original shaders/state substituted | diagnostic vertex/fragment shaders; diagnostic pipeline |
| Missing semantic dependencies | viewport (this log), bound-shader-state, shader resources |

**This is not a native PC port.** The command provenance is UE4's; the resource contents are
shadPS4's. A draw executed from UE4-origin commands over shadPS4-exported resources is the
intermediate milestone the task defines, and it is labelled as such in the program's own output.

## 11. Resource-ownership gap analysis

The chain the task asks for is
`UE4 object -> RHI command reference -> PS4 RHI resource -> effective GPU resource -> package resource`.

| Edge | Status |
| --- | --- |
| UE4 command -> index-buffer reference | **recovered.** `+0x10` of the command object, read at Execute |
| index reference -> PS4 RHI resource | **recovered.** The RHI receives it as its second argument |
| PS4 RHI resource -> effective GPU resource | **recovered.** `0x201DBE6000`, from the GNM draw |
| effective GPU resource -> package resource | **recovered.** Package resource 11, 6072 bytes, hash verified |
| UE4 command -> vertex-buffer references | **partial.** The stream-source payload is an interface pointer adjusted by `-0x68` in Execute, not a resource base, so it is carried as `tentative` |
| UE4 command -> render targets | **recovered.** Payload arrays at `+0x18`/`+0xD8` |
| UE4 command -> shader/pipeline | **missing.** `FRHICommandSetBoundShaderState` is not in the recovered cluster |
| resource reference counting / lifetime | **not attempted.** Out of scope for one draw |

The vertex-buffer edge is the honest weak point. The stream-source command's payload is an interface
pointer, and the Execute wrapper subtracts `0x68` to reach the resource base. Treating the raw
payload as a resource identity would be wrong, so the tape marks it `tentative` and says why.

## 12. Tests

50 tests, all passing, exercising the production functions rather than reimplementing their
formulas: 33 C++ contract tests and 17 Python builder tests.

```
[==========] 33 tests from 6 test suites ran.
[  PASSED  ] 33 tests.

Ran 17 tests in 0.000s
OK
```

Coverage of the required negative cases:

| Case | Test |
| --- | --- |
| Incorrect command vtable | `ClassifyCommandVtable` range/stride tests (existing suite) |
| Incorrect payload offset | payload offsets are asserted against the disassembly in the report; the tape refuses a draw whose conversion fails |
| Stale command object | deferred scope stack-depth guard (existing suite) |
| Different command-list contexts | tape keeps only the draw's own list |
| Deferred versus direct RHI calls | `with_command`/`without_command` accounting (existing suite) |
| Missing preceding state | `MissingViewportIsReportedNotDefaulted`, `MissingRenderTargetsIsReportedNotDefaulted` |
| Reordered state-setting commands | `StateAfterTheDrawDoesNotCount` |
| Multiple draws sharing state | `TwoDrawsOnOneTapeAreRefused` |
| Resource destruction before consumption | not covered: no lifetime model in this phase |
| Invalid resource references | `DanglingIndexResourceIsRefused`, `DanglingRenderTargetIsRefused` |
| Semantic draw-count conversion | `ClassroomConversionMatchesTheDownstreamIndexCount` |
| Base-vertex and first-index conversions | `StartIndexBecomesAnIndexOffsetNotAByteOffset`, `NegativeBaseVertexIsPreservedAsSigned` |
| Duplicate or ambiguous provenance | offline builder refuses an ambiguous match |
| Unresolved shader/pipeline objects | `UnresolvedIndexResourceIsNotExecutable`; pipeline reported as substituted |
| Tape schema mismatch | `SchemaMismatchIsRefused` |

Two cases are explicitly **not** covered and are labelled as such: resource destruction before
consumption (no lifetime model exists yet) and the incorrect-payload-offset case at the tape level
(the offsets are verified statically, and a wrong offset would produce a payload that fails the
conversion, but there is no test that feeds a deliberately wrong offset through the live hook).

## 13. Reproducible checks

```powershell
# Build the emulator (the live exporter)
pwsh -File _build\build.ps1 -Root <worktree>

# Build the tests and run the tape contract suite
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_semantic_tape_test
.\build-tests\tests\shadps4_semantic_tape_test.exe

# Build the standalone replays
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay

# Run the offline builder's tests
python tools/test_semantic_tape_from_log.py

# Build a tape from an existing capture log (offline path)
python tools/semantic_tape_from_log.py <shad_log.txt> <output-dir> --draw 82

# Execute the tape
.\build-geometry-replay\deracine_tape_replay.exe <output-dir> tape \
    --substitute-viewport --substitute-render-targets
```

The live path, which additionally captures the effective viewport:

```powershell
$env:SHADPS4_DERACINE_CMD_CAPTURE='1'
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER='33'
$env:SHADPS4_DERACINE_DRAW_PROVENANCE='1'
$env:SHADPS4_DERACINE_VERTEX_INPUT='1'
$env:SHADPS4_DERACINE_SHADER_PARAMS='1'
$env:SHADPS4_DERACINE_DRAW_PACKAGE='<output dir>'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE='1'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR='<output dir>'
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW='82'
$env:SHADPS4_EXIT_AFTER='75'; $env:SHADPS4_CLEAN_EXIT='1'
```

## 14. Honest limits

**The viewport in the committed tape is substituted, not recovered.** The tape was built from the
SC-09H log, which predates the context-capture fix. The live exporter reads the effective viewport
from the RHI context at draw entry; that path is implemented and builds, but it has not been
exercised against a live run in this phase. The interpreter reports the substitution rather than
hiding it.

**The pipeline is substituted.** `FRHICommandSetBoundShaderState` is not in the recovered command
cluster, so the diagnostic pipeline is bound and named as substituted.

**Only the position stream is rendered.** The package carries five streams; the diagnostic renderer
consumes position alone.

**The transform is diagnostic.** The object is centred and scaled to fit the viewport. That is not
the game's transform.

**No Vulkan validation layers were enabled.** The draws completed without device loss, which is a
weaker statement.

**The scene identity is not established.** The mesh is a verified 3D object of unknown scene
identity, as SC-09H reported.

**The game's rendering path is unchanged.** All hooks are read-only, version-gated and opt-in. No
GNM path was disabled, no RHI method was patched, and nothing was rerouted.

## 15. What this phase establishes, and what it does not

It establishes that a native renderer can consume recovered Unreal RHI commands: the draw arguments
come from UE4's deferred command record, and a mutation of that record changes the rendered result.

It does not establish that the running game uses that renderer. It does not. The architectural
boundary has moved from

`UE4 -> PS4 RHI -> GNM -> PM4 -> shadPS4 -> Vulkan`

to a demonstrated

`UE4 -> semantic tape -> native Vulkan`

for one draw, with the resource contents still supplied by the downstream oracle. Closing that last
gap — resource creation and shader binding from UE4 objects — is the next milestone.
