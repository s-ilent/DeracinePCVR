# RHI-02I-R3: capture selection and index-resource provenance correctness

## Result

The finalizer no longer reads a vertex-buffer address as the index address. The package-selected
provenance is authoritative for tape export, the bridge record is selected by session and provenance
rather than array position, and the manifest reports selection and verification separately.

**Production wiring demonstrated offline.** The tests invoke the actual replay executable against
independently authored artifacts. **Original Déraciné same-draw verification is pending** — it
requires an authorised game capture.

| Item | Value |
| --- | --- |
| Branch | `research/rhi02ir3-capture-selection-20261125` |
| Base | `da66cd1b107d6b9d86dfeff523fbc540e8823f3b` |
| Selection integration cases | 20/20 |
| RHI-02I-R2 integration cases | 19/19 |
| Unit tests | 95 passing |
| Runtime verification | **pending** |

## 1. Source defects reproduced

| # | Defect | Confirmed at |
| --- | --- | --- |
| A | index address read by global text search | `capture_session.cpp:165`, `FindNumber(package_json, "guest_base", ...)` |
| B | missing address accepted | `capture_session.cpp:205`, `package_index_base != 0 &&` guard |
| C | tape defaults to the last recorded draw | `semantic_tape.cpp:451`, `LastRecordedProvenanceId()` |
| D | `SelectPair()` never called | no caller in `src/src` |

Defect A is concrete: `draw_package.cpp` writes `vertex_bindings` at line 171 and `index_binding` at
line 197, and both carry a `guest_base` field. The global search therefore returned the first vertex
address.

## 2. Production changes

### Structural artifact readers

`src/src/core/capture_artifacts.h/.cpp` parses the package and bridge structurally using the
`nlohmann_json` library already linked to the emulator target. No new dependency was added.

It distinguishes:

| Problem | Meaning |
| --- | --- |
| `missing_object` | `index_binding` is absent |
| `missing_field` | `guest_base` is absent from the object |
| `wrong_type` | the value is not an integer |
| `malformed_number` | negative, fractional or out of range |
| `duplicate_key` | an identity-critical key appears twice |
| `unreadable` | the file is absent or not valid JSON |

A zero address is reported as present-but-zero, not as absent, so the two are distinguishable.

### Bridge record selection

`SelectBridgeRecord` matches on session and provenance. A duplicate match is reported as ambiguous
rather than resolved by array position. A record from another session is skipped even when the
provenance id agrees.

The replay-side reader now clears its output on every failure path, so a caller that ignores the
return value cannot consume records from a sidecar the reader rejected.

### Package-selected provenance drives the tape

`SemanticTape::ExportTape()` reads the package's `provenance_id` and exports that original command.
It no longer defaults to the last recorded draw when a package exists. A manual
`SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` is honoured only when no package exists; a conflict is logged
as `DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT` and the package wins.

The semantic arguments still come from the original command observer. Nothing is regenerated from
package metadata.

### Manifest reports selection and verification separately

`session.json` moves to schema 2 and records `candidate_selected`, `identity_verified`, `outcome`,
`verification`, the three provenance ids, the original object, both addresses, the package schema
version and the rejection reason.

A manifest with `candidate_selected: true` and `identity_verified: false` means the draw was found
but its resource identity was not established. That is not a success.

### Predicate determinability is enforced in the replay

A bridge whose address-slot predicate was not determinable cannot establish which slot the draw
consumed, so its address is not evidence. The replay now rejects this before the join, naming the
actual cause.

## 3. Test commands and results

```powershell
python tools/test_rhi02ir3_selection.py .\build-geometry-replay-rel\deracine_tape_replay.exe
python tools/test_rhi02ir2_integration.py .\build-geometry-replay-rel\deracine_tape_replay.exe
.\build-tests\tests\shadps4_capture_artifacts_test.exe
.\build-tests\tests\shadps4_draw_identity_test.exe
.\build-tests\tests\shadps4_index_bridge_test.exe
.\build-tests\tests\shadps4_capture_session_test.exe
.\build-tests\tests\shadps4_slot_predicate_test.exe
.\build-tests\tests\shadps4_bridge_production_test.exe
```

| Suite | Result |
| --- | --- |
| `test_rhi02ir3_selection.py` | 20/20 |
| `test_rhi02ir2_integration.py` | 19/19 |
| `shadps4_capture_artifacts_test` | 16 passed |
| `shadps4_draw_identity_test` | 24 passed |
| `shadps4_index_bridge_test` | 16 passed |
| `shadps4_capture_session_test` | 10 passed |
| `shadps4_slot_predicate_test` | 13 passed |
| `shadps4_bridge_production_test` | 16 passed |

## 4. Rejected cases and exit statuses

| Case | Exit |
| --- | --- |
| Vertex and index addresses differ | 0 |
| Multiple vertex bindings precede the index | 0 |
| Index binding missing | 1 |
| Index address zero | 1 |
| Index address disagrees with the bridge | 1 |
| Bridge has multiple records, selected is not first | 0 |
| Duplicate matching bridge records | 1 |
| Package selects 40, last draw is 58 | 0 |
| Original command missing | 1 |
| Bridge disagrees on the original object | 1 |
| Tape and package differ in session | 1 |
| Tape provenance differs from package | 1 |
| Correct selection, wrong object | 1 |
| Correctly selected fixture | 0 |
| Unrelated draw is not promoted | 1 |
| Predicate not determinable | 1 |
| Unconfirmed generation | 1 |
| Invalid index bounds | 1 |
| Missing bridge session | 1 |
| Missing package session | 1 |

## 5. Negative control

`CaptureArtifacts.TheLegacyGlobalSearchSelectsTheVertexAddress` reproduces the old algorithm and
proves it returns the vertex address from the same document the structural reader reads correctly.
If that test ever fails, the fixture no longer reproduces the original defect and the regression
coverage has been lost.

## 6. Schema changes

| Artifact | Old | New | Change |
| --- | --- | --- | --- |
| `session.json` | 1 | 2 | adds `selection` with separate selection and verification fields |
| `package.json` | 3 | 3 | unchanged |
| `tape.json` | 2 | 2 | unchanged |
| `index_bridge.json` | 1 | 1 | unchanged |

## 7. RHI-03 integration

See `docs/research/RHI02IR3_RHI03_HANDOFF.md`. No schema conflict. `tools/geometry_replay/package_reader.cpp/h`
was not modified, because RHI-03 owns its schema-4 consolidation.

## 8. Remaining original-game evidence gaps

**Original Déraciné same-draw verification — pending.** No game launch. The production path is
wired and fail-closed, but no live record exists.

**Allocation size, offset and generation remain unrecovered.** The creation path is the next static
target. An address match is the strongest identity available, and a reused address cannot be
distinguished from a genuine one.

**The address-offset domain is unresolved.** Whether `index_binding.guest_base` is the allocation
base, a view, or the first fetched index address has not been proven. The matcher compares base
addresses and validates the range, but does not add or subtract an unproven offset.

## 9. Scope

No game launch, no foregrounded process, no desktop capture, no SDK installation, no merge into
`main` or RHI-03. No shader, Vulkan or resource-lifetime research was reopened.