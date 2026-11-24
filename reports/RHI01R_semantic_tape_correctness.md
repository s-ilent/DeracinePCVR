# RHI-01R: semantic tape correctness and producer parity

## Result

Three correctness defects in the RHI-01 semantic tape are repaired. The live C++ exporter and the
offline Python builder now run the same draw-resolution and tape-construction logic, and the
independent reader refuses a tape whose stored derived values contradict its own semantic payload.

| Item | Value |
| --- | --- |
| Base | `5475e344` (RHI-01) |
| C++ contract tests | 77 passing |
| Python builder tests | 34 passing |
| Existing 33-resource tape | still builds and replays |
| RHI-01 diagnostic replay | unchanged, 42,077 colours |

## 1. Draw identity: the defect and the repair

The live exporter resolved the deferred draw with:

```cpp
if (command.sequence != snapshot.command_capture_id)
    continue;
```

Both sides are counters, but they are different counters. `CapturedCommand.sequence` is the
capture-slot ordinal, assigned as `index + 1` in `rhi_command_capture.cpp`.
`ProvenanceSnapshot.command_capture_id` comes from the thread-local `g_deferred_sequence`, a
per-thread counter incremented at wrapper entry. Comparing them selects whichever command happens to
sit at that slot number.

The offline builder had already found and corrected this. The live exporter had not.

### The repair

`MatchDrawCommand` now lives in `semantic_tape_contract.h` and is called by both producers and by
the tests. It matches in two stages:

1. **Identity.** Command list and index resource must both agree. Both come from the same RHI
   invocation, so they identify the draw rather than merely correlating with it.
2. **Disambiguation.** When more than one candidate survives, the semantic payload fields the
   provenance record independently carries are compared. A candidate that disagrees on any known
   field is excluded.

A zero command list or zero index resource is `UnusableIdentity` rather than a wildcard. More than
one surviving candidate is `Ambiguous` rather than a first-match. No timestamp, draw size or packet
proximity is used.

### Sequence-counter independence

Two tests vary the counters independently:

- `SelectionDoesNotDependOnEitherCounter` records the same draw at capture slots 1, 7, 174 and
  9999 while the provenance record carries a deferred ordinal matching none of them. The selected
  draw is the same every time.
- `TheWrongDrawIsNotSelectedWhenCountersDisagree` places a decoy at slot 635, the deferred ordinal
  the record carries. Matching on that counter would pick the decoy. The matcher picks the real draw
  (1012 primitives, 593 vertices).

The Python builder has the equivalent tests, so a divergence between the two producers fails on one
side.

### A bug this work introduced and then fixed

The first version of `BuildTapeFromCapture` mapped the matcher's result through a second array,
`candidate_record_index[matched_index]`. `MatchDrawCommand` already returns the capture record
index, so the second mapping resolved correctly only when the draw was the first candidate. Two
tests caught it. The mapping is gone and the comment says why.

## 2. Resource capacity: one contract, three implementations

The C++ schema declared `std::array<Resource, 32>` while the reported tape carries 33 resources.
The live `intern_resource()` returned zero when `AppendResource()` failed, which could leave a
tape that validated structurally while referencing an identity it did not carry.

### The contract

`MaxTapeResources = 64`, defined once in `semantic_tape_contract.h` and mirrored as
`MAX_TAPE_RESOURCES` in the Python builder and `kMaxTapeResources` in the reader.

The bound is justified rather than round. The observed classroom draw needs 33: one index buffer,
one colour target, one depth target and thirty vertex-stream references. The widest draw the
recovered command family can describe needs at most 34: index, depth, eight colour targets, eight
UAVs and sixteen vertex streams. 64 is roughly twice that worst case, so a real draw fits with
headroom while a runaway capture still fails at a fixed, compile-time bound.

### Behaviour

- Exhaustion is a hard failure in both producers. No path returns a zero resource id and continues.
- A repeated reference to the same guest identity reuses its id and consumes no further slot.
- Both producers validate their own output before serialization.
- The reader rejects a tape over capacity rather than partially reading it.

