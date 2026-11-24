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

```powershell
(Get-FileHash "<approved eboot.bin>" -Algorithm SHA256).Hash
# must be 8caee78e40693b2e57c68ce87ce9507d907b824b6f6a4c481f414ac0627011a0
```

The index observer refuses to install unless the RHI entry probe installed, and the probe installs
only after verifying that hash.

## One bounded execution

```powershell
$out = "M:\Development\Deracine-work\_build\rhi02ir3-capture-<timestamp>"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$env:SHADPS4_DERACINE_CAPTURE_SESSION = "1"
$env:SHADPS4_DERACINE_CAPTURE_SESSION_DIR = $out
$env:SHADPS4_DERACINE_RHI_PROBE = "1"
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = "33"
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = "1"
$env:SHADPS4_DERACINE_INDEX_IDENTITY = "1"
$env:SHADPS4_DERACINE_VERTEX_INPUT = "1"
$env:SHADPS4_DERACINE_SHADER_PARAMS = "1"
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out
$env:SHADPS4_DERACINE_CMD_CAPTURE = "1"
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = "33"
$env:SHADPS4_DERACINE_SEMANTIC_TAPE = "1"
$env:SHADPS4_DERACINE_SEMANTIC_TAPE_DIR = $out
$env:SHADPS4_EXIT_AFTER = "75"
$env:SHADPS4_CLEAN_EXIT = "1"
```

Do **not** set `SHADPS4_DERACINE_SEMANTIC_TAPE_DRAW` in a combined capture. The package selects the
draw, and a manual selection that conflicts with it is logged as
`DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT` and overridden. Manual selection is for standalone
diagnostic runs with no package.

## How the draw is selected

The two observers are independent, and the selection is explicit:

1. The **resource package** selects its own draw by geometry completeness. That is its existing
   policy and it is unchanged.
2. The **semantic tape** reads the package's `provenance_id` and exports the original UE4 command
   already captured for that id. It does not default to the last recorded draw when a package
   exists, because that would describe a different draw than the package.
3. The **finalizer** reads the package, tape and bridge from disk and verifies identity.

The semantic arguments come from the original command observer, never from package metadata.

## Expected artifacts

| File | Schema | Written by |
| --- | --- | --- |
| `session.json` | 2 | the finalizer, last |
| `package.json` | 3 | the resource package exporter |
| `resources.bin` | — | the resource package exporter |
| `tape.json` | 2 | the semantic tape exporter |
| `index_bridge.json` | 1 | the index identity observer |
| `shad_log.txt` | — | the emulator log |

## Inspecting the selection

```powershell
Get-Content "$out\session.json"
Select-String -Path "$out\shad_log.txt" -Pattern "DERACINE_CAPTURE_SELECTION"
Select-String -Path "$out\shad_log.txt" -Pattern "DERACINE_SEMANTIC_TAPE_SELECTED"
```

The manifest reports selection and verification separately:

| Field | Meaning |
| --- | --- |
| `candidate_selected` | a bridge record matched the package's session and provenance |
| `identity_verified` | the resolved address equals the package index address |
| `outcome` | `candidate_selected`, `insufficient_evidence`, `rejected` or `ambiguous` |
| `verification` | `identity_verified`, `insufficient_evidence`, `rejected` or `not_attempted` |
| `package_index_address` | read from `index_binding.guest_base`, never from a vertex binding |
| `bridge_selected_address` | the address the original predicate selected |
| `original_rhi_object` | the original UE4 object the bridge resolved |
| `reason` | the specific rejection reason |

A manifest with `candidate_selected: true` and `identity_verified: false` means the draw was found
but its resource identity was not established. That is not a success.

## Bridge-aware validation

```powershell
python tools/validate_draw_package.py $out
Get-Content "$out\index_bridge.json"
```

A candidate is usable only when all of these hold:

1. `session.json` has `selection.identity_verified: true`.
2. `session.json`'s `session_id` equals the one in `package.json`, `tape.json` and
   `index_bridge.json`.
3. `selection.package_provenance_id` equals `selection.tape_provenance_id`.
4. The bridge record for that provenance has `has_selected_guest_address: true`.
5. Its `selected_guest_address` equals `selection.package_index_address`.
6. Its `original_rhi_object` equals the tape's index resource.
7. Its `predicate_determinable` is `true`.
8. The package validates.

## Replay

```powershell
.\build-geometry-replay-rel\deracine_tape_replay.exe $out tape
```

The replay reads all four artifacts and calls the bridge-aware matcher. It refuses original-mode
execution unless every check passes.

## Failure classification

| Condition | Meaning | Action |
| --- | --- | --- |
| `outcome: rejected` | a positive disagreement | the replay refuses |
| `outcome: insufficient_evidence` | a required fact is missing | do not claim a match |
| `outcome: ambiguous` | more than one bridge record matches | discard and re-capture |
| `reason: the package carries no index binding` | the draw was not indexed | check the scene |
| `reason: the package index address is zero` | the address was not captured | do not claim a match |
| `reason: the address-slot predicate was not determinable` | a predicate input was unreadable | do not claim a match |
| `reason: the resolved index allocation and the package name different buffers` | different resources | reject |
| `DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT` | a manual selection conflicted | the package won; remove the flag |

**Do not substitute another draw.** A failure artifact that names the unresolved edge is more
useful than a package and a tape that happen to validate separately.

## Cleanup

The package, tape, bridge and log contain proprietary game-derived data. They stay in the private
capture directory and are never committed.

## Stopping point

This runbook stops before launching the game. Launching Déraciné or shadPS4 requires explicit user
authorisation.
