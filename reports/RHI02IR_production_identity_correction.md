# RHI-02I-R: production identity bridge and single-execution capture repair

## Result

The four defects found in source review are repaired. The observer now reproduces the game's own
thread-dependent slot predicate, the replay calls the bridge-aware matcher, the capture session is
generated once and serialized by every exporter, and a missing execution id can no longer be
accepted.

**No Déraciné runtime verification.** No game launch was performed. The static layout is consistent
and the production path is connected, but no live record has been produced.

| Item | Value |
| --- | --- |
| Base | `99073a06` (RHI-02I) |
| Slot predicate tests | 13 passing (new) |
| Production bridge tests | 16 passing (new) |
| Existing bridge tests | 16 passing |
| Existing selection tests | 10 passing |
| Existing identity tests | 24 passing |
| Runtime verification | **pending** |

## 1. The four defects, confirmed before repair

| # | Defect | Confirmed at |
| --- | --- | --- |
| 1 | first-nonzero slot selection | `rhi_entry_probe.cpp`, `if (have_a && address_a != 0)` |
| 2 | `SelectPair()` never called | no caller anywhere in `src/src` |
| 3 | replay calls `JoinTapeToPackage()` | `tape_replay.cpp:783` |
| 4 | zero execution id accepted | `capture_session_contract.h:130`, `!= 0 &&` guard |

Each was confirmed in the production call graph, not merely in a unit test.

## 2. Statically established layout

The slot predicate is recovered from `0xCBBCCE`-`0xCBBCF1`:

```
cbbcce:  cmpq  $0x0, 0x44ac748        ; global thread record
cbbcd6:  je    0xcbbcf3               ; null -> slot A
cbbcd8:  movl  0x44ac740, %eax        ; global flag
cbbcde:  testl %eax, %eax
cbbce0:  jne   0xcbbcf3               ; flag set -> slot A
cbbce2:  callq 0x33c6770              ; scePthreadGetthreadid
cbbce7:  movq  0x44ac748, %rcx        ; the record
cbbcee:  cmpl  0x44(%rcx), %eax       ; compare against record+0x44
cbbcf1:  jne   0xcbbcf9               ; differ -> slot B
cbbcf3:  addq  $0x20, %rbx            ; slot A
cbbcf9:  addq  $0x40, %rbx            ; slot B
```

So slot A is selected when the record is null, when the flag is non-zero, or when the current
guest thread id equals the id at `record+0x44`. Slot B is selected only when the record is
non-null, the flag is zero, and the ids differ.

`scePthreadGetthreadid` is `posix_pthread_getthreadid_np`, which returns `g_curthread->tid`. That is
exactly what `CurrentGuestThreadId()` reads, so the comparison is reproducible without a host
thread identity. This was verified in `pthread.cpp:369`, not assumed.

| Field | Offset | Evidence |
| --- | --- | --- |
| Interface pointer | argument 2 | static verified |
| Resource base | object − 0x68 | static verified |
| Address slot A | +0x20 | static verified |
| Address slot B | +0x40 | static verified |
| Element width | +0x88 | static verified |
| Thread record global | `0x44AC748` | static verified |
| Thread flag global | `0x44AC740` | static verified |
| Record id offset | +0x44 | static verified |
| Allocation size | — | **not recovered** |
| Allocation offset | — | **not recovered** |
| Resource generation | — | **not recovered** |

## 3. Observer implementation

`EvaluateSlotPredicate` implements the recovered rule and returns `Unknown` when any input is
missing. There is no fallback to a slot: an indeterminate predicate leaves the address absent, so
the record cannot become comparable.

The observer records both candidates separately (`address_candidate_a`, `address_candidate_b`) and
the selected address. A nonzero but unselected candidate is never promoted.

### The version gate

The index observer previously checked only module and title identity, while the hash check lived in
the RHI entry probe. It now requires `RhiEntryProbe::IsInstalled()`, which is set only after the
probe verified the exact approved executable hash. Setting the index flag alone on an unapproved
build does nothing.

## 4. Session identity

`Core::CaptureSession` generates one identity per execution, from a process-specific nonce mixed
with the clock. A wall-clock timestamp alone is not unique, so the nonce is what prevents two runs
starting in the same tick from colliding. A zero identity is reserved for "no session" and is never
produced.

It is initialised once in `linker.cpp`, before any observer captures data, and read by the identity
observer. No exporter generates its own.

## 5. Persisted bridge

`index_bridge.json` is written by the identity observer into the capture directory. It carries
session id, execution id, provenance id, the original object, the resource base, both address
candidates, the selected slot and address, the element width, the predicate state, and the evidence
class. Allocation size, offset and generation appear only when genuinely recovered.

The replay reads it through `index_bridge_reader.cpp`, which refuses a missing sidecar, an
unexpected version, a duplicate record, an empty record list, and a record missing a required field.

**The bridge cannot be reconstructed from the package.** The package carries a guest address;
deriving the bridge from it would compare the package with itself. This is why the sidecar exists.

## 6. Matcher integration

`tape_replay.cpp` now calls `JoinTapeToPackageWithBridge`. It also checks that the bridge's
`original_rhi_object` equals the tape's index resource before the join, and reports
`bridge_object_mismatch` when they differ.

The fail-closed gate is unchanged and correct: a `Mismatched` outcome is always fatal, a synthetic
fixture may execute only under the synthetic label, and an original-mode run without a verified
bridge exits non-zero with `insufficient_evidence`.

## 7. Tests

| Suite | Result |
| --- | --- |
| `shadps4_slot_predicate_test` | 13 passed (new) |
| `shadps4_bridge_production_test` | 16 passed (new) |
| `shadps4_index_bridge_test` | 16 passed |
| `shadps4_capture_session_test` | 10 passed |
| `shadps4_draw_identity_test` | 24 passed |

The predicate tests cover every required case: null record, nonzero flag, matching ids, differing
ids, missing flag, missing current id, missing recorded id, and an indeterminate predicate that must
never become a guess. They also assert the recovered offsets against the disassembly, so a change
without a matching disassembly change fails.

The production tests exercise the real sidecar format and the real matcher: a verified bridge joins,
a differing object is rejected, the predicate-selected slot is used rather than the first candidate,
a missing selected address is insufficient evidence, a different session is rejected, an
unconfirmed generation is insufficient evidence, an incompatible range is rejected, a synthetic tape
cannot become verified, a missing sidecar cannot produce a verified join, and an ambiguous provenance
is reported.

## 8. Runbook

`docs/research/RHI02_same_draw_capture_runbook.md` is corrected to match the implementation. It
now includes the session flags that were missing, the exact approved hash, the expected file set,
the shared-session verification, the bridge-aware validation command, the replay command, the
failure classification, and cleanup.

## 9. What remains

**Runtime verification — pending.** No game launch. The predicate is statically recovered and the
production path is connected, but no live record exists.

**Allocation size, offset and generation remain unrecovered.** The creation path is the next static
target. Until it is found, an address match is the strongest identity available, and a reused
address cannot be distinguished from a genuine one. The matcher reports this as insufficient
evidence rather than fabricating a generation.

## 10. Reproducible checks

```powershell
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_slot_predicate_test
pwsh -File _build\build.ps1 -Tests -Root <worktree> -Target shadps4_bridge_production_test
.\build-tests\tests\shadps4_slot_predicate_test.exe
.\build-tests\tests\shadps4_bridge_production_test.exe
```

## 11. Scope

No game launch, no foregrounded process, no desktop capture, no SDK installation, no merge into
`main`. The observer is read-only, version-gated and default OFF.