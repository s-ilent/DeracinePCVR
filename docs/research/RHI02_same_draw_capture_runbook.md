# RHI-02: single-execution same-draw capture runbook

## Status

**Prepared, not executed.** This runbook requires explicit authorisation to launch the emulator. No
part of it has been run.

It resolves the RHI-02 proof obligation: that a captured UE4 semantic command and its corresponding
resource package describe the same draw, established from **one** game execution.

## Why the previous design was unsound

The earlier runbook captured a resource package in one launch, read its `provenance_id`, then
launched the game again to export a tape pinned to that id.

A provenance id is a per-execution record ordinal. The same id in a second process names a different
draw. The two-launch design therefore could not establish that the tape and the package described
the same execution, however carefully the id was copied.

It is replaced by a single execution in which both observers run, every artifact carries a shared
session id, and the compatible pair is selected at export time.

## The identity problem this runbook must solve

The two sides record the index resource in different namespaces:

| Side | Value | Namespace |
| --- | --- | --- |
| Tape | `rhi_index_resource` from the RHI invocation argument | original UE4 `FRHIIndexBuffer` object pointer |
| Package | `index_binding.guest_base` | guest GPU virtual address |

Their numeric equality is not evidence. The bridge that resolves one into the other is recovered
from the indexed-draw implementation at `0x0CBB9C0` and is described in
`reports/RHI02I_original_index_resource_bridge.md`.

## Prerequisites

| Item | Value |
| --- | --- |
| Emulator build | `build/shadPS4.exe` from this branch |
| Approved game image | the 1.00 SELF, identified by hash |
| Game SHA-256 | `8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0` |
| Title id | `CUSA11919` |

**Do not use the `CUSA11919-qual` image.** It hashes to
`8DA8E46AC5FDC48E4E9B59E459920A266039993DC13A2D92740F8803E46FDF75`, which is not the approved
1.00 image. Static addresses and runtime observations from different builds must not be joined.

Verify before running:

```powershell
(Get-FileHash "<approved eboot.bin>" -Algorithm SHA256).Hash
# must be 8CAEE78E40693B2E57C68CE87CE9507D907B824B6F6A4C481F414AC0627011A0
```

## One execution, all observers

Every flag below is opt-in and default OFF. All of them run in the same process.

```powershell
# A fresh output directory. Never reuse an existing capture.
$out = "M:\Development\Deracine-work\_build\rhi02i-capture-<timestamp>"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# Provenance: the draw identity both observers key on.
$env:SHADPS4_DERACINE_RHI_PROBE = "1"
$env:SHADPS4_DERACINE_RHI_PROBE_AFTER = "33"
$env:SHADPS4_DERACINE_DRAW_PROVENANCE = "1"

# The original index-resource bridge. This is the new observer.
$env:SHADPS4_DERACINE_INDEX_IDENTITY = "1"

# Geometry and shader parameters.
$env:SHADPS4_DERACINE_VERTEX_INPUT = "1"
$env:SHADPS4_DERACINE_SHADER_PARAMS = "1"

# The resource package.
$env:SHADPS4_DERACINE_DRAW_PACKAGE = $out

# The semantic command capture.
$env:SHADPS4_DERACINE_CMD_CAPTURE = "1"
$env:SHADPS4_DERACINE_CMD_CAPTURE_AFTER = "33"

# Bounded run.
$env:SHADPS4_EXIT_AFTER = "75"
$env:SHADPS4_CLEAN_EXIT = "1"
```

## What each observer records

| Observer | Artifact | Identity it carries |
| --- | --- | --- |
| RHI entry probe | provenance records | `provenance_id`, execution id |
| Index identity | `DERACINE_INDEX_IDENTITY` lines | `execution_id`, `provenance_id`, object, resolved guest base |
| Command capture | `DERACINE_CMD_CAPTURE` lines | command list, payload, capture sequence |
| Resource package | `package.json` | `provenance_id`, `index_binding.guest_base` |

## Reading the result

```powershell
Select-String -Path shad_log.txt -Pattern "DERACINE_INDEX_IDENTITY_SUMMARY"
Select-String -Path shad_log.txt -Pattern "DERACINE_INDEX_IDENTITY execution_id"
Select-String -Path shad_log.txt -Pattern "DERACINE_DRAW_PROVENANCE_SUMMARY"
```

The `execution_id` on every identity line must be the same value. Two different values mean the
log contains records from more than one run and must not be joined.

## Matching and validation

```powershell
# The package must validate on its own.
python tools/validate_draw_package.py $out

# The identity lines must show a resolved guest base for the selected draw.
Select-String -Path shad_log.txt -Pattern "DERACINE_INDEX_IDENTITY execution_id" |
    Where-Object { $_ -match "provenance_id=<selected>" }
```

A candidate is usable only when all of these hold:

1. The identity line for the selected `provenance_id` has `has_guest_base=1`.
2. Its `guest_base` equals the package's `index_binding.guest_base`.
3. Its `execution_id` equals the execution id on every other record used.
4. The package validates.

## Failing clearly

| Condition | Meaning | Action |
| --- | --- | --- |
| No `DERACINE_INDEX_IDENTITY` lines | the observer did not install | check the flag and the image hash |
| `has_guest_base=0` | the object did not resolve | the bridge is unresolved; do not claim a match |
| `guest_base` differs from the package | different resources | reject the pair |
| Two `execution_id` values | mixed runs | discard and re-capture |
| `DERACINE_INDEX_IDENTITY_SUMMARY records=0` | nothing was observed | check the start delay |

**Do not substitute another draw.** A failure artifact that names the unresolved edge is more
useful than a package and a tape that happen to validate separately.

## Artifact handling

The package and the log contain proprietary game-derived data. They stay in a private local
directory and are never committed. Only the schema, validator and documentation are committed.

## Stopping point

This runbook stops before launching the game. Launching Déraciné or shadPS4 requires explicit user
authorisation.