### Tests

| Case | C++ | Python |
| --- | --- | --- |
| 33-resource tape supported | `TheObservedThirtyThreeResourceTapeIsSupported` | `test_the_observed_thirty_three_resource_tape_is_supported` |
| Exactly at capacity accepted | `ExactlyAtCapacityIsAccepted` | `test_exactly_at_capacity_is_accepted` |
| One over capacity refused | `OneOverCapacityIsRefused` | `test_one_over_capacity_is_refused_by_the_validator` |
| Late overflow after many valid operations | `LateOverflowAfterManyValidOperationsFailsExplicitly` | `test_late_overflow_fails_explicitly` |
| Repeated references consume no slot | `RepeatedReferencesConsumeNoAdditionalSlots` | `test_repeated_references_consume_no_additional_slots` |
| Reader rejects over-capacity | `TheReaderRejectsATapeOverCapacity` | — |

The late-overflow test is the important one: it fills the table with distinct vertex streams and
then adds the draw, so the overflow happens after many otherwise valid operations. It must still
fail rather than truncate.

## 3. The semantic payload is authoritative

The tape carries both the original UE4 `DrawIndexedPayload` and the derived Vulkan parameters. The
interpreter previously trusted the serialized derived values. The RHI-01 mutation test changed both
sides at once, so it established tape control but not independently verified conversion.

### The contract

`DeriveDraw` is the single conversion, and it now exists in all three implementations. The reader
recomputes from the payload and compares:

```
index_count       = num_primitives * 3        (triangle list)
first_index       = start_index
vertex_offset     = base_vertex_index         (signed)
instance_count    = num_instances
index_byte_offset = start_index * element_size
```

`ValidateTape` returns `DerivedInconsistent` when the stored values disagree. `Execute` recomputes
and hands the sink the recomputed values, so a backend never receives an unverified number.

The established corrections are preserved: UE4 `PT_TriangleList = 0`, the AMD GPU topology's
triangle list is a different enum with value 4, and `StartIndex` is an index offset rather than a
byte offset.

### Signed vertex offset

`vkCmdDrawIndexed` takes `int32_t vertexOffset`. The sink previously cast through `u32`, which
would turn a negative base vertex into a huge positive offset. It now passes the signed value
directly.

### The two required tests

**Negative consistency test.** Change only `num_primitives` from 1012 to 500, leaving the
serialized derived block at 3036:

```
tape is not executable: derived_inconsistent
```

The interpreter refuses it. Covered by `TheTapeValidatorRejectsAnInconsistentDraw` and
`AChangedPrimitiveCountIsDetected`.

**Positive semantic mutation test.** Change only the source payload, regenerate through the
production derivation path, and execute:

| Source | Derived | Executed | Distinct colours | Non-background pixels |
| --- | --- | --- | --- | --- |
| 1012 primitives | 3036 indices | 3036 | 42,077 | 79,528 |
| 500 primitives | 1500 indices | 1500 | 19,009 | 35,721 |

The Vulkan draw follows the newly derived count and the image changes with it.

Also covered: nonzero first index (168 → byte offset 336), nonzero first instance, negative base
vertex (−7 preserved as signed), unsupported topology (refused), and primitive-count overflow
(refused).

## 4. Producer parity

`BuildTapeFromCapture` was extracted from the exporter into `semantic_tape_contract.h`. The live
exporter is now a thin adapter that gathers capture records and provenance facts into plain values.
The tests fill the same structures from synthetic fixtures and call the same function.

This is what makes the live logic testable without a live capture, and it is why the two producers
cannot drift: they are the same code path in C++, and the Python builder mirrors it with tests
asserting the same fixture values.

Parity coverage:

