# RHI-02I-R2: end-to-end session provenance and fail-closed original-draw identity

## Result

The four acceptance blockers are closed. The original-object mismatch is now fatal inside the
matcher, all four artifacts carry one session identity, the replay validates every artifact before
GPU work, and the production capture path invokes a real selection that records its decision.

**Production wiring demonstrated offline.** The tests invoke the actual replay executable against
independently authored artifacts. **Original Déraciné same-draw verification is pending** — it
requires an authorised game capture. Synthetic artifacts do not prove original runtime correctness.

| Item | Value |
| --- | --- |
| Branch | `research/rhi02ir2-session-provenance-20261125` |
| Base | `1f7a643904d27545d10e07fd87d263774e78d60a` |
| Integration cases | 19/19 |
| Unit suites | 79 passing |
| Runtime verification | **pending** |

## 1. Source defects reproduced

| # | Defect | Confirmed at |
| --- | --- | --- |
| 1 | object mismatch logged, not fatal | `tape_replay.cpp`, `bridge_object_mismatch` then continue |
| 2 | `PackageIdentity.execution_id` unset | `tape_replay.cpp:792`, only `provenance_id` assigned |
| 3 | no session in tape or package | no `session_id` in `semantic_tape.cpp` or `draw_package.cpp` |
| 4 | `SelectPair()` never called | no caller in `src/src` |

## 2. Production changes

### Object mismatch is fatal in the matcher

The check moved from the caller into `JoinTapeToPackageWithBridge`, so a caller that forgets it
cannot obtain a join from a bridge that resolved a different object. The matcher reports
`Mismatched` with "the index bridge resolved a different original object than the tape named", and
the existing gate treats `Mismatched` as fatal.

The check is domain-aware: it is comparable only when the tape's index resource is in the
`Ue4Object` domain. A tape carrying a guest address has no original object to compare, so it is
`InsufficientEvidence` rather than a false match.

### Session identity in all four artifacts

| Artifact | Schema | Field |
| --- | --- | --- |
| `session.json` | 1 | `session_id` |
| `package.json` | **3** (was 2) | `session_id` |
| `tape.json` | **2** (was 1) | `session_id` |
| `index_bridge.json` | 1 | `session_id` |

Every exporter reads `CaptureSession::SessionId()`. No exporter generates its own.

The matcher enforces the invariant directly: a zero session on the tape, the package or the bridge
is `InsufficientEvidence`; a disagreement is `Mismatched`.

### Session ID versus execution ID

Both are retained with distinct purposes. `session_id` is the canonical cross-artifact identity.
`execution_id` is the identity observer's own record and is compared only against the bridge. The
package does not serialize an execution id, so the matcher treats a zero package execution id as
not comparable rather than substituting the manifest's value.

### Real selection in the production path

`CaptureSession::FinalizeSelection()` runs last in `Emulator::Shutdown()`, after the package, tape
and bridge are written. It reads all three from disk, checks session, provenance and address, and
writes the decision into `session.json`.

The manifest is written last because it records a decision about artifacts that must already exist.
Writing it earlier would let it claim a selection that had not been made.

### Replay validates all four artifacts

The replay reads `session.json`, `tape.json`, `package.json` and `index_bridge.json` before
constructing Vulkan resources. It checks supported versions, a shared non-zero session, matching
provenance, original-object identity, the deterministically selected slot, guest-resource
correspondence, element width, index ranges, ambiguity and positive disagreements.

A missing manifest session is fatal in original mode. A synthetic fixture is exempt because it is
explicitly not claiming original provenance.

## 3. Test commands and results

```powershell
python tools/test_rhi02ir2_integration.py .\build-geometry-replay-rel\deracine_tape_replay.exe
.\build-tests\tests\shadps4_draw_identity_test.exe
.\build-tests\tests\shadps4_index_bridge_test.exe
.\build-tests\tests\shadps4_capture_session_test.exe
.\build-tests\tests\shadps4_slot_predicate_test.exe
.\build-tests\tests\shadps4_bridge_production_test.exe
```

| Suite | Result |
| --- | --- |
| Integration (real executable) | 19/19 |
| `shadps4_draw_identity_test` | 24 passed |
| `shadps4_index_bridge_test` | 16 passed |
| `shadps4_capture_session_test` | 10 passed |
| `shadps4_slot_predicate_test` | 13 passed |
| `shadps4_bridge_production_test` | 16 passed |

## 4. Rejected cases and exit statuses

Every rejection returns exit 1 from the real executable.

| Case | Exit |
| --- | --- |
| Valid four-artifact set | 0 |
| Bridge names a different original object | 1 |
| Matching address, wrong original object | 1 |
| Missing tape session | 1 |
| Missing package session | 1 |
| Missing bridge session | 1 |
| Missing manifest session | 1 |
| Different tape session | 1 |
| Different package session | 1 |
| Different bridge session | 1 |
| Provenance reused across sessions | 1 |
| Bridge absent | 1 |
| Predicate selected B while A is nonzero | 0 (B is authoritative) |
| Incompatible resource address | 1 |
| Invalid index bounds | 1 |
| Legacy tape without session | 1 |
| Legacy package without session | 1 |
| Unconfirmed generation | 1 |
| Tape carries a guest address | 1 |

## 5. Schema changes

| Artifact | Old | New | Change |
| --- | --- | --- | --- |
| `package.json` | 2 | 3 | adds `session_id` |
| `tape.json` | 1 | 2 | adds `session_id` |

Legacy schemas remain readable for diagnostic replay. They never qualify as verified original
captures, because they carry no session provenance.

## 6. RHI-03 integration

See `docs/research/RHI02IR2_RHI03_HANDOFF.md`. The minimum migration is that RHI-03's schema 4 must
carry `session_id` with the same meaning. The two are additive; no field changes meaning.

## 7. Remaining original-game evidence gaps

**Original Déraciné same-draw verification — pending.** No game launch. The production path is
wired and fail-closed, but no live record exists.

**Allocation size, offset and generation remain unrecovered.** The creation path is the next static
target. Until it is found, an address match is the strongest identity available, and a reused
address cannot be distinguished from a genuine one. The matcher reports this as insufficient
evidence rather than fabricating a generation.

**The address-offset domain is unresolved.** The original RHI computes `StartIndex × index_width`.
Whether the package's `index_binding.guest_base` is the allocation base, a view, or the first
fetched index address has not been proven. The matcher compares the base addresses and validates
the range, but does not add or subtract an unproven offset to manufacture equality.

## 8. Scope

No game launch, no foregrounded process, no desktop capture, no SDK installation, no merge into
`main` or RHI-03. No shader, Vulkan or resource-layout research was reopened.