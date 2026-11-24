# RHI-02I-R2 to RHI-03 handoff

**Date:** 2026-11-25
**RHI-02I-R2 branch:** `research/rhi02ir2-session-provenance-20261125`
**Base:** `1f7a643904d27545d10e07fd87d263774e78d60a` (RHI-02I-R)

This document is a reconciliation plan. RHI-03's worktree has not been modified.

## 1. What changed

| Area | Change |
| --- | --- |
| Package schema | 2 → **3**, adds `session_id` |
| Tape schema | 1 → **2**, adds `session_id` |
| Bridge sidecar | unchanged schema 1, now carries `session_id` |
| Session manifest | now records the selection decision |
| Matcher | enforces session identity and original-object correspondence |
| Replay | reads all four artifacts, fail-closed |
| Capture | `FinalizeSelection()` runs last and records the decision |

## 2. Session fields and invariants

For verified-original execution:

```text
manifest.session_id
    == tape.session_id
    == package.session_id
    == bridge.session_id
    != 0
```

A matching session proves the artifacts claim one execution. It does **not** prove they describe
the same draw. The original-object-to-allocation bridge remains mandatory.

### Session ID versus execution ID

Both are retained, with distinct purposes:

| Field | Meaning | Where |
| --- | --- | --- |
| `session_id` | the capture session, generated once per emulator execution | all four artifacts |
| `execution_id` | the identity observer's own record of the same execution | bridge only |

`session_id` is the canonical cross-artifact identity. `execution_id` is retained because the
bridge already carried it and the matcher compares it against the package. The package does not
serialize an execution id, so the matcher treats a zero package execution id as not comparable
rather than substituting the manifest's value.

**RHI-03 should treat `session_id` as the cross-artifact identity.** If it wants a single field,
drop `execution_id` from the bridge and compare `session_id` only.

## 3. Files requiring integration

| File | Change |
| --- | --- |
| `src/src/core/draw_package.h` | `SchemaVersion` 3, `Package::session_id` |
| `src/src/core/draw_package.cpp` | writes `session_id` |
| `src/src/core/semantic_tape_contract.h` | `SchemaVersion` 2, `Tape::session_id` |
| `src/src/core/semantic_tape.cpp` | writes `session_id` |
| `src/src/core/capture_session.h/.cpp` | `FinalizeSelection()` |
| `src/src/core/shader_param_capture.cpp` | sets `package.session_id` |
| `src/src/emulator.cpp` | finalization runs after every exporter |
| `tools/geometry_replay/draw_identity.h/.cpp` | session and object checks |
| `tools/geometry_replay/tape.h/.cpp` | reads `session_id`, accepts legacy 1 |
| `tools/geometry_replay/package_reader.h/.cpp` | reads `session_id` |
| `tools/geometry_replay/tape_replay.cpp` | reads the manifest, fail-closed gate |

## 4. Export ordering

`Emulator::Shutdown()` now runs:

1. command capture
2. provenance
3. vertex input
4. shader params (writes the package)
5. index identities
6. index bridge sidecar
7. semantic tape
8. **`CaptureSession::FinalizeSelection()`** — reads the package, tape and bridge from disk and
   writes the manifest with the decision

The manifest is written last because it records a decision about artifacts that must already exist.
Writing it earlier would let it claim a selection that had not been made.

## 5. Required validator behaviour

A validator must reject:

- a package or tape whose schema predates session identity, in verified-original mode
- a missing or zero `session_id` on any artifact
- conflicting `session_id` values
- a bridge whose `original_rhi_object` differs from the tape's index resource
- a bridge whose selected address differs from the package's index binding
- a draw whose index range runs past the resolved allocation
- an ambiguous bridge record

Legacy schemas remain readable for diagnostic replay. They must never qualify as verified original
captures.

## 6. CLI changes

None. The replay's arguments are unchanged. It now reads `session.json` and `index_bridge.json` from
the capture directory in addition to `tape.json` and `package.json`.

## 7. Tests RHI-03 must rerun

```powershell
python tools/test_rhi02ir2_integration.py <replay-exe>   # 19 cases
.\build-tests\tests\shadps4_slot_predicate_test.exe      # 13
.\build-tests\tests\shadps4_bridge_production_test.exe   # 16
.\build-tests\tests\shadps4_index_bridge_test.exe        # 16
.\build-tests\tests\shadps4_capture_session_test.exe     # 10
.\build-tests\tests\shadps4_draw_identity_test.exe       # 24
```

## 8. Schema conflict with RHI-03's planned schema 4

RHI-03 plans a schema 4 for the original-shader pipeline. This branch introduces schema 3 with
`session_id`.

**Minimum migration:** RHI-03's schema 4 must carry `session_id` with the same meaning. If it also
carries the original-shader fields, the two are additive and no field changes meaning. Do not
redefine `session_id` as anything other than the capture session identity.

If RHI-03 has already written a schema 4 without `session_id`, it must add the field rather than
renumber, so a reader can accept both.

## 9. What this branch does not do

No original-shader pipeline work. No resource-lifetime research. No OpenXR. No merge into `main` or
RHI-03.