| Dimension | C++ | Python |
| --- | --- | --- |
| Draw identity | `ResolvesByListAndIndexResource` | `test_resolves_by_list_and_index_resource` |
| Operation ordering | `PreservesOperationOrderAndExcludesOtherLists` | `test_state_from_another_list_is_excluded` |
| Resource identities | `RepeatedReferencesConsumeNoAdditionalSlots` | `test_repeated_references_consume_no_additional_slots` |
| Resource limits | capacity suite | `Capacity` suite |
| Primitive conversion | `ClassroomConversionMatchesTheDownstreamIndexCount` | `test_classroom_draw_matches_the_cpp_expectation` |
| Missing viewport | `ReportsAMissingViewportRatherThanInventingOne` | `test_missing_viewport_is_reported_not_invented` |
| Inherited viewport snapshot | `InsertsAnInheritedViewportSnapshotBeforeTheDraw` | — |
| Missing render targets | `ReportsMissingRenderTargets` | `test_missing_viewport_and_render_targets_are_both_reported` |
| Multiple unresolved dependencies | `ReportsMultipleUnresolvedDependencies` | `test_missing_viewport_and_render_targets_are_both_reported` |
| Ambiguous draw matches | `IndistinguishableDrawsAreRefusedAsAmbiguous` | `test_an_ambiguous_match_is_refused` |
| Sequence-counter disagreement | `SelectionDoesNotDependOnEitherCounter` | `test_sequence_counter_disagreement_does_not_change_selection` |

Byte-identical JSON is not required and is not asserted. The tests compare semantic content and
failure behaviour.

### Multiple simultaneous problems

The task asked whether a single `CheckExecutable` failure conceals another unresolved dependency.
It did. `CheckExecutable` returns one problem, which is the right shape for a caller that only needs
a yes/no answer and the wrong shape for a report.

`CollectProblems` now returns the full set as a bitmask. A tape with only a draw reports
`NoViewport`, `NoRenderTargets` and `NoGraphicsPipeline` together, while `CheckExecutable` still
returns just `NoViewport`. Covered by `CollectsEveryMissingDimensionAtOnce`.

Diagnostic substitutions remain explicit. `substituted_state: viewport graphics_pipeline` is
printed, and a substitution never satisfies a claim of original RHI state recovery.

## 5. Live viewport capture preserved

The effective-viewport read from the PS4 RHI context at draw entry is unchanged, including its
bounded guest-memory checks. Its decoding and insertion are now tested with synthetic context data:
`InsertsAnInheritedViewportSnapshotBeforeTheDraw` supplies a 2880x3240 snapshot and asserts the
viewport operation precedes the draw, carries `Evidence::RuntimeConfirmed`, and holds the snapshot
values.

**This path is not runtime-validated.** The committed tape still substitutes its viewport because
the SC-09H capture predates the fix. No new game capture was made, so no runtime claim is made.

## 6. What remains untested

- **The live exporter has not run.** No emulator launch was performed. `BuildTapeFromCapture` is
  exercised by synthetic fixtures and the adapter is compiled, but the path from a live capture
  table through `ExportTape` to a written file is unverified.
- **The live viewport read is unverified at runtime.** Its decoding and insertion are tested; the
  guest-memory read against a real RHI context is not.
- **The 33-resource tape was produced by the offline builder.** The live exporter has not been shown
  to produce the same tape from the same execution.
- **Resource destruction before consumption** remains uncovered. There is no lifetime model in this
  phase.

## 7. Reproducible checks

```powershell
# C++ contract tests
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_semantic_tape_test
.\build-tests\tests\shadps4_semantic_tape_test.exe

# Python builder tests
python tools/test_semantic_tape_from_log.py

# Standalone replays
cmake -S tools/geometry_replay -B build-geometry-replay -G Ninja
cmake --build build-geometry-replay

# Rebuild the tape and replay it
python tools/semantic_tape_from_log.py <shad_log.txt> <output-dir> --draw 82
.\build-geometry-replay\deracine_tape_replay.exe <output-dir> tape \
    --substitute-viewport --substitute-render-targets
```

## 8. Scope

This is a bounded repair. No shader translation, texture capture, NRI, OpenXR or broader RHI reverse
engineering was attempted. The game's rendering path is unchanged: all instrumentation remains
read-only, version-gated and opt-in, and default OFF.
