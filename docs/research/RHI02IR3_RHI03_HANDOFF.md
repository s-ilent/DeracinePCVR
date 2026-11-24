# RHI-02I-R3 to RHI-03 handoff

**Date:** 2026-11-25
**RHI-02I-R3 branch:** `research/rhi02ir3-capture-selection-20261125`
**Base:** `da66cd1b107d6b9d86dfeff523fbc540e8823f3b` (RHI-02I-R2)

RHI-03's worktree has not been modified.

## 1. What changed

| Area | Change |
| --- | --- |
| Package index address | read structurally from `index_binding`, never by text search |
| Bridge record | selected by session and provenance, not by array position |
| Tape selection | the package-selected provenance is authoritative |
| Manifest | schema 2, records selection and verification separately |
| New module | `src/src/core/capture_artifacts.h/.cpp` |

Package and tape schemas are unchanged from RHI-02I-R2 (3 and 2). The manifest moves to schema 2.

## 2. The defect this fixes

The previous finalizer searched the serialized package for the first `"guest_base"` and took it.
The package writes `vertex_bindings[]` before `index_binding`, and both carry that field name, so
the search returned a vertex-buffer address. That is a silent wrong answer, not a parse failure.

`capture_artifacts.cpp` parses the JSON structurally: it locates `index_binding` as an object and
reads `guest_base` from within it. It distinguishes a missing object, a missing field, a zero value,
a wrong type, a malformed number, an overflow and a duplicate identity-critical key.

## 3. Changed interfaces

### New: `Core::CaptureArtifacts`

```cpp
bool ReadPackageMetadata(const std::string& path, PackageMetadata& out,
                         FieldProblem& problem, std::string& error);
bool ReadBridgeRecords(const std::string& path, std::vector<BridgeRecord>& out,
                       FieldProblem& problem, std::string& error);
bool SelectBridgeRecord(const std::vector<BridgeRecord>& records, u64 session_id,
                        u64 provenance_id, BridgeRecord& out, bool& ambiguous,
                        std::string& error);
```

`PackageMetadata` carries `index.present`, `index.has_guest_base` and `index.guest_base` as separate
facts, so a zero address is distinguishable from an absent one.

### Changed: `SemanticTape::ExportTape()`

The package-selected provenance is now authoritative. A manual `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW`
is honoured only when no package exists, and a conflict is logged as
`DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT` with the package winning.

### Changed: `CaptureSession::FinalizeSelection()`

Reads the package and bridge structurally, selects the bridge record by session and provenance, and
writes a manifest that reports `candidate_selected` and `identity_verified` separately.

## 4. Finalization dependencies

`Emulator::Shutdown()` order:

1. command capture
2. provenance
3. vertex input
4. shader params (writes the package)
5. index identities
6. index bridge sidecar
7. semantic tape (reads the package to choose its draw)
8. `CaptureSession::FinalizeSelection()` (reads package, tape and bridge)

The tape must run after the package because it reads the package's provenance. The finalizer must
run after all three because it validates what they wrote.

## 5. Shared-reader requirements

RHI-03 owns `tools/geometry_replay/package_reader.cpp/h` for its schema-4 consolidation. This branch
does **not** modify those files.

If RHI-03 wants one reader, the semantic requirement is:

- the index address is read from the `index_binding` object, never from a text search
- a zero address is distinguishable from an absent one
- a duplicate identity-critical key is rejected
- a vertex binding is never consulted for index identity

`capture_artifacts.cpp` is a Core-side reader for the finalizer. It is not a replacement for the
replay-side package reader, and the two may be consolidated later.

## 6. Identity verification contract

Unchanged from RHI-02I-R2. The matcher still requires:

- the same non-zero session on tape, package and bridge
- the bridge's `original_rhi_object` equal to the tape's index resource, in the `Ue4Object` domain
- the bridge's selected address equal to the package's index address
- a determinable address-slot predicate
- a valid index range
- no unconfirmed generation

`JoinTapeToPackageWithBridge()` was not weakened.

## 7. New tests

| Suite | Cases |
| --- | --- |
| `shadps4_capture_artifacts_test` | 16 |
| `tools/test_rhi02ir3_selection.py` | 20 |

The artifacts suite includes a negative control that reproduces the old global search and proves it
returns the vertex address from the same document the structural reader reads correctly.

## 8. Merge guidance

No schema conflict. RHI-03's schema 4 must carry `session_id` with the same meaning as RHI-02I-R2
established. This branch adds no package or tape field.

The only merge risk is `capture_session.cpp`, which RHI-03 may also touch if it adds finalization
steps. Resolve by keeping the structural readers and the separate selection/verification reporting.

## 9. What this branch does not do

No shader, Vulkan or resource-lifetime work. No OpenXR. No merge into `main` or RHI-03.