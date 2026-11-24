# RHI-02: single-execution same-draw capture runbook

## Status

**Prepared, not executed.** This runbook requires explicit authorisation to launch the emulator. No
part of it has been run.

It resolves the RHI-02 proof obligation: that a captured UE4 semantic command and its corresponding
resource package describe the same draw, established from **one** game execution through a verified
original index-resource bridge.

## Why the previous design was unsound

The earlier runbook captured a resource package in one launch, read its `provenance_id`, then
launched the game again to export a tape pinned to that id.

A provenance id is a per-execution record ordinal. The same id in a second process names a different
draw. The two-launch design could not establish that the tape and the package described the same
execution, however carefully the id was copied.

It is replaced by a single execution in which every observer runs, all artifacts carry one session
id, and the compatible pair is selected at export time.

## The identity problem this runbook must solve

The two sides record the index resource in different namespaces:

| Side | Value | Namespace |
| --- | --- | --- |
| Tape | `rhi_index_resource` from the RHI invocation argument | original UE4 `FRHIIndexBuffer` object pointer |
| Package | `index_binding.guest_base` | guest GPU virtual address |

Their numeric equality is not evidence. The bridge that resolves one into the other is recovered
from the indexed-draw implementation at `0x0CBB9C0` and is described in
`reports/RHI02IR_production_identity_correction.md`.

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

## One execution, all observers

Every flag below is opt-in and default OFF. All of them run in the same process.

```powershell
# A fresh output directory. Never reuse an existing capture.
$out = "M:\Development\Deracine-work\_build\rhi02ir-capture-<timestamp>"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# The capture session. This generates the one session id every exporter records.
$env:SHADPS4_DERACINE_CAPTURE_SESSION = "1"
$env:SHADPS4_DERACINE_CAPTURE_SESSION_DIR = $out

# Provenance: the draw identity both observers key on.
$env:SHADPS4_DERACINE_RHI_PROBE = "1"
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = "33"
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = "1"

# The original index-resource bridge. This is the observer that resolves the UE4 object.
$env:SHADPS4_DERACINE_INDEX_IDENTITY = "1"

# Geometry and shader parameters.
$env:SHADPS4_DERACINE_VERTEX_INPUT = "1"
$env:SHADPS4_DERACINE_SHADER_PARAMS = "1"

# The resource package.
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out

# The semantic command capture.
$env:SHADPS4_DERACINE_CMD_CAPTURE = "1"
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = "33"

# The semantic tape, exported from the already observed command.
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = "1"
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR = $out

# Bounded run.
$env:SHADPS4_EXIT_AFTER = "75"
$env:SHADPS4_CLEAN_EXIT = "1"
```

## Expected files

| File | Written by |
| --- | --- |
| `session.json` | the capture session |
| `package.json`, `resources.bin` | the resource package exporter |
| `tape.json` | the semantic tape exporter |
| `index_bridge.json` | the index identity observer |
| `shad_log.txt` | the emulator log |

## What each observer records

| Observer | Artifact | Identity it carries |
| --- | --- | --- |
| Capture session | `session.json` | `session_id` |
| RHI entry probe | provenance records | `provenance_id`, execution id |
| Index identity | `index_bridge.json` | `session_id`, `execution_id`, `provenance_id`, object, selected address, both candidates, predicate state |
| Command capture | `DERACINE_CMD_CAPTURE` lines | command list, payload, capture sequence |
| Resource package | `package.json` | `provenance_id`, `index_binding.guest_base` |

## Verifying the shared session

```powershell
Get-Content "$out\session.json"
Select-String -Path "$out\shad_log.txt" -Pattern "DERACINE_INDEX_IDENTITY_SUMMARY"
Select-String -Path "$out\shad_log.txt" -Pattern "DERACINE_CAPTURE_SESSION_INSTALL"
```

The `session_id` in `session.json` must equal the one in the install line. A mismatch means the
directory contains artifacts from more than one run and must be discarded.

## Bridge-aware identity validation

```powershell
# The package must validate on its own.
python tools/validate_draw_package.py $out

# The bridge must name the selected draw and carry a selected address.
Get-Content "$out\index_bridge.json"
```

A candidate is usable only when all of these hold:

1. `index_bridge.json` has a record for the package's `provenance_id`.
2. That record has `has_selected_guest_address: true`.
3. Its `selected_guest_address` equals the package's `index_binding.guest_base`.
4. Its `original_rhi_object` equals the tape's `rhi_index_resource`.
5. Its `session_id` equals `session.json`'s.
6. Its `predicate_determinable` is `true`.
7. The package validates.

## Replay

```powershell
.\build-geometry-replay-rel\deracine_tape_replay.exe $out tape
```

The replay reads `index_bridge.json` and calls the bridge-aware matcher. It refuses original-mode
execution unless the bridge is complete and verified. A run without `--synthetic-fixture` that
cannot establish the bridge exits non-zero with `insufficient_evidence`.

## Failure classification

| Condition | Meaning | Action |
| --- | --- | --- |
| `bridge: missing` | no sidecar was written | check `SHADPS4_DERACINE_INDEX_IDENTITY` and the session directory |
| `has_selected_guest_address: false` | the predicate could not be evaluated | the address is unknown; do not claim a match |
| `predicate_determinable: false` | a predicate input was unreadable | the slot is unknown; do not claim a match |
| `selected_guest_address` differs from the package | different resources | reject the pair |
| `original_rhi_object` differs from the tape | the bridge resolved a different object | reject the pair |
| two `session_id` values | mixed runs | discard and re-capture |
| `identity: rejected` | positive disagreement | the replay refuses |
| `identity: insufficient_evidence` | no verified bridge | the replay refuses |

**Do not substitute another draw.** A failure artifact that names the unresolved edge is more
useful than a package and a tape that happen to validate separately.

## Cleanup

The package, tape, bridge and log contain proprietary game-derived data. They stay in the private
capture directory and are never committed. Delete the directory when the evidence has been
reviewed, or keep it outside the repository.

## Stopping point

This runbook stops before launching the game. Launching Déraciné or shadPS4 requires explicit user
authorisation.
