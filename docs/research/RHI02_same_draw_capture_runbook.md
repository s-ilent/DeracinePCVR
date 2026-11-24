# RHI-02: single-execution same-draw capture runbook

## Status

**Prepared, not executed.** This runbook requires explicit authorisation to launch the emulator. No
part of it has been run.

It resolves the RHI-02 proof obligation: that a captured UE4 semantic command and its corresponding
resource package describe the same draw, established from **one** game execution through a verified
original index-resource bridge.

## Prerequisites

| Item | Value |
| --- | --- |
| Emulator build | `build/shadPS4.exe` from this branch |
| Approved game image | the 1.00 SELF, identified by hash |
| Game SHA-256 | `8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0` |
| Title id | `CUSA11919` |

**Do not use the `CUSA11919-qual` image.** It hashes to
`8da8e46ac5fdc48e4e9b59e459920a266039993dc13a2d92740f8803e46fdf75`, which is not the approved
1.00 image. Static addresses and runtime observations from different builds must not be joined.

Verify before running:

```powershell
(Get-FileHash "<approved eboot.bin>" -Algorithm SHA256).Hash
# must be 8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0
```

The index observer refuses to install unless the RHI entry probe installed, and the probe installs
only after verifying that hash. Setting the index flag alone on an unapproved build does nothing.

## One bounded execution

Every flag below is opt-in and default OFF. All of them run in the same process.

```powershell
# A fresh private capture directory. Never reuse an existing capture.
$out = "M:\Development\Deracine-work\_build\rhi02ir2-capture-<timestamp>"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The capture session. This generates the one session id every exporter records.
$env:SHADPS4_DERACINE_CAPTURE_SESSION = "1"
$env:SHADPS4_DERACINE_CAPTURE_SESSION_DIR = $out

# Provenance: the draw identity both observers key on.
$env:SHADPS4_DERACINE_RHI_PROBE = "1"
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = "33"
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = "1"

# The original index-resource bridge.
$env:SHADPS4_DERACINE_INDEX_IDENTITY = "1"

# Geometry and shader parameters.
$env:SHADPS4_DERACINE_VERTEX_INPUT = "1"
$env:SHADPS4_DERACINE_SHADER_PARAMS = "1"

# The resource package.
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out

# The semantic command capture and tape.
$env:SHADPS4_DERACINE_CMD_CAPTURE = "1"
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = "33"
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = "1"
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR = $out

# Bounded run.
$env:SHADPS4_EXIT_AFTER = "75"
$env:SHADPS4_CLEAN_EXIT = "1"
```

## Expected artifacts

| File | Schema | Written by |
| --- | --- | --- |
| `session.json` | 1 | the capture session, last |
| `package.json` | 3 | the resource package exporter |
| `resources.bin` | — | the resource package exporter |
| `tape.json` | 2 | the semantic tape exporter |
| `index_bridge.json` | 1 | the index identity observer |
| `shad_log.txt` | — | the emulator log |

A package at schema 2 or a tape at schema 1 predates session provenance. It is readable for
diagnostic replay but cannot qualify as a verified original capture.

## Session finalization

`CaptureSession::FinalizeSelection()` runs **last**, after the package, tape and bridge are written.
It reads all three from disk, checks that they share one session and one provenance, and writes the
decision into `session.json`. The manifest is written last because it records a decision about
artifacts that must already exist.

## Inspecting the selection

```powershell
Get-Content "$out\session.json"
Select-String -Path "$out\shad_log.txt" -Pattern "DERACINE_CAPTURE_SELECTION"
```

The `selection.outcome` field is one of:

| Outcome | Meaning |
| --- | --- |
| `selected` | the artifacts share a session, a provenance and an address |
| `no_package` | the resource package was not written |
| `no_tape` | the semantic tape was not written |
| `no_bridge` | the index bridge sidecar was not written |
| `missing_session` | an artifact carries no session identity |
| `session_mismatch` | the artifacts were not written in one session |
| `provenance_mismatch` | the artifacts describe different draws |
| `bridge_unresolved` | the bridge did not resolve a guest address |
| `address_mismatch` | the resolved allocation and the package disagree |

## Bridge-aware validation

```powershell
python tools/validate_draw_package.py $out
Get-Content "$out\index_bridge.json"
```

A candidate is usable only when all of these hold:

1. `session.json` has `selection.selected: true`.
2. `session.json`'s `session_id` equals the one in `package.json`, `tape.json` and
   `index_bridge.json`.
3. The bridge record for the package's `provenance_id` has `has_selected_guest_address: true`.
4. Its `selected_guest_address` equals the package's `index_binding.guest_base`.
5. Its `original_rhi_object` equals the tape's index resource.
6. Its `predicate_determinable` is `true`.
7. The package validates.

## Replay

```powershell
.\build-geometry-replay-rel\deracine_tape_replay.exe $out tape
```

The replay reads all four artifacts and calls the bridge-aware matcher. It refuses original-mode
execution unless every check passes. A run without `--synthetic-fixture` that cannot establish the
bridge exits non-zero.

## Failure classification

| Condition | Meaning | Action |
| --- | --- | --- |
| `session: manifest_missing` | no `session.json` | check the session flags |
| `bridge: missing` | no sidecar | check `SHADPS4_DERACINE_INDEX_IDENTITY` |
| `has_selected_guest_address: false` | the predicate could not be evaluated | do not claim a match |
| `predicate_determinable: false` | a predicate input was unreadable | do not claim a match |
| `identity: mismatched` | a positive disagreement | the replay refuses |
| `identity: insufficient_evidence` | no verified bridge | the replay refuses |
| `identity rejected: ... different original object` | the bridge resolved another object | reject |
| `identity rejected: ... different sessions` | mixed runs | discard and re-capture |

**Do not substitute another draw.** A failure artifact that names the unresolved edge is more
useful than a package and a tape that happen to validate separately.

## Evidence retained for unresolved candidates

When the selection fails, `session.json` records the outcome and the reason, and the three
artifacts remain in the directory. Keep them: they name which edge failed, which is what a later
investigation needs.

## Cleanup

The package, tape, bridge and log contain proprietary game-derived data. They stay in the private
capture directory and are never committed. Delete the directory when the evidence has been
reviewed, or keep it outside the repository.

## Stopping point

This runbook stops before launching the game. Launching Déraciné or shadPS4 requires explicit user
authorisation.